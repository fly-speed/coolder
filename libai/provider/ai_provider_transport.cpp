#include "stdafx.h"
#include "ai_provider_transport_internal.h"
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

namespace provider_detail
{

bool parse_http_url(
    const std::string &url, parsed_url_t &parsed, std::string &err)
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
			err =
			    "IPv6 AI provider hosts must be enclosed in brackets";
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
	HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0,
	    location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
	    "ROOT");
	if (store == NULL)
		return false;
	PCCERT_CONTEXT cert = NULL;
	while ((cert = CertEnumCertificatesInStore(store, cert)) != NULL) {
		DWORD size = 0;
		if (!CryptBinaryToStringA(cert->pbCertEncoded,
		        cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER, NULL,
		        &size) ||
		    size <= 1)
			continue;
		std::vector<char> pem(size);
		if (!CryptBinaryToStringA(cert->pbCertEncoded,
		        cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER, &pem[0],
		        &size) ||
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
	if (loaded)
		return loaded;
	err = "cannot load trusted certificates from Windows certificate store";
	return loaded;
}
#endif

bool configure_tls(
    acl::openssl_conf &conf, const std::string &host, std::string &err)
{
#ifdef _WIN32
	if (!load_windows_root_ca(conf, err))
		return false;
#else
	std::string ca_file;
	const char *candidates[] = { "/etc/ssl/certs/ca-certificates.crt",
		"/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem",
		"/etc/ssl/cert.pem" };
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]);
	     ++i) {
		if (!(access(candidates[i], R_OK) == 0))
			continue;
		ca_file = candidates[i];
		break;
	}
	const bool loaded = ca_file.empty() ?
	    conf.load_default_ca() :
	    conf.load_ca(ca_file.c_str(), NULL);
	if (!loaded) {
		err = "cannot initialize trusted CA certificates";
		return false;
	}
#endif
	if (conf.set_verify_host(host.c_str()))
		return true;
	err = "cannot initialize AI provider hostname verification";
	return false;
}

void add_auth_headers(acl::http_header &header,
    const provider_config_t &provider, const std::string &api_key)
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
			header.add_entry(
			    "OpenAI-Project", provider.openai_project.c_str());
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
    const std::string &url, const std::string &payload, acl::json &body,
    int &status, long long &latency_ms, std::string &err)
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
	acl::http_request request(
	    parsed.address.c_str(), connect_timeout, 120, true);
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
				error_body.assign(
				    response.c_str(), response.size());
			}
		}
		err = provider_client_t::describe_http_error(status, error_body,
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
	if (body.finish())
		return true;
	err = "AI provider returned invalid JSON";
	ai_log_error("provider.client", "parse-json-response", err);
	return false;
}

bool get_json(const provider_config_t &provider, const std::string &api_key,
    const std::string &url, acl::json &body, int &status, long long &latency_ms,
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
	acl::http_request request(parsed.address.c_str(),
	    provider_connect_timeout_seconds(), 120, true);
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
		err = provider_client_t::describe_http_error(status, error_body,
		    response_header(request, "x-request-id"),
		    response_header(request, "Retry-After"));
		return false;
	}
	if (!(!request.get_body(body) || !body.finish()))
		return true;
	err = "cannot read AI provider retrieve response";
	return false;
}

// Runs on the request's fiber scheduler, so it can wake a blocked header/body
// read without accessing or closing a socket from another OS thread.

} // namespace provider_detail

using namespace provider_detail;

std::shared_ptr<provider_transport_session_t>
provider_client_t::create_transport_session()
{
	return std::make_shared<provider_transport_session_t>();
}

} // namespace ai
} // namespace webcool
