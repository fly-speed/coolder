#include "stdafx.h"
#include "../context/agent_context_limits.h"
#include "ai_provider_client_internal.h"
#include "fiber/fiber_base.h"
#include "../prompt/prompt_language.h"
#include "../common/ai_error_log.h"
#include "output_token_limit.h"
#include "provider_request_scheduler.h"

#include <chrono>
#include <memory>

namespace webcool
{
namespace ai
{
using namespace provider_detail;
bool provider_client_t::test_connection(const provider_config_t &provider,
    const std::string &api_key, provider_test_result_t &result,
    std::string &err)
{
	result.http_status = 0;
	result.latency_ms = 0;
	result.endpoint = test_url(provider);
	result.model_verified = false;
	result.responses_verified = false;
	result.tools_verified = false;
	if (provider.protocol == "openai_responses") {
		result.endpoint = completion_url(provider, false);
		// /models proves only authentication. A tiny, non-persisted native tool
		// request verifies the selected deployment, Responses endpoint, configured
		// reasoning controls and function-call schema before a real coding run.
		completion_request_t probe;
		probe.system_prompt =
		    "Return the required capability-probe function call.";
		probe.user_prompt = "Run the WebCool capability probe.";
		probe.max_output_tokens = 256;
		probe.reasoning_effort = "none";
		probe.reasoning_summary = "none";
		probe.text_verbosity = "low";
		probe.service_tier = provider.responses_service_tier;
		probe.prompt_cache_ttl = provider.responses_cache_ttl;
		probe.prompt_cache_key = "webcool-capability-probe";
		probe.safety_identifier = "webcool-capability-probe";
		probe.store = false;
		probe.background = false;
		probe.compact_context = provider.responses_compact;
		probe.strict_tools = true;
		probe.require_tool_call = true;
		agent_tool_t tool("webcool.capability_probe",
		    "Return a harmless acknowledgement.", false, true, false,
		    "read");
		tool.parameters.push_back(
		    agent_tool_parameter_t("ack", "Set to ok.", true));
		probe.tools.push_back(tool);
		completion_result_t completion;
		const bool ok = provider_client_t::complete(
		    provider, api_key, probe, completion, err, NULL);
		result.http_status = completion.http_status;
		result.latency_ms = completion.latency_ms;
		result.endpoint = completion_url(provider, false);
		if (!ok)
			return ai_error("provider.client",
			    "test-responses-capability", err);
		if (!completion.native_tool_call ||
		    completion.tool_name != "webcool.capability.probe") {
			err =
			    "AI provider Responses capability probe returned no required tool call";
			return ai_error(
			    "provider.client", "test-responses-tool", err);
		}
		result.model_verified = true;
		result.responses_verified = true;
		result.tools_verified = true;
		return true;
	}
	parsed_url_t parsed;
	if (!parse_http_url(result.endpoint, parsed, err)) {
		return ai_error("provider.client", "parse-test-url", err);
	}

	std::unique_ptr<acl::openssl_conf> ssl;
	if (parsed.use_ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err)) {
			return ai_error(
			    "provider.client", "configure-test-tls", err);
		}
	}
	// Connection testing should use the same administrator-selected connect
	// budget as real completions; otherwise Kimi can pass one path and fail the
	// other solely because they used different hidden eight-second limits.
	acl::http_request request(parsed.address.c_str(),
	    provider_connect_timeout_seconds(), 20, true);
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
		err = std::string("AI provider connection failed: ") +
		    acl::last_serror();
		return ai_error("provider.client", "test-request", err);
	}
	result.latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now() - started)
	        .count();
	result.http_status = request.http_status();
	if (!(result.http_status < 200 || result.http_status >= 300))
		return true;
	std::string error_body;
	const long long length = request.body_length();
	if (length < 0 || length <= 256 * 1024) {
		acl::string response;
		if (request.get_body(response) &&
		    response.size() <= 256 * 1024) {
			error_body.assign(response.c_str(), response.size());
		}
	}
	err = provider_client_t::describe_http_error(result.http_status,
	    error_body, response_header(request, "x-request-id"),
	    response_header(request, "Retry-After"),
	    response_header(request, "x-ratelimit-reset-requests"),
	    response_header(request, "x-ratelimit-reset-tokens"));
	return ai_error("provider.client", "test-http-status", err);
}

