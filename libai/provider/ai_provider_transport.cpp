#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include "fiber/fiber_base.h"
#include "../agent/ai_admin_policy.h"
#include "../common/ai_error_log.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <unistd.h>
#include <netdb.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

namespace webcool
{
namespace ai
{
// Owned by one sequential coding run on its worker. No cross-user/global pool.
class provider_transport_session_t {
public:
	std::string key;
	std::chrono::steady_clock::time_point released;
	std::unique_ptr<acl::openssl_conf> ssl;
	std::unique_ptr<acl::http_request> connection;
};

namespace provider_detail
{

bool parse_http_url(const std::string &url, parsed_url_t &parsed,
		    std::string &err)
{
	const std::string http_prefix = "http://";
	const std::string https_prefix = "https://";
	size_t prefix_size = 0;
	if (url.compare(0, https_prefix.size(), https_prefix) == 0) {
		parsed.use_ssl = true;
		prefix_size = https_prefix.size();
	} else if (url.compare(0, http_prefix.size(), http_prefix) == 0) {
		parsed.use_ssl = false;
		prefix_size = http_prefix.size();
	} else {
		err = "AI provider URL must use HTTP or HTTPS";
		return false;
	}
	const size_t slash = url.find('/', prefix_size);
	parsed.host = slash == std::string::npos ?
			      url.substr(prefix_size) :
			      url.substr(prefix_size, slash - prefix_size);
	if (parsed.host.empty() || parsed.host.find('@') != std::string::npos ||
	    parsed.host.find_first_of(" \t\r\n?#") != std::string::npos) {
		err = "AI provider URL has an invalid host";
		return false;
	}
	if (parsed.host[0] == '[') {
		const size_t close = parsed.host.find(']');
		if (close == std::string::npos ||
		    (close + 1 < parsed.host.size() &&
		     parsed.host[close + 1] != ':')) {
			err = "AI provider URL has an invalid IPv6 host";
			return false;
		}
		parsed.verify_host = parsed.host.substr(1, close - 1);
	} else {
		const size_t colon = parsed.host.find(':');
		if (colon != std::string::npos &&
		    parsed.host.find(':', colon + 1) != std::string::npos) {
			err = "IPv6 AI provider hosts must be enclosed in brackets";
			return false;
		}
		parsed.verify_host = colon == std::string::npos ?
					     parsed.host :
					     parsed.host.substr(0, colon);
	}
	if (parsed.verify_host.empty()) {
		err = "AI provider URL has an invalid host";
		return false;
	}
	parsed.address = parsed.host;
	if ((parsed.host[0] == '[' &&
	     parsed.host.find("]:") == std::string::npos) ||
	    (parsed.host[0] != '[' &&
	     parsed.host.find(':') == std::string::npos)) {
		parsed.address += parsed.use_ssl ? ":443" : ":80";
	}
	parsed.path = slash == std::string::npos ? "/" : url.substr(slash);
	return true;
}

#ifdef _WIN32
bool append_windows_root_store(DWORD location, FILE *out, size_t &count)
{
	HCERTSTORE store =
		CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0,
			      location | CERT_STORE_OPEN_EXISTING_FLAG |
				      CERT_STORE_READONLY_FLAG,
			      "ROOT");
	if (store == NULL)
		return false;
	PCCERT_CONTEXT cert = NULL;
	while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
		DWORD size = 0;
		if (!CryptBinaryToStringA(
			    cert->pbCertEncoded, cert->cbCertEncoded,
			    CRYPT_STRING_BASE64HEADER, NULL, &size) ||
		    size <= 1)
			continue;
		std::vector<char> pem(size);
		if (!CryptBinaryToStringA(
			    cert->pbCertEncoded, cert->cbCertEncoded,
			    CRYPT_STRING_BASE64HEADER, &pem[0], &size) ||
		    size <= 1)
			continue;
		if (fwrite(&pem[0], 1, size - 1, out) != size - 1 ||
		    fwrite("\r\n", 1, 2, out) != 2) {
			CertFreeCertificateContext(cert);
			CertCloseStore(store, 0);
			return false;
		}
		++count;
	}
	CertCloseStore(store, 0);
	return true;
}

bool load_windows_root_ca(acl::openssl_conf &conf, std::string &err)
{
	char temp_dir[MAX_PATH + 1] = { 0 };
	char temp_file[MAX_PATH + 1] = { 0 };
	const DWORD dir_len = GetTempPathA(MAX_PATH, temp_dir);
	if (dir_len == 0 || dir_len > MAX_PATH ||
	    GetTempFileNameA(temp_dir, "wca", 0, temp_file) == 0) {
		err = "cannot create temporary Windows CA bundle";
		return false;
	}
	FILE *out = fopen(temp_file, "wb");
	if (out == NULL) {
		DeleteFileA(temp_file);
		err = "cannot write temporary Windows CA bundle";
		return false;
	}
	size_t count = 0;
	const bool user_ok = append_windows_root_store(
		CERT_SYSTEM_STORE_CURRENT_USER, out, count);
	const bool machine_ok = append_windows_root_store(
		CERT_SYSTEM_STORE_LOCAL_MACHINE, out, count);
	const bool write_ok = fclose(out) == 0;
	const bool loaded = (user_ok || machine_ok) && write_ok && count > 0 &&
			    conf.load_ca(temp_file, NULL);
	DeleteFileA(temp_file);
	if (!loaded)
		err = "cannot load trusted certificates from Windows certificate store";
	return loaded;
}
#endif

bool configure_tls(acl::openssl_conf &conf, const std::string &host,
		   std::string &err)
{
#ifdef _WIN32
	if (!load_windows_root_ca(conf, err))
		return false;
#else
	std::string ca_file;
	const char *candidates[] = { "/etc/ssl/certs/ca-certificates.crt",
				     "/etc/pki/tls/certs/ca-bundle.crt",
				     "/etc/ssl/ca-bundle.pem",
				     "/etc/ssl/cert.pem" };
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
	     ++i) {
		if (access(candidates[i], R_OK) == 0) {
			ca_file = candidates[i];
			break;
		}
	}
	const bool loaded = ca_file.empty() ?
				    conf.load_default_ca() :
				    conf.load_ca(ca_file.c_str(), NULL);
	if (!loaded) {
		err = "cannot initialize trusted CA certificates";
		return false;
	}
#endif
	if (!conf.set_verify_host(host.c_str())) {
		err = "cannot initialize AI provider hostname verification";
		return false;
	}
	return true;
}

void add_auth_headers(acl::http_header &header,
		      const provider_config_t &provider,
		      const std::string &api_key)
{
	if (provider.protocol == "anthropic_messages") {
		header.add_entry("anthropic-version", "2023-06-01");
		if (!api_key.empty())
			header.add_entry("x-api-key", api_key.c_str());
	} else if (!api_key.empty() && provider.protocol == "gemini_native") {
		header.add_entry("x-goog-api-key", api_key.c_str());
	} else if (!api_key.empty()) {
		const std::string authorization = "Bearer " + api_key;
		header.add_entry("Authorization", authorization.c_str());
	}
	if (provider.protocol == "openai_responses" ||
	    provider.protocol == "openai_chat" ||
	    provider.protocol == "openai_compatible") {
		if (!qwen_responses_endpoint(provider) &&
		    !provider.openai_organization.empty()) {
			header.add_entry("OpenAI-Organization",
					 provider.openai_organization.c_str());
		}
		if (!qwen_responses_endpoint(provider) &&
		    !provider.openai_project.empty()) {
			header.add_entry("OpenAI-Project",
					 provider.openai_project.c_str());
		}
	}
	if (qwen_responses_endpoint(provider) && provider.qwen_session_cache &&
	    provider.responses_store &&
	    provider.response_state_mode != "stateless") {
		header.add_entry("x-dashscope-session-cache", "enable");
	}
}

// ACL returns NULL for a missing response header. Copy it immediately because
// the pointer is owned by the request object and must not escape its lifetime.
std::string response_header(acl::http_request &request, const char *name)
{
	const char *value = request.header_value(name);
	return value ? value : "";
}

int provider_connect_timeout_seconds()
{
	const unsigned long configured =
		ai_runtime_policy_get().provider_connect_timeout_seconds;
	return static_cast<int>(std::max(5UL, std::min(300UL, configured)));
}

bool post_json(const provider_config_t &provider, const std::string &api_key,
	       const std::string &url, const std::string &payload,
	       acl::json &body, int &status, long long &latency_ms,
	       std::string &err)
{
	parsed_url_t parsed;
	if (!parse_http_url(url, parsed, err))
		return false;
	std::unique_ptr<acl::openssl_conf> ssl;
	if (parsed.use_ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err))
			return false;
	}
	// Cloud gateways may compress JSON even when the application does not add an
	// explicit Accept-Encoding header. ACL decodes supported encodings before it
	// feeds the resulting bytes to the incremental JSON parser below.
	const int connect_timeout = provider_connect_timeout_seconds();
	acl::http_request request(parsed.address.c_str(), connect_timeout, 120,
				  true);
	if (ssl.get() != NULL) {
		request.set_ssl(ssl.get()).set_ssl_sni(
			parsed.verify_host.c_str());
	}
	acl::http_header &header = request.request_header();
	header.set_url(parsed.path.c_str())
		.set_host(parsed.host.c_str())
		.set_keep_alive(false)
		.set_content_type("application/json");
	add_auth_headers(header, provider, api_key);
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	if (!request.post(payload.data(), payload.size())) {
		// Capture ACL's fiber-local error immediately. Calling another ACL API can
		// overwrite it and previously left the UI with only an ambiguous strerror.
		const int transport_code = acl::last_error();
		const std::string transport_detail =
			bounded_transport_error(acl::last_serror(), 240);
		latency_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count();
		err = "AI provider request failed"
		      " (phase=connect-send-or-wait-response-headers"
		      ", connect_timeout_seconds=" +
		      std::to_string(connect_timeout) +
		      ", response_timeout_seconds=120"
		      ", elapsed_ms=" +
		      std::to_string(latency_ms) +
		      ", transport_error=" + std::to_string(transport_code);
		if (!transport_detail.empty())
			err += ": " + transport_detail;
		err += ")";
		ai_log_error("provider.client", "json-request", err);
		return false;
	}
	latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			     std::chrono::steady_clock::now() - started)
			     .count();
	status = request.http_status();
	if (status < 200 || status >= 300) {
		// Provider errors are intentionally retained as a small string: the error
		// formatter needs vendor-specific codes and messages, while an unexpectedly
		// large error page must not consume unbounded memory.
		std::string error_body;
		const long long length = request.body_length();
		if (length < 0 || length <= 256 * 1024) {
			acl::string response;
			if (request.get_body(response) &&
			    response.size() <= 256 * 1024) {
				error_body.assign(response.c_str(),
						  response.size());
			}
		}
		err = provider_client_t::describe_http_error(
			status, error_body,
			response_header(request, "x-request-id"),
			response_header(request, "Retry-After"),
			response_header(request, "x-ratelimit-reset-requests"),
			response_header(request, "x-ratelimit-reset-tokens"));
		ai_log_error("provider.client", "http-response", err);
		return false;
	}
	const long long length = request.body_length();
	if (length > static_cast<long long>(kMaxStreamWireBytes)) {
		err = "AI provider JSON response exceeds 32 MiB";
		ai_log_error("provider.client", "json-response-limit", err);
		return false;
	}
	// ACL feeds the HTTP body to json::update() in small chunks. This avoids the
	// previous full response string and the second allocation made while parsing
	// that string. finish() is still required to reject truncated/invalid JSON.
	if (!request.get_body(body)) {
		err = "cannot read AI provider response";
		ai_log_error("provider.client", "read-json-response", err);
		return false;
	}
	if (!body.finish()) {
		err = "AI provider returned invalid JSON";
		ai_log_error("provider.client", "parse-json-response", err);
		return false;
	}
	return true;
}