bool provider_client_t::probe_usage_limits(const provider_config_t &provider,
    const std::string &api_key, provider_usage_probe_t &result,
    std::string &err)
{
	result = provider_usage_probe_t();
	// Kimi currently documents a balance endpoint but no dedicated endpoint for
	// querying the organisation's live RPM/TPM tier. Capture rate-limit headers
	// when present; otherwise the UI must report those dimensions as unknown.
	if (!kimi_model_is(provider, "kimi-")) {
		result.message =
		    "provider does not expose a supported usage probe";
		return true;
	}
	result.supported = true;
	result.endpoint = kimi_balance_url(provider);
	parsed_url_t parsed;
	if (!parse_http_url(result.endpoint, parsed, err)) {
		return ai_error("provider.client", "parse-usage-url", err);
	}
	std::unique_ptr<acl::openssl_conf> ssl;
	if (parsed.use_ssl) {
		ssl.reset(new acl::openssl_conf(false));
		if (!configure_tls(*ssl, parsed.verify_host, err)) {
			return ai_error(
			    "provider.client", "configure-usage-tls", err);
		}
	}
	// Keep preflight bounded: it must not make the Send button appear hung when
	// the provider is unreachable.
	acl::http_request request(parsed.address.c_str(), 5, 10, true);
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
		err = std::string("AI provider usage probe failed: ") +
		    acl::last_serror();
		return ai_error("provider.client", "usage-request", err);
	}
	result.checked = true;
	result.latency_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(
	        std::chrono::steady_clock::now() - started)
	        .count();
	result.http_status = request.http_status();
	result.request_limit =
	    response_header(request, "x-ratelimit-limit-requests");
	result.request_remaining =
	    response_header(request, "x-ratelimit-remaining-requests");
	result.request_reset =
	    response_header(request, "x-ratelimit-reset-requests");
	result.token_limit =
	    response_header(request, "x-ratelimit-limit-tokens");
	result.token_remaining =
	    response_header(request, "x-ratelimit-remaining-tokens");
	result.token_reset =
	    response_header(request, "x-ratelimit-reset-tokens");
	result.retry_after = response_header(request, "Retry-After");
	if (result.http_status < 200 || result.http_status >= 300) {
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
		err = describe_http_error(result.http_status, error_body,
		    response_header(request, "x-request-id"),
		    result.retry_after, result.request_reset,
		    result.token_reset);
		return ai_error("provider.client", "usage-http-status", err);
	}
	acl::json body;
	if (!request.get_body(body) || !body.finish()) {
		err = "cannot read AI provider usage response";
		return ai_error("provider.client", "usage-json-response", err);
	}
	acl::json_node *data = body["data"];
	// ACL may represent a named object as a wrapper whose get_obj() owns the
	// actual children. object_child() handles both wrapped and direct objects.
	acl::json_node *balance = object_child(data, "available_balance");
	if (balance != NULL) {
		if (balance->get_double() != NULL) {
			result.available_balance = *balance->get_double();
			result.balance_known = true;
		} else if (balance->get_int64() != NULL) {
			result.available_balance =
			    static_cast<double>(*balance->get_int64());
			result.balance_known = true;
		}
	}
	result.currency = "CNY";
	result.can_start =
	    !result.balance_known || result.available_balance > 0.0;
	result.message = result.balance_known ?
	    (result.can_start ? "Kimi account balance is available" :
	                        "Kimi account balance is exhausted") :
	    "Kimi balance endpoint returned no recognizable balance";
	return true;
}
bool provider_client_t::retrieve_response(const provider_config_t &provider,
    const std::string &api_key, const std::string &response_id,
    completion_result_t &result, std::string &err)
{
	result = completion_result_t();
	if (provider.protocol != "openai_responses" ||
	    responses_are_stateless(provider) || response_id.empty() ||
	    response_id.size() > 256) {
		err = "invalid OpenAI Responses retrieve request";
		return ai_error(
		    "provider.client", "validate-response-retrieve", err);
	}
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
	        provider_scheduler_key(provider, api_key), NULL, NULL, permit,
	        err)) {
		result.error_category = provider_error_cancelled;
		return ai_error(
		    "provider.client", "schedule-response-retrieve", err);
	}
	const bool retrieved = get_json(provider, api_key,
	    responses_item_url(provider, response_id, false), body, status,
	    latency, err);
	if (!retrieved) {
		classify_error(status, err, result);
		provider_request_scheduler_t::finish(
		    permit, status, result.retryable_error, 0);
		return ai_error("provider.client", "retrieve-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	result.http_status = status;
	result.latency_ms = latency;
	if (parse_completion_json(provider, body, result, err))
		return true;
	result.error_category =
	    result.incomplete_reason == "max_output_tokens" ?
	    provider_error_response_limit :
	    (result.response_status == "cancelled" ? provider_error_cancelled :
	                                             provider_error_protocol);
	return ai_error("provider.client", "parse-retrieved-response", err);
}

bool provider_client_t::cancel_response(const provider_config_t &provider,
    const std::string &api_key, const std::string &response_id,
    completion_result_t &result, std::string &err)
{
	result = completion_result_t();
	if (provider.protocol != "openai_responses" ||
	    responses_are_stateless(provider) || response_id.empty() ||
	    response_id.size() > 256) {
		err = "invalid OpenAI Responses cancel request";
		return ai_error(
		    "provider.client", "validate-response-cancel", err);
	}
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
	        provider_scheduler_key(provider, api_key), NULL, NULL, permit,
	        err)) {
		result.error_category = provider_error_cancelled;
		return ai_error(
		    "provider.client", "schedule-response-cancel", err);
	}
	const bool cancelled = post_json(provider, api_key,
	    responses_item_url(provider, response_id, true), "{}", body, status,
	    latency, err);
	if (!cancelled) {
		classify_error(status, err, result);
		provider_request_scheduler_t::finish(
		    permit, status, result.retryable_error, 0);
		return ai_error("provider.client", "cancel-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	result.http_status = status;
	result.latency_ms = latency;
	result.response_id = node_text(body["id"]);
	result.response_status = node_text(body["status"]);
	if (!result.response_id.empty())
		return true;
	err = "OpenAI Responses cancel returned no response id";
	result.error_category = provider_error_protocol;
	return ai_error("provider.client", "parse-cancel-response", err);
}

bool provider_client_t::compact_response(const provider_config_t &provider,
    const std::string &api_key, const std::string &previous_response_id,
    const std::string &instructions, std::string &compaction_id,
    std::string &compacted_output_json, long long &input_tokens,
    long long &output_tokens, std::string &err)
{
	compaction_id.clear();
	compacted_output_json.clear();
	input_tokens = 0;
	output_tokens = 0;
	if (provider.protocol != "openai_responses" ||
	    responses_are_stateless(provider) || previous_response_id.empty() ||
	    previous_response_id.size() > 256) {
		err = "invalid OpenAI Responses compact request";
		return ai_error(
		    "provider.client", "validate-response-compact", err);
	}
	acl::json payload_json;
	acl::json_node &root = payload_json.create_node();
	root.add_text("model", provider.model.c_str());
	root.add_text("previous_response_id", previous_response_id.c_str());
	if (!instructions.empty())
		root.add_text("instructions", instructions.c_str());
	if (!provider.responses_cache_ttl.empty() &&
	    provider.responses_cache_ttl != "none" &&
	    model_name_contains(provider, "gpt-5.6")) {
		acl::json_node &options = payload_json.create_node();
		root.add_child("prompt_cache_options", options);
		options.add_text("mode", "implicit");
		options.add_text("ttl", provider.responses_cache_ttl.c_str());
	}
	const acl::string &serialized = root.to_string();
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
	        provider_scheduler_key(provider, api_key), NULL, NULL, permit,
	        err)) {
		return ai_error(
		    "provider.client", "schedule-response-compact", err);
	}
	const bool compacted =
	    post_json(provider, api_key, responses_compact_url(provider),
	        std::string(serialized.c_str(), serialized.size()), body,
	        status, latency, err);
	if (!compacted) {
		completion_result_t classified;
		classify_error(status, err, classified);
		provider_request_scheduler_t::finish(
		    permit, status, classified.retryable_error, 0);
		return ai_error("provider.client", "compact-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	compaction_id = node_text(body["id"]);
	acl::json_node *output = body["output"];
	acl::json_node *output_array = output && output->is_array() ?
	    output :
	    (output ? output->get_obj() : NULL);
	if (output_array != NULL && output_array->is_array()) {
		const acl::string &output_serialized =
		    output_array->to_string();
		compacted_output_json.assign(
		    output_serialized.c_str(), output_serialized.size());
	}
	acl::json_node *usage = body["usage"];
	input_tokens = node_number(object_child(usage, "input_tokens"));
	output_tokens = node_number(object_child(usage, "output_tokens"));
	if (!(compaction_id.empty() || compacted_output_json.empty()))
		return true;
	err = "OpenAI Responses compact returned no reusable output items";
	return ai_error("provider.client", "parse-compact-response", err);
}

} // namespace ai
} // namespace webcool