bool get_json(const provider_config_t &provider, const std::string &api_key,
	      const std::string &url, acl::json &body, int &status,
	      long long &latency_ms, std::string &err)
{
	parsed_url_t parsed;
	if (!parse_http_url(url, parsed, err))
		return false;
	std::unique_ptr<acl::openssl_conf> ssl;
	if (parsed.use_ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err))
			return false;
	}
	acl::http_request request(parsed.address.c_str(),
				  provider_connect_timeout_seconds(), 120,
				  true);
	if (ssl.get() != NULL) {
		request.set_ssl(ssl.get()).set_ssl_sni(
			parsed.verify_host.c_str());
	}
	acl::http_header &header = request.request_header();
	header.set_url(parsed.path.c_str())
		.set_host(parsed.host.c_str())
		.set_keep_alive(false)
		.set_content_type("application/json");
	add_auth_headers(header, provider, api_key);
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	if (!request.get()) {
		latency_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count();
		err = std::string("AI provider retrieve request failed: ") +
		      bounded_transport_error(acl::last_serror(), 240);
		return false;
	}
	latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			     std::chrono::steady_clock::now() - started)
			     .count();
	status = request.http_status();
	if (status < 200 || status >= 300) {
		std::string error_body;
		acl::string response;
		if (request.body_length() <= 256 * 1024 &&
		    request.get_body(response) &&
		    response.size() <= 256 * 1024) {
			error_body.assign(response.c_str(), response.size());
		}
		err = provider_client_t::describe_http_error(
			status, error_body,
			response_header(request, "x-request-id"),
			response_header(request, "Retry-After"));
		return false;
	}
	if (!request.get_body(body) || !body.finish()) {
		err = "cannot read AI provider retrieve response";
		return false;
	}
	return true;
}

// Runs on the request's fiber scheduler, so it can wake a blocked header/body
// read without accessing or closing a socket from another OS thread.
struct stream_watch_state_t {
	provider_stream_progress_t progress;
	std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	completion_stream_observer_t *observer;
	long long idle_ms, stall_ms, total_ms, reported_ms = -10000;
	bool done = false;
	std::string timeout;
	void tick(bool force = false)
	{
		progress.elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count();
		if (timeout.empty())
			timeout = provider_stream_deadline(progress, idle_ms,
							   stall_ms, total_ms);
		if (!timeout.empty())
			progress.phase = timeout;
		if (force || !timeout.empty() ||
		    progress.elapsed_ms - reported_ms >= 10000) {
			reported_ms = progress.elapsed_ms;
			observer->on_stream_progress(progress);
		}
	}
};
class stream_watch_t {
public:
	std::shared_ptr<stream_watch_state_t> state;
	stream_watch_t(completion_stream_observer_t &observer,
		       const ai_admin_policy_t &policy)
		: state(new stream_watch_state_t)
	{
		state->observer = &observer;
		state->idle_ms =
			policy.provider_stream_timeout_seconds * 1000LL;
		state->stall_ms =
			policy.provider_effective_output_timeout_seconds *
			1000LL;
		state->total_ms =
			policy.provider_request_timeout_seconds * 1000LL;
		state->tick(true);
		ACL_FIBER *worker = acl_fiber_running();
		if (worker) {
			const auto shared = state;
			acl::gofiber([shared, worker] {
				while (!shared->done) {
					acl::fiber::delay(1000);
					if (shared->done)
						break;
					shared->tick();
					if (!shared->done &&
					    !shared->timeout.empty()) {
						acl_fiber_kill(worker);
						break;
					}
				}
			});
		}
	}
	~stream_watch_t()
	{
		state->done = true;
	}
	bool expired(completion_result_t &result, long long &latency,
		     std::string &err)
	{
		state->tick();
		if (state->timeout.empty())
			return false;
		latency = state->progress.elapsed_ms;
		result.error_category = provider_error_timeout;
		result.retryable_error = false;
		err = "AI provider " + state->timeout +
		      " (elapsed_ms=" + std::to_string(latency) +
		      ", received_bytes=" +
		      std::to_string(state->progress.received_bytes) +
		      ", tool_argument_bytes=" +
		      std::to_string(state->progress.tool_argument_bytes) + ")";
		return true;
	}
};

// Expose connection establishment so transport diagnostics do not conflate
// name resolution, TCP, TLS and response-header waits.
class staged_http_request_t : public acl::http_request {
public:
	using acl::http_request::http_request;
	using acl::http_request::open;
};

namespace
{

// One streaming exchange owns its parsing/connection state. The run() scope
// still controls the watchdog and the connection-return guard lifetimes.
class stream_exchange_t {
public:
	stream_exchange_t(
		const provider_config_t &config, const std::string &key,
		const std::string &endpoint, const std::string &data,
		completion_result_t &output,
		completion_stream_observer_t &listener, int &http_status,
		long long &latency, std::string &error,
		const std::shared_ptr<provider_transport_session_t> &slot)
		: provider(config)
		, api_key(key)
		, url(endpoint)
		, payload(data)
		, result(output)
		, observer(listener)
		, status(http_status)
		, latency_ms(latency)
		, err(error)
		, session(slot)
	{
	}
	bool run();

private:
	void stage(const char *name);
	bool capture_transport_failure();
	bool send_request();
	bool process_stream_line(const std::string &line);
	bool prepare_connection();
	bool check_response_status(acl::http_request &request);
	bool read_stream_body(acl::http_request &request);
	bool finish_stream_body();

	const provider_config_t &provider;
	const std::string &api_key;
	const std::string &url;
	const std::string &payload;
	completion_result_t &result;
	completion_stream_observer_t &observer;
	int &status;
	long long &latency_ms;
	std::string &err;
	const std::shared_ptr<provider_transport_session_t> &session;
	parsed_url_t parsed;
	std::unique_ptr<acl::openssl_conf> ssl;
	std::unique_ptr<acl::http_request> connection;
	std::string connection_key;
	ai_admin_policy_t policy;
	int connect_timeout = 0;
	unsigned long configured_timeout = 0;
	int stream_timeout = 0;
	stream_watch_t *watch = NULL;
	std::chrono::steady_clock::time_point started;
	std::string phase;
	std::string timings;
	int dns_code = 0;
	int connect_error_code = 0;
	std::string connect_error_detail;
	bool tls_timed_out = false;
	std::vector<streamed_tool_call_t> streamed_calls;
	size_t received = 0;
	std::chrono::steady_clock::time_point last_body_data_at;
	std::string raw_body;
	bool buffered_json_candidate = true;
	bool buffered_json_too_large = false;
	std::string pending_line;
	stream_diagnostics_t diagnostics;
};

void stream_exchange_t::stage(const char *name)
{
	if (!phase.empty())
		timings += ", " + phase + "_end_ms=" +
			   std::to_string(
				   std::chrono::duration_cast<
					   std::chrono::milliseconds>(
					   std::chrono::steady_clock::now() -
					   started)
					   .count());
	phase = name;
	watch->state->progress.phase = name;
	watch->state->tick(true);
}

bool stream_exchange_t::capture_transport_failure()
{
	connect_error_code = acl::last_error();
	connect_error_detail = bounded_transport_error(acl::last_serror(), 240);
	return false;
}

bool stream_exchange_t::send_request()
{
	if (observer.cancel_requested())
		return false;
	if (!connection) {
		stage("dns");
		addrinfo hints = {}, *addresses = NULL;
		hints.ai_socktype = SOCK_STREAM;
		const std::string port =
			parsed.address.substr(parsed.address.rfind(':') + 1);
		dns_code = getaddrinfo(parsed.verify_host.c_str(), port.c_str(),
				       &hints, &addresses);
		std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>
			address_guard(addresses, freeaddrinfo);
		if (dns_code != 0 || !addresses)
			return capture_transport_failure();
		stage("tcp_connect");
		const auto tcp_started = std::chrono::steady_clock::now();
		for (addrinfo *entry = addresses; entry;
		     entry = entry->ai_next) {
			char host[NI_MAXHOST];
			if (getnameinfo(
				    entry->ai_addr,
				    static_cast<socklen_t>(entry->ai_addrlen),
				    host, sizeof(host), NULL, 0,
				    NI_NUMERICHOST) != 0)
				continue;
			const auto used =
				std::chrono::duration_cast<
					std::chrono::seconds>(
					std::chrono::steady_clock::now() -
					tcp_started)
					.count();
			if (used >= connect_timeout ||
			    observer.cancel_requested())
				break;
			const std::string address =
				std::string(entry->ai_family == AF_INET6 ? "[" :
									   "") +
				host +
				(entry->ai_family == AF_INET6 ? "]:" : ":") +
				port;
			std::unique_ptr<staged_http_request_t> candidate(new staged_http_request_t(
				address.c_str(),
				connect_timeout - static_cast<int>(used),
				static_cast<int>(std::min(
					configured_timeout,
					std::min(
						policy.provider_effective_output_timeout_seconds,
						policy.provider_request_timeout_seconds))),
				true));
			if (candidate->open()) {
				connection = std::move(candidate);
				break;
			}
			capture_transport_failure(); // Capture errno before socket cleanup.
			connection.reset();
		}
		if (!connection)
			return false;
		if (ssl) {
			stage("tls_handshake");
			connection->get_client()->get_stream().set_rw_timeout(
				connect_timeout);
			const auto handshake_started =
				std::chrono::steady_clock::now();
			acl::sslbase_io *tls = ssl->create(false);
			tls->set_sni_host(parsed.verify_host.c_str());
			if (connection->get_client()->get_stream().setup_hook(
				    tls) == tls) {
				capture_transport_failure();
				// SO_RCVTIMEO may report EAGAIN instead of ETIMEDOUT. Only
				// normalize it when the configured handshake budget elapsed.
				tls_timed_out =
					temporary_resource_transport_error(
						connect_error_detail) &&
					std::chrono::duration_cast<
						std::chrono::milliseconds>(
						std::chrono::steady_clock::
							now() -
						handshake_started)
							.count() >=
						connect_timeout * 1000LL - 100;
				tls->destroy();
				return false;
			}
		}
	} else {
		stage("connection_reused");
		connection->get_client()->reset();
		connection->request_header().reset();
	}
	const int read_timeout = static_cast<int>(std::min(
		configured_timeout,
		std::min(policy.provider_effective_output_timeout_seconds,
			 policy.provider_request_timeout_seconds)));
	// OpenSSL installs socket-level timers during setup_hook. Update both
	// those timers and ACL's stream timer after the handshake; changing just
	// rw_timeout would leave the shorter handshake timer on subsequent reads.
	if (ssl && !connection->get_client()->get_stream().set_rw_timeout(
			   read_timeout, true))
		return capture_transport_failure();
	connection->get_client()->get_stream().set_rw_timeout(read_timeout);
	stage("sending_request");
	acl::http_header &header = connection->request_header();
	header.set_url(parsed.path.c_str())
		.set_host(parsed.host.c_str())
		.set_keep_alive(session != NULL)
		.set_content_type("application/json")
		.set_method(acl::HTTP_METHOD_POST)
		.set_content_length(payload.size());
	header.add_entry("Accept", "text/event-stream");
	add_auth_headers(header, provider, api_key);
	// Use the client primitives to avoid http_request's hidden reconnect
	// replay of a POST on what it considers an already-open connection.
	acl::http_client *client = connection->get_client();
	if (!client->write_head(header) ||
	    !client->write_body(payload.data(), payload.size()))
		return capture_transport_failure();
	stage("waiting_headers");
	return client->read_head() || capture_transport_failure();
}

bool stream_exchange_t::process_stream_line(const std::string &line)
{
	std::string delta;
	std::string reasoning_delta;
	if (!parse_stream_line(provider, line, result, streamed_calls, delta,
			       reasoning_delta, diagnostics, err))
		return false;
	if (streamed_tool_argument_bytes(streamed_calls) >
	    kMaxToolArgumentsBytes) {
		err = "AI provider tool arguments exceed 512 KiB";
		ai_log_error("provider.client", "tool-arguments-limit", err);
		return false;
	}
	const size_t argument_bytes =
		streamed_tool_argument_bytes(streamed_calls);
	if (!delta.empty() || !reasoning_delta.empty() ||
	    argument_bytes > watch->state->progress.tool_argument_bytes) {
		watch->state->tick();
		watch->state->progress.last_effective_ms =
			watch->state->progress.elapsed_ms;
		watch->state->progress.text_bytes += delta.size();
		watch->state->progress.reasoning_bytes +=
			reasoning_delta.size();
		watch->state->progress.tool_argument_bytes = argument_bytes;
		const std::string stream_phase =
			argument_bytes ? "receiving_tool_arguments" :
			!delta.empty() ? "receiving_text" :
					 "receiving_reasoning";
		const bool changed =
			stream_phase != watch->state->progress.phase;
		watch->state->progress.phase = stream_phase;
		watch->state->tick(changed);
	}
	if (!reasoning_delta.empty()) {
		if (result.reasoning.size() + reasoning_delta.size() >
		    kMaxCompletionBytes) {
			err = "AI provider reasoning exceeds 4 MiB";
			ai_log_error("provider.client",
				     "reasoning-output-limit", err);
			return false;
		}
		result.reasoning += reasoning_delta;
		if (!observer.on_reasoning_delta(reasoning_delta)) {
			err = "AI provider streaming response cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
	}
	if (delta.empty())
		return true;
	if (result.text.size() + delta.size() > kMaxCompletionBytes) {
		err = "AI provider response exceeds 4 MiB";
		return false;
	}
	result.text += delta;
	if (!observer.on_text_delta(delta)) {
		err = "AI provider streaming response cancelled";
		result.error_category = provider_error_cancelled;
		return false;
	}
	return true;
}

bool stream_exchange_t::prepare_connection()
{
	if (!parse_http_url(url, parsed, err))
		return false;
	connection_key = provider_scheduler_key(provider, api_key) + "\n" + url;
	if (session && session->connection) {
		if (session->key == connection_key &&
		    std::chrono::steady_clock::now() - session->released <
			    std::chrono::seconds(30) &&
		    session->connection->get_client()->get_stream().alive()) {
			ssl = std::move(session->ssl);
			connection = std::move(session->connection);
			result.connection_reused = true;
		} else {
			session->connection.reset();
			session->ssl.reset();
		}
	}
	if (parsed.use_ssl && !ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err))
			return false;
	}
	return true;
}

bool stream_exchange_t::check_response_status(acl::http_request &request)
{
	if (watch->expired(result, latency_ms, err))
		return false;
	watch->state->progress.headers_ms = watch->state->progress.elapsed_ms;
	watch->state->progress.phase = "waiting_first_byte";
	watch->state->tick(true);
	status = request.http_status();
	if (observer.cancel_requested()) {
		err = "AI provider streaming request cancelled";
		result.error_category = provider_error_cancelled;
		return false;
	}
	if (status < 200 || status >= 300) {
		// Error responses are ordinary JSON rather than SSE on the providers we
		// support. Read the bounded body now so the UI can show the vendor's error
		// code and message instead of losing everything except the HTTP status.
		std::string error_body;
		const long long length = request.body_length();
		if (length < 0 || length <= 256 * 1024) {
			acl::string response;
			if (request.get_body(response) &&
			    response.size() <= 256 * 1024) {
				error_body.assign(response.c_str(),
						  response.size());
			}
		}
		err = provider_client_t::describe_http_error(
			status, error_body,
			response_header(request, "x-request-id"),
			response_header(request, "Retry-After"),
			response_header(request, "x-ratelimit-reset-requests"),
			response_header(request, "x-ratelimit-reset-tokens"));
		ai_log_error("provider.client", "stream-http-response", err);
		// Preserve diagnostic bytes before returning; previously only the parsed
		// message survived, making unknown gateway schemas impossible to inspect.
		// Redact before truncation so a key crossing the limit cannot leak.
		if (!api_key.empty()) {
			for (size_t pos = error_body.find(api_key);
			     pos != std::string::npos;
			     pos = error_body.find(api_key, pos + 10))
				error_body.replace(pos, api_key.size(),
						   "[REDACTED]");
		}
		result.error_response_excerpt = error_body.substr(0, 8192);
		result.error_response_content_type =
			response_header(request, "Content-Type");
		result.error_response_content_encoding =
			response_header(request, "Content-Encoding");
		return false;
	}
	return true;
}

bool stream_exchange_t::read_stream_body(acl::http_request &request)
{
	while (!request.body_finish()) {
		if (watch->expired(result, latency_ms, err))
			return false;
		// Non-fiber callers also bound the next blocking read by the remaining
		// total and effective-output deadlines.
		const auto &progress = watch->state->progress;
		const long long remaining = std::min(
			watch->state->total_ms - progress.elapsed_ms,
			watch->state->stall_ms - (progress.elapsed_ms -
						  progress.last_effective_ms));
		request.get_client()->get_stream().set_rw_timeout(
			static_cast<int>(std::max(
				1LL,
				std::min(static_cast<long long>(stream_timeout),
					 (remaining + 999) / 1000))));
		if (observer.cancel_requested()) {
			err = "AI provider streaming response cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		acl::string chunk;
		const int got = request.read_body(chunk, true);
		if (watch->expired(result, latency_ms, err))
			return false;
		if (got < 0) {
			// Preserve bounded transport diagnostics without exposing the endpoint,
			// request payload, API key, or provider response content. These counters
			// make timeout, proxy reset and truncated SSE failures distinguishable in
			// both the ACL log and the WebCool error panel.
			const int transport_code = acl::last_error();
			const std::string transport_detail =
				bounded_transport_error(acl::last_serror(),
							240);
			err = "cannot read AI provider streaming response"
			      " (http_status=" +
			      std::to_string(status) +
			      ", stream_idle_timeout_seconds=" +
			      std::to_string(stream_timeout) +
			      ", received_bytes=" + std::to_string(received) +
			      ", events=" +
			      std::to_string(diagnostics.event_count) +
			      ", elapsed_ms=" +
			      std::to_string(
				      std::chrono::duration_cast<
					      std::chrono::milliseconds>(
					      std::chrono::steady_clock::now() -
					      started)
					      .count()) +
			      ", idle_ms=" +
			      std::to_string(
				      std::chrono::duration_cast<
					      std::chrono::milliseconds>(
					      std::chrono::steady_clock::now() -
					      last_body_data_at)
					      .count()) +
			      ", tool_argument_bytes=" +
			      std::to_string(streamed_tool_argument_bytes(
				      streamed_calls)) +
			      ", tool_call_fragments=" +
			      std::to_string(streamed_calls.size()) +
			      ", reasoning_bytes=" +
			      std::to_string(diagnostics.reasoning_bytes) +
			      ", visible_text_bytes=" +
			      std::to_string(result.text.size()) +
			      ", transport_error=" +
			      std::to_string(transport_code);
			if (!transport_detail.empty()) {
				err += ": " + transport_detail;
			}
			err += ")";
			result.http_status = status;
			result.error_category = provider_error_network;
			result.retryable_error = true;
			latency_ms = std::chrono::duration_cast<
					     std::chrono::milliseconds>(
					     std::chrono::steady_clock::now() -
					     started)
					     .count();
			ai_log_error("provider.client", "stream-body-read",
				     err);
			return false;
		}
		if (got == 0) {
			if (observer.cancel_requested()) {
				err = "AI provider streaming response cancelled";
				result.error_category =
					provider_error_cancelled;
				return false;
			}
			if (request.body_finish())
				break;
			continue;
		}
		last_body_data_at = std::chrono::steady_clock::now();
		received += chunk.size();
		const bool first_byte =
			watch->state->progress.first_byte_ms < 0;
		if (first_byte)
			watch->state->progress.first_byte_ms =
				watch->state->progress.elapsed_ms;
		watch->state->progress.last_data_ms =
			watch->state->progress.elapsed_ms;
		watch->state->progress.received_bytes = received;
		if (first_byte) {
			watch->state->progress.phase = "receiving_stream";
			watch->state->tick(true);
		}
		if (received > kMaxStreamWireBytes) {
			err = "AI provider streaming transport exceeds 32 MiB";
			ai_log_error("provider.client", "stream-wire-limit",
				     err);
			return false;
		}
		if (buffered_json_candidate) {
			if (raw_body.size() + chunk.size() <=
			    kMaxCompletionBytes) {
				raw_body.append(chunk.c_str(), chunk.size());
			} else {
				raw_body.clear();
				buffered_json_candidate = false;
				buffered_json_too_large = true;
			}
		}
		pending_line.append(chunk.c_str(), chunk.size());
		for (;;) {
			const size_t newline = pending_line.find('\n');
			if (newline == std::string::npos)
				break;
			std::string line = pending_line.substr(0, newline);
			pending_line.erase(0, newline + 1);
			if (!line.empty() && line[line.size() - 1] == '\r')
				line.resize(line.size() - 1);
			if (!process_stream_line(line))
				return false;
		}
		if (diagnostics.event_count > 0 && buffered_json_candidate) {
			// A parsed event proves this is a stream, so keeping a duplicate full
			// body for the ordinary-JSON fallback would only waste memory.
			raw_body.clear();
			buffered_json_candidate = false;
		}
		const bool sse_line =
			pending_line.compare(0, 5, "data:") == 0 ||
			pending_line.compare(0, 6, "event:") == 0 ||
			pending_line.compare(0, 1, ":") == 0;
		if ((sse_line && pending_line.size() > kMaxStreamEventBytes) ||
		    (!sse_line && pending_line.size() > kMaxCompletionBytes)) {
			err = sse_line ?
				      "AI provider streaming event exceeds 1 MiB" :
				      "AI provider response exceeds 4 MiB";
			ai_log_error("provider.client", "stream-event-limit",
				     err);
			return false;
		}
	}
	return true;
}

bool stream_exchange_t::finish_stream_body()
{
	if (!pending_line.empty() && !process_stream_line(pending_line))
		return false;
	if (!finalize_streamed_tool_calls(streamed_calls, result, err)) {
		// A syntactically incomplete tool call together with finish_reason=length
		// is deterministic truncation. Keep the partial write unexecutable, but
		// classify it as an output limit so recovery and UI diagnostics are honest.
		if (diagnostics.finish_reason == "length") {
			result.incomplete_reason = "max_output_tokens";
			result.error_category = provider_error_response_limit;
			err += ". The provider stopped at its output-token limit before the"
			       " tool call JSON was complete.";
		}
		return false;
	}
	latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			     std::chrono::steady_clock::now() - started)
			     .count();
	if (result.text.empty() && !result.native_tool_call) {
		if (buffered_json_too_large && diagnostics.event_count == 0) {
			err = "AI provider response exceeds 4 MiB";
			ai_log_error("provider.client",
				     "buffered-response-limit", err);
			return false;
		}
		completion_result_t buffered;
		std::string ignored;
		if (!raw_body.empty() &&
		    parse_completion(provider, raw_body, buffered, ignored)) {
			std::string buffered_text;
			std::string buffered_reasoning;
			adopt_completed_stream_result(buffered, result,
						      buffered_text,
						      buffered_reasoning);
			if (!buffered_reasoning.empty()) {
				result.reasoning = buffered_reasoning;
				if (!observer.on_reasoning_delta(
					    buffered_reasoning)) {
					err = "AI provider streaming response cancelled";
					result.error_category =
						provider_error_cancelled;
					return false;
				}
			}
			if (!buffered_text.empty()) {
				result.text = buffered_text;
				if (!observer.on_text_delta(buffered_text)) {
					err = "AI provider streaming response cancelled";
					result.error_category =
						provider_error_cancelled;
					return false;
				}
			}
			if (!result.text.empty() || result.native_tool_call)
				return true;
		}
		err = "AI provider streaming response contains no visible text or tool call"
		      " (protocol=" +
		      provider.protocol +
		      ", received_bytes=" + std::to_string(received) +
		      ", events=" + std::to_string(diagnostics.event_count);
		if (diagnostics.reasoning_bytes > 0) {
			err += ", reasoning_bytes=" +
			       std::to_string(diagnostics.reasoning_bytes);
		}
		if (!diagnostics.finish_reason.empty()) {
			err += ", finish_reason=" + diagnostics.finish_reason;
		}
		if (!diagnostics.last_event_type.empty()) {
			err += ", last_event_type=" +
			       diagnostics.last_event_type;
		}
		err += "). ";
		if (diagnostics.reasoning_bytes > 0 &&
		    diagnostics.finish_reason == "length") {
			err += "The model used the entire output-token budget for reasoning before"
			       " producing its answer.";
		} else {
			err += "The provider returned an unsupported event schema or produced no"
			       " answer.";
		}
		ai_log_error("provider.client", "empty-stream-response", err);
		return false;
	}
	return true;
}

bool stream_exchange_t::run()
{
	if (!prepare_connection())
		return false;
	// Return only fully consumed successful responses to this task's slot.
	// On any exception/error/cancellation the local owners close the transport.
	struct return_connection_t {
		std::shared_ptr<provider_transport_session_t> slot;
		std::unique_ptr<acl::openssl_conf> &tls;
		std::unique_ptr<acl::http_request> &request;
		const std::string &error;
		const int &status;
		std::string key;
		~return_connection_t()
		{
			if (slot && request && error.empty() && status >= 200 &&
			    status < 300 && request->body_finish() &&
			    request->get_client()->is_server_keep_alive() &&
			    !request->get_client()->disconnected()) {
				slot->connection.reset();
				slot->ssl = std::move(tls);
				slot->connection = std::move(request);
				slot->key = key;
				slot->released =
					std::chrono::steady_clock::now();
			}
		}
	} give_back{ session, ssl, connection, err, status, connection_key };
	// Streaming gateways can also compress the SSE/NDJSON byte stream. ACL must
	// expose decoded chunks to the protocol parser and byte-limit accounting.
	// ACL applies rw_timeout both while waiting for the response headers in
	// post() and while waiting for the next body fragment in read_body(). Use the
	// administrator's dedicated provider idle timeout: the previous hard-coded
	// 120 seconds incorrectly killed models that reason silently before the first
	// SSE event.
	policy = ai_runtime_policy_get();
	connect_timeout = static_cast<int>(std::max(
		5UL, std::min(300UL, policy.provider_connect_timeout_seconds)));
	configured_timeout = policy.provider_stream_timeout_seconds;
	stream_timeout = static_cast<int>(
		std::max(30UL, std::min(3600UL, configured_timeout)));
	stream_watch_t watch_lifetime(observer, policy);
	watch = &watch_lifetime;
	started = std::chrono::steady_clock::now();
	if (!send_request()) {
		if (watch->expired(result, latency_ms, err)) {
			err += " (transport_phase=" + phase + timings + ")";
			return false;
		}
		if (observer.cancel_requested()) {
			err = "AI provider streaming request cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		latency_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count();
		err = "AI provider streaming request failed (phase=" + phase +
		      ", connect_timeout_seconds=" +
		      std::to_string(connect_timeout) +
		      ", stream_idle_timeout_seconds=" +
		      std::to_string(stream_timeout) +
		      ", elapsed_ms=" + std::to_string(latency_ms) + timings +
		      ", connection_reused=" +
		      std::to_string(result.connection_reused) +
		      ", dns_error=" + std::to_string(dns_code) +
		      ", transport_error=" +
		      std::to_string(connect_error_code) + ": " +
		      connect_error_detail + ")";
		if (tls_timed_out)
			err += " TLS handshake timed out";
		ai_log_error("provider.client", "stream-request", err);
		return false;
	}
	acl::http_request &request = *connection;
	if (!check_response_status(request))
		return false;
	last_body_data_at = std::chrono::steady_clock::now();
	// Retain the already bounded response in memory so a server that ignores
	// stream=true and returns ordinary JSON can be parsed without a second API
	// request (and without charging the user for a duplicate completion).
	if (!read_stream_body(request))
		return false;
	return finish_stream_body();
}

} // namespace

bool post_json_stream(
	const provider_config_t &provider, const std::string &api_key,
	const std::string &url, const std::string &payload,
	completion_result_t &result, completion_stream_observer_t &observer,
	int &status, long long &latency_ms, std::string &err,
	const std::shared_ptr<provider_transport_session_t> &session)
{
	stream_exchange_t exchange(provider, api_key, url, payload, result,
				   observer, status, latency_ms, err, session);
	return exchange.run();
}

} // namespace provider_detail

using namespace provider_detail;

std::shared_ptr<provider_transport_session_t>
provider_client_t::create_transport_session()
{
	return std::make_shared<provider_transport_session_t>();
}

} // namespace ai
} // namespace webcool
