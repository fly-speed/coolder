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

namespace webcool {
namespace ai {
using namespace provider_detail;

bool provider_client_t::test_connection(const provider_config_t& provider,
	const std::string& api_key, provider_test_result_t& result,
	std::string& err)
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
		probe.system_prompt = "Return the required capability-probe function call.";
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
			"Return a harmless acknowledgement.", false, true, false, "read");
		tool.parameters.push_back(agent_tool_parameter_t(
			"ack", "Set to ok.", true));
		probe.tools.push_back(tool);
		completion_result_t completion;
		const bool ok = provider_client_t::complete(provider, api_key, probe,
			completion, err, NULL);
		result.http_status = completion.http_status;
		result.latency_ms = completion.latency_ms;
		result.endpoint = completion_url(provider, false);
		if (!ok) return ai_error("provider.client", "test-responses-capability", err);
		if (!completion.native_tool_call
			|| completion.tool_name != "webcool.capability.probe")
		{
			err = "AI provider Responses capability probe returned no required tool call";
			return ai_error("provider.client", "test-responses-tool", err);
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
			return ai_error("provider.client", "configure-test-tls", err);
		}
	}
	// Connection testing should use the same administrator-selected connect
	// budget as real completions; otherwise Kimi can pass one path and fail the
	// other solely because they used different hidden eight-second limits.
	acl::http_request request(parsed.address.c_str(),
		provider_connect_timeout_seconds(), 20, true);
	if (ssl.get() != NULL) {
		request.set_ssl(ssl.get()).set_ssl_sni(parsed.verify_host.c_str());
	}
	acl::http_header& header = request.request_header();
	header.set_url(parsed.path.c_str()).set_host(parsed.host.c_str())
		.set_keep_alive(false).set_content_type("application/json");
	add_auth_headers(header, provider, api_key);
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	if (!request.get()) {
		err = std::string("AI provider connection failed: ") + acl::last_serror();
		return ai_error("provider.client", "test-request", err);
	}
	result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	result.http_status = request.http_status();
	if (result.http_status < 200 || result.http_status >= 300) {
		std::string error_body;
		const long long length = request.body_length();
		if (length < 0 || length <= 256 * 1024) {
			acl::string response;
			if (request.get_body(response) && response.size() <= 256 * 1024) {
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
	return true;
}

bool provider_client_t::probe_usage_limits(const provider_config_t& provider,
	const std::string& api_key, provider_usage_probe_t& result,
	std::string& err)
{
	result = provider_usage_probe_t();
	// Kimi currently documents a balance endpoint but no dedicated endpoint for
	// querying the organisation's live RPM/TPM tier. Capture rate-limit headers
	// when present; otherwise the UI must report those dimensions as unknown.
	if (!kimi_model_is(provider, "kimi-")) {
		result.message = "provider does not expose a supported usage probe";
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
			return ai_error("provider.client", "configure-usage-tls", err);
		}
	}
	// Keep preflight bounded: it must not make the Send button appear hung when
	// the provider is unreachable.
	acl::http_request request(parsed.address.c_str(), 5, 10, true);
	if (ssl.get() != NULL) {
		request.set_ssl(ssl.get()).set_ssl_sni(parsed.verify_host.c_str());
	}
	acl::http_header& header = request.request_header();
	header.set_url(parsed.path.c_str()).set_host(parsed.host.c_str())
		.set_keep_alive(false).set_content_type("application/json");
	add_auth_headers(header, provider, api_key);
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	if (!request.get()) {
		err = std::string("AI provider usage probe failed: ") + acl::last_serror();
		return ai_error("provider.client", "usage-request", err);
	}
	result.checked = true;
	result.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	result.http_status = request.http_status();
	result.request_limit = response_header(request, "x-ratelimit-limit-requests");
	result.request_remaining = response_header(request,
		"x-ratelimit-remaining-requests");
	result.request_reset = response_header(request, "x-ratelimit-reset-requests");
	result.token_limit = response_header(request, "x-ratelimit-limit-tokens");
	result.token_remaining = response_header(request,
		"x-ratelimit-remaining-tokens");
	result.token_reset = response_header(request, "x-ratelimit-reset-tokens");
	result.retry_after = response_header(request, "Retry-After");
	if (result.http_status < 200 || result.http_status >= 300) {
		std::string error_body;
		const long long length = request.body_length();
		if (length < 0 || length <= 256 * 1024) {
			acl::string response;
			if (request.get_body(response) && response.size() <= 256 * 1024) {
				error_body.assign(response.c_str(), response.size());
			}
		}
		err = describe_http_error(result.http_status, error_body,
			response_header(request, "x-request-id"), result.retry_after,
			result.request_reset, result.token_reset);
		return ai_error("provider.client", "usage-http-status", err);
	}
	acl::json body;
	if (!request.get_body(body) || !body.finish()) {
		err = "cannot read AI provider usage response";
		return ai_error("provider.client", "usage-json-response", err);
	}
	acl::json_node* data = body["data"];
	// ACL may represent a named object as a wrapper whose get_obj() owns the
	// actual children. object_child() handles both wrapped and direct objects.
	acl::json_node* balance = object_child(data, "available_balance");
	if (balance != NULL) {
		if (balance->get_double() != NULL) {
			result.available_balance = *balance->get_double();
			result.balance_known = true;
		} else if (balance->get_int64() != NULL) {
			result.available_balance = static_cast<double>(*balance->get_int64());
			result.balance_known = true;
		}
	}
	result.currency = "CNY";
	result.can_start = !result.balance_known || result.available_balance > 0.0;
	result.message = result.balance_known
		? (result.can_start ? "Kimi account balance is available"
			: "Kimi account balance is exhausted")
		: "Kimi balance endpoint returned no recognizable balance";
	return true;
}

namespace {

// Shared state for one completion, including recovery attempts and usage.
class completion_call_t {
public:
	completion_call_t(const provider_config_t& provider_value, const std::string& key,
		const completion_request_t& request, completion_result_t& output,
		std::string& error, completion_stream_observer_t* stream_observer, bool& retried)
		: provider(provider_value), api_key(key), requested_input(request), result(output),
		  err(error), observer(stream_observer), transport_retried(retried) {}
	bool run();

private:
	bool submit_request();
	bool submit_repair_request();
	bool initialize_request();
	bool request_with_retries();
	bool recover_reasoning_budget();
	bool recover_tool_arguments();
	bool retry_without_streaming();
	bool fallback_without_native_tools();
	bool parse_response();
	bool poll_background_response();
	void merge_recovered_usage();

	const provider_config_t& provider;
	const std::string& api_key;
	const completion_request_t& requested_input;
	completion_result_t& result;
	std::string& err;
	completion_stream_observer_t* observer;
	bool& transport_retried;
	completion_request_t input;
	completion_request_t wire_input;
	long long effective_max_output_tokens = 0;
	bool use_stream = false;
	std::string payload;
	// A non-NULL object marks a successful or attempted non-streaming path. Each
	// retry receives a fresh parser so partial JSON from an earlier attempt can
	// never leak into the next response.
	std::unique_ptr<acl::json> body;
	// If a successful HTTP stream ends with malformed function arguments, one
	// safe non-streaming replay is allowed.  Keep the failed attempt's usage and
	// visible reasoning for audit/UI purposes, but never retain its partial tool
	// arguments.  The replay result is merged only after its JSON parses fully.
	completion_result_t malformed_stream_result;
	bool recovered_malformed_stream = false;
	std::string malformed_stream_error;
	int status = 0;
	bool requested = false;
	std::string scheduler_key;
};

bool completion_call_t::submit_request()
{
	status = 0;
	err.clear();
	result.error_category = provider_error_none;
	result.retryable_error = false;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(scheduler_key,
		provider_request_cancelled, observer, permit, err))
	{
		result.error_category = provider_error_cancelled;
		return ai_error("provider.client", "scheduler-acquire", err);
	}
	bool ok = false;
	if (observer != NULL && !input.background) {
		if (!observer->on_request_payload(payload, err)) {
			provider_request_scheduler_t::finish(permit, 0, false, 0);
			return false;
		}
		ok = post_json_stream(provider, api_key,
			completion_url(provider, true), payload, result, *observer,
			status, result.latency_ms, err, input.transport_session);
	} else {
		body.reset(new acl::json);
		ok = post_json(provider, api_key,
			completion_url(provider, false), payload, *body, status,
			result.latency_ms, err);
	}
	if (!ok) classify_error(status, err, result);
	const unsigned long retry_after = result.error_category == provider_error_rate_limit && result.retryable_error
		? rate_limit_retry_delay_seconds(err, 0) : 0;
	provider_request_scheduler_t::finish(permit,
		result.error_category == provider_error_rate_limit ? 429 : status,
		result.retryable_error, retry_after);
	return ok;
}

bool completion_call_t::submit_repair_request()
{
	body.reset(new acl::json);
	status = 0;
	err.clear();
	result.error_category = provider_error_none;
	result.retryable_error = false;
	if (!observer->on_request_payload(payload, err)) {
		return ai_error("provider.client",
			"archive-malformed-tool-retry", err);
	}
	provider_request_permit_t retry_permit;
	if (!provider_request_scheduler_t::acquire(scheduler_key,
		provider_request_cancelled, observer, retry_permit, err))
	{
		result.error_category = provider_error_cancelled;
		return ai_error("provider.client",
			"scheduler-malformed-tool-retry", err);
	}
	long long retry_latency_ms = 0;
	const bool ok = post_json(provider, api_key,
		completion_url(provider, false), payload, *body, status,
		retry_latency_ms, err);
	result.latency_ms += retry_latency_ms;
	if (!ok) classify_error(status, err, result);
	const unsigned long retry_after = status == 429
		&& result.retryable_error
		? rate_limit_retry_delay_seconds(err, 0) : 0;
	provider_request_scheduler_t::finish(retry_permit, status,
		result.retryable_error, retry_after);
	return ok;
}

bool completion_call_t::initialize_request()
{
	// Kimi only executes Responses synchronously. Normalize before validation,
	// streaming selection, retries and polling so saved background settings do
	// not disable streaming or accidentally enter the background polling path.
	input = requested_input;
	if (deepseek_responses_endpoint(provider)
		|| kimi_responses_endpoint(provider) || qwen_responses_endpoint(provider)) {
		input.background = false;
		input.compact_context = false;
	}
	result.error_response_excerpt.clear();
	result.error_response_content_type.clear();
	result.error_response_content_encoding.clear();
	result.text.clear();
	result.response_id.clear();
	result.response_status.clear();
	result.incomplete_reason.clear();
	result.reasoning.clear();
	result.input_tokens = 0;
	result.cached_input_tokens = 0;
	result.output_tokens = 0;
	result.reasoning_tokens = 0;
	result.effective_max_output_tokens = 0;
	result.latency_ms = 0;
	result.http_status = 0;
	result.error_category = provider_error_none;
	result.retryable_error = false;
	result.reasoning_budget_recovered = false;
	result.native_tool_call = false;
	result.tool_calls.clear();
	result.tool_name.clear();
	result.tool_path.clear();
	result.tool_query.clear();
	result.tool_old_text.clear();
	result.tool_target_path.clear();
	result.tool_content.clear();
	if (input.user_prompt.empty() || input.user_prompt.size() > kMaxAgentPromptBytes
		|| input.system_prompt.size() > 64 * 1024)
	{
		err = "agent prompt is empty or too large";
		return ai_error("provider.client", "validate-prompt-size", err);
	}
	if (input.max_output_tokens < 1 || input.max_output_tokens > 1000000) {
		err = "max_output_tokens must be between 1 and 1000000";
		return ai_error("provider.client", "validate-output-limit", err);
	}
	effective_max_output_tokens = effective_provider_output_limit(
		provider, input.max_output_tokens);
	result.effective_max_output_tokens = effective_max_output_tokens;
	if (provider.protocol == "openai_responses"
		&& input.background && !input.store)
	{
		err = "OpenAI background responses require store=true";
		return ai_error("provider.client", "validate-background-store", err);
	}
	return true;
}

bool completion_call_t::request_with_retries()
{
	requested = submit_request();

	// Tool-based coding runs can consume several requests in quick succession.
	// A provider's RPM/concurrency 429 is temporary and normally includes an exact
	// Retry-After value. Retry it inside the same model turn so the durable agent
	// checkpoint does not fail merely because the next token-bucket slot is one
	// second away. Quota/billing 429s remain terminal via classify_error().
	for (unsigned int retry = 0; !requested && result.error_category == provider_error_rate_limit && retry < 3; ++retry) {
		classify_error(status, err, result);
		if (!result.retryable_error) break;
		const unsigned long delay_seconds =
			rate_limit_retry_delay_seconds(err, retry);
		ai_log_error("provider.client", "rate-limit-retry",
			"temporary provider rate limit/overload; retrying after "
			+ std::to_string(delay_seconds) + " seconds");
		if (!wait_for_rate_limit_retry(delay_seconds, observer)) {
			err = "AI provider rate-limit retry cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		result = completion_result_t();
		result.effective_max_output_tokens = effective_max_output_tokens;
		requested = submit_request();
	}
	// Some compatible Responses gateways sporadically terminate an HTTP-200 SSE
	// response after a single metadata/role event. The response has no consumable
	// text or tool call, so retry exactly once on a fresh connection. This is much
	// narrower than replaying arbitrary protocol failures (malformed tool writes,
	// invalid JSON and other unsafe responses remain terminal).
	if (!requested && status >= 200 && status < 300
		&& result.error_category == provider_error_protocol
		&& err.find("streaming response contains no visible text or tool call")
			!= std::string::npos)
	{
		ai_log_error("provider.client", "empty-stream-retry",
			"provider returned an empty HTTP-200 stream; retrying once");
		if (!wait_for_transport_retry(0, observer, err)) {
			err = "AI provider empty-stream retry cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		result = completion_result_t();
		transport_retried = true;
		result.effective_max_output_tokens = effective_max_output_tokens;
		requested = submit_request();
	}
	// Retry narrowly classified pre-response transport failures. Resource
	// pressure gets two attempts; a connection timeout gets one because replaying
	// an ambiguously delivered POST more aggressively could duplicate billing.
	const unsigned int transport_retry_limit = initial_transport_retry_limit(err,
		result.error_category);
	for (unsigned int retry = 0; !requested && status == 0
		&& retry < transport_retry_limit; ++retry)
	{
		ai_log_error("provider.client", "initial-transport-retry",
			err + "; retry=" + std::to_string(retry + 1));
		if (!wait_for_transport_retry(retry, observer, err)) {
			err = "AI provider temporary-resource retry cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		result = completion_result_t();
		transport_retried = true;
		result.effective_max_output_tokens = effective_max_output_tokens;
		requested = submit_request();
	}
	// Several compatible providers expose their model-specific output ceiling
	// only by rejecting an oversized request. Learn the exact upper bound from
	// that structured 400 and replay once with the same prompt and tool history.
	for (unsigned int retry = 0; !requested && status == 400 && retry < 2; ++retry) {
		const long long learned_limit =
			provider_max_output_tokens_from_error(err);
		if (learned_limit <= 0 || learned_limit >= effective_max_output_tokens) break;
		effective_max_output_tokens = learned_limit;
		remember_provider_output_limit(provider, learned_limit);
		ai_log_error("provider.client", "max-output-tokens-cap-retry",
			"provider rejected requested max_output_tokens="
			+ std::to_string(input.max_output_tokens) + "; retrying with learned limit="
			+ std::to_string(learned_limit));
		wire_input = input;
		wire_input.max_output_tokens = effective_max_output_tokens;
		build_completion_payload(provider, wire_input, use_stream, payload);
		result = completion_result_t();
		result.effective_max_output_tokens = effective_max_output_tokens;
		requested = submit_request();
	}
	return true;
}

bool completion_call_t::recover_reasoning_budget()
{
	// A reasoning model can spend the whole shared output budget before emitting
	// any answer. Retry only models with a real reasoning off switch. Kimi K2.7
	// Code/K3 always think; replaying an identical request restarts another billed
	// reasoning pass and has repeatedly exhausted the same limit without a tool
	// call, so those models return one recoverable checkpoint instead.
	const bool reasoning_was_enabled = kimi_model_always_thinks(provider)
		|| (input.reasoning_effort != "none"
			&& input.thinking_mode != "disabled");
	const bool output_budget_exhausted = reasoning_budget_exhausted(err)
		|| result.incomplete_reason == "max_output_tokens";
	if (!requested && output_budget_exhausted
		&& kimi_model_always_thinks(provider))
	{
		err += " This Kimi coding model did not expose a supported thinking-off"
			" control, so WebCool skipped an identical automatic retry to avoid"
			" repeating the same billed reasoning pass. Resume from the saved"
			" checkpoint; if it recurs, raise the administrator output-token"
			" policy to at least 32768 for this model.";
		result.error_category = provider_error_response_limit;
		ai_log_error("provider.client", "kimi-reasoning-budget-exhausted", err);
	}
	if (!requested && observer != NULL
		&& reasoning_was_enabled && output_budget_exhausted
		&& is_openai_reasoning_protocol(provider)
		&& !kimi_model_always_thinks(provider)
		&& result.text.empty() && !result.native_tool_call
		&& !observer->cancel_requested())
	{
		const completion_result_t exhausted = result;
		const std::string exhausted_err = err;
		ai_log_error("provider.client", "reasoning-budget-retry", exhausted_err);
		completion_request_t retry = input;
		retry.max_output_tokens = effective_max_output_tokens;
		retry.reasoning_effort = "none";
		if (!retry.thinking_mode.empty()) retry.thinking_mode = "disabled";
		// The incomplete response is not part of tool_history, so carry a bounded
		// tail into every recovery request. This lets both DeepSeek's no-thinking
		// retry and Kimi's minimum-thinking continuation act on completed analysis
		// instead of paying to rediscover it from the original prompt.
		const std::string prefix = prompt_text(prompt_id::provider_reasoning_recovery, input.ui_language != "en");
		const std::string suffix = "\n</recovery_checkpoint>";
		const size_t prompt_limit = kMaxAgentPromptBytes - 8 * 1024;
		if (retry.user_prompt.size() + prefix.size() + suffix.size() < prompt_limit) {
			const size_t available = prompt_limit - retry.user_prompt.size()
				- prefix.size() - suffix.size();
			const size_t retained = std::min<size_t>(48 * 1024,
				std::min(available, exhausted.reasoning.size()));
			retry.user_prompt += prefix;
			if (retained > 0) {
				retry.user_prompt.append(exhausted.reasoning,
					exhausted.reasoning.size() - retained, retained);
			}
			retry.user_prompt += suffix;
		}
		result = completion_result_t();
		result.effective_max_output_tokens = effective_max_output_tokens;
		status = 0;
		err.clear();
		build_completion_payload(provider, retry, true, payload);
		if (!observer->on_request_payload(payload, err)) {
			return ai_error("provider.client", "archive-reasoning-retry", err);
		}
		requested = post_json_stream(provider, api_key,
			completion_url(provider, true), payload, result, *observer,
			status, result.latency_ms, err, input.transport_session);
		// Both attempts are billable and therefore both belong in usage/audit data.
		result.input_tokens += exhausted.input_tokens;
		result.cached_input_tokens += exhausted.cached_input_tokens;
		result.output_tokens += exhausted.output_tokens;
		result.reasoning_tokens += exhausted.reasoning_tokens;
		result.latency_ms += exhausted.latency_ms;
		if (!exhausted.reasoning.empty()) {
			std::string combined = exhausted.reasoning;
			if (combined.size() + 2 < kMaxCompletionBytes
				&& !result.reasoning.empty()) combined += "\n\n";
			if (combined.size() < kMaxCompletionBytes) {
				combined.append(result.reasoning, 0,
					std::min(kMaxCompletionBytes - combined.size(),
						result.reasoning.size()));
			}
			result.reasoning.swap(combined);
		}
		if (!requested) {
			if (result.error_category == provider_error_cancelled) return false;
			if (status == 400 || status == 404 || status == 422) {
				err = exhausted_err + " Automatic no-reasoning retry was rejected"
					" by this compatible endpoint; configure this OpenAI model with"
					" the openai_responses protocol or raise its output-token policy.";
			} else if (reasoning_budget_exhausted(err)) {
				err += " Automatic retry with reasoning effort=none also exhausted"
					" the output-token budget; raise the administrator output-token"
					" policy or reduce the task scope.";
			}
			classify_error(status, err, result);
			return ai_error("provider.client", "reasoning-budget-recovery", err);
		}
		// Expose the successful recovery to the outer agent loop. Without this
		// marker the very next tool turn restores the configured `high` effort and
		// can consume another complete output budget before taking an action.
		result.reasoning_budget_recovered = true;
	}
	return true;
}

bool completion_call_t::recover_tool_arguments()
{
	// A provider can successfully finish the HTTP/SSE response while truncating
	// or corrupting the JSON string used for a native tool call.  Treating this as
	// a terminal agent failure forces the user to resume manually even though all
	// earlier tool results are already present in `input.tool_history`.  Replay the
	// same turn once as ordinary JSON, with a short repair instruction.  Partial
	// arguments from the first attempt are deliberately discarded and can never
	// reach the workspace executor.
	const bool malformed_tool_arguments =
		err.find("invalid streamed tool arguments") != std::string::npos
		|| err.find("invalid tool arguments") != std::string::npos;
	const bool empty_stream_response =
		err.find("streaming response contains no visible text or tool call")
			!= std::string::npos;
	if (!requested && observer != NULL && status >= 200 && status < 300
		&& (result.error_category == provider_error_protocol
			|| result.error_category == provider_error_response_limit)
		&& (malformed_tool_arguments || empty_stream_response)
		&& !observer->cancel_requested())
	{
		malformed_stream_result = result;
		malformed_stream_error = err;
		ai_log_error("provider.client", malformed_tool_arguments
			? "malformed-tool-retry" : "empty-stream-nonstream-retry", err);

		completion_request_t retry = input;
		retry.max_output_tokens = effective_max_output_tokens;
		if (malformed_tool_arguments) {
			const std::string repair_instruction = prompt_text(
				prompt_id::provider_tool_recovery, input.ui_language != "en");
			if (retry.user_prompt.size() + repair_instruction.size()
				<= kMaxAgentPromptBytes)
			{
				retry.user_prompt += repair_instruction;
			}
		}
		// Prefer the least expensive supported reasoning setting for a protocol
		// repair. Provider adapters already omit unsupported controls.
		retry.reasoning_effort = "none";
		if (!retry.thinking_mode.empty()) retry.thinking_mode = "disabled";

		result = completion_result_t();
		result.tool_arguments_recovery_attempted = malformed_tool_arguments;
		result.effective_max_output_tokens = effective_max_output_tokens;
		build_completion_payload(provider, retry, false, payload);
		requested = submit_repair_request();
		const unsigned int repair_transport_retry_limit =
			initial_transport_retry_limit(err, result.error_category);
		for (unsigned int transport_retry = 0; !requested && status == 0
			&& transport_retry < repair_transport_retry_limit; ++transport_retry)
		{
			ai_log_error("provider.client", "repair-initial-transport-retry",
				err + "; retry=" + std::to_string(transport_retry + 1));
			if (!wait_for_transport_retry(transport_retry, observer, err)) {
				err = "AI provider temporary-resource retry cancelled";
				result.error_category = provider_error_cancelled;
				break;
			}
			transport_retried = true;
			requested = submit_repair_request();
		}
		if (requested) {
			recovered_malformed_stream = true;
		} else {
			const std::string retry_error = err;
			err = malformed_stream_error
				+ ". Automatic non-streaming recovery also failed: "
				+ retry_error;
			classify_error(status, err, result);
		}
	}
	return true;
}

bool completion_call_t::retry_without_streaming()
{
	// Older OpenAI-compatible gateways often reject stream_options or streaming
	// tool calls. Retry the same request once without streaming before removing
	// native tool definitions.
	if (!requested && observer != NULL && (status == 400 || status == 404
		|| status == 422) && result.error_category != provider_error_cancelled)
	{
		body.reset(new acl::json);
		err.clear();
		status = 0;
		result.text.clear();
		result.response_id.clear();
		result.native_tool_call = false;
		result.tool_calls.clear();
		result.tool_name.clear();
		result.tool_path.clear();
		result.tool_query.clear();
		result.tool_old_text.clear();
		result.tool_target_path.clear();
		result.tool_content.clear();
		completion_request_t nonstream_retry = input;
		nonstream_retry.max_output_tokens = effective_max_output_tokens;
		// A compatible gateway may support native functions but reject the
		// stronger required-tool selector. Preserve the native catalog on this
		// first compatibility retry and relax only that optional selector; the
		// agent's proposal-only prompt and guard still reject prose/read loops.
		nonstream_retry.require_tool_call = false;
		// This branch is a protocol compatibility repair after a 400/404/422, not a
		// fresh reasoning turn. In particular, some Responses-compatible endpoints
		// reject tool_choice=required; replaying the relaxed request with `high`
		// thinking can spend the whole budget before producing the required action.
		nonstream_retry.reasoning_effort = "none";
		if (!nonstream_retry.thinking_mode.empty()) {
			nonstream_retry.thinking_mode = "disabled";
		}
		build_completion_payload(provider, nonstream_retry, false, payload);
		if (!observer->on_request_payload(payload, err)) {
			return ai_error("provider.client", "archive-nonstream-retry", err);
		}
		requested = post_json(provider, api_key, completion_url(provider, false),
			payload, *body, status, result.latency_ms, err);
	}
	return true;
}

bool completion_call_t::fallback_without_native_tools()
{
	if (!requested)
	{
		if (result.error_category != provider_error_cancelled) {
			classify_error(status, err, result);
		}
		if (input.tools.empty() || (status != 400 && status != 404
			&& status != 422))
		{
			return ai_error("provider.client", "completion-request", err);
		}
		// Some OpenAI-compatible servers reject native tool definitions. Record
		// that adapter failure, then retry once with the bounded JSON fallback.
		ai_log_error("provider.client", "native-tools-fallback", err);
		completion_request_t fallback = input;
		fallback.max_output_tokens = effective_max_output_tokens;
		fallback.tools.clear();
		// The JSON-in-text compatibility fallback has no native functions from
		// which the provider could choose, so retaining tool_choice=required would
		// make the retry invalid.
		fallback.require_tool_call = false;
		build_completion_payload(provider, fallback, false, payload);
		body.reset(new acl::json);
		status = 0;
		err.clear();
		long long retry_latency_ms = 0;
		if (observer != NULL && !observer->on_request_payload(payload, err)) {
			return ai_error("provider.client", "archive-tool-fallback", err);
		}
		if (!post_json(provider, api_key, completion_url(provider, false), payload, *body,
			status, retry_latency_ms, err))
		{
			classify_error(status, err, result);
			return ai_error("provider.client", "fallback-completion-request", err);
		}
		result.latency_ms += retry_latency_ms;
		result.error_category = provider_error_none;
		result.retryable_error = false;
	}
	return true;
}

bool completion_call_t::parse_response()
{
	// Preserve the real successful status even if the response later fails
	// schema validation; otherwise diagnostics misleadingly report HTTP 0.
	result.http_status = status;
	if (body.get() != NULL && !parse_completion_json(provider, *body, result, err))
	{
		result.error_category = result.incomplete_reason == "max_output_tokens"
			? provider_error_response_limit
			: (result.response_status == "cancelled"
				? provider_error_cancelled : provider_error_protocol);
		if (recovered_malformed_stream) {
			err = malformed_stream_error
				+ ". Automatic non-streaming recovery returned an invalid response: "
				+ err;
		}
		// The parser returns fixed summaries and never embeds the response body.
		return ai_error("provider.client", "parse-completion", err);
	}
	return true;
}

bool completion_call_t::poll_background_response()
{
	if (input.background && provider.protocol == "openai_responses"
		&& (result.response_status == "queued"
			|| result.response_status == "in_progress"))
	{
		if (result.response_id.empty()) {
			err = "OpenAI background response returned no response id";
			result.error_category = provider_error_protocol;
			return ai_error("provider.client", "background-response-id", err);
		}
		if (observer != NULL
			&& !observer->on_response_state(result.response_id,
				result.response_status))
		{
			err = "OpenAI background response tracking was cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		const std::string background_id = result.response_id;
		long long total_background_latency = result.latency_ms;
		const std::chrono::steady_clock::time_point poll_started =
			std::chrono::steady_clock::now();
		size_t poll = 0;
		size_t consecutive_poll_errors = 0;
		while (std::chrono::duration_cast<std::chrono::minutes>(
			std::chrono::steady_clock::now() - poll_started).count() < 30) {
			if (observer != NULL && observer->cancel_requested()) {
				completion_result_t cancelled;
				std::string cancel_err;
				if (!provider_client_t::cancel_response(provider, api_key,
					background_id, cancelled, cancel_err))
				{
					ai_log_error("provider.client", "cancel-background-response",
						cancel_err);
				}
				err = "OpenAI background response cancelled";
				result.error_category = provider_error_cancelled;
				return false;
			}
			const unsigned int delay_ms = static_cast<unsigned int>(
				std::min<size_t>(5000, 750 + poll * 250));
			acl::fiber::delay(delay_ms);
			++poll;
			completion_result_t polled;
			std::string poll_err;
			if (!provider_client_t::retrieve_response(provider, api_key,
				background_id, polled, poll_err))
			{
				if (observer != NULL && !polled.response_status.empty()) {
					observer->on_response_state(background_id,
						polled.response_status);
				}
				if ((polled.retryable_error || polled.error_category == provider_error_network)
					&& consecutive_poll_errors++ < 5) continue;
				err = poll_err;
				result = polled;
				return false;
			}
			consecutive_poll_errors = 0;
			total_background_latency += polled.latency_ms;
			if (observer != NULL) {
				observer->on_response_state(background_id, polled.response_status);
			}
			if (polled.response_status == "queued"
				|| polled.response_status == "in_progress") continue;
			polled.latency_ms = total_background_latency;
			result = polled;
			if (observer != NULL && !result.reasoning.empty()
				&& !observer->on_reasoning_delta(result.reasoning))
			{
				err = "OpenAI background response cancelled";
				result.error_category = provider_error_cancelled;
				return false;
			}
			if (observer != NULL && !result.text.empty()
				&& !observer->on_text_delta(result.text))
			{
				err = "OpenAI background response cancelled";
				result.error_category = provider_error_cancelled;
				return false;
			}
			break;
		}
		if (result.response_status == "queued"
			|| result.response_status == "in_progress")
		{
			err = "OpenAI background response polling timed out";
			result.error_category = provider_error_timeout;
			result.retryable_error = true;
			return ai_error("provider.client", "background-response-timeout", err);
		}
	}
	return true;
}

void completion_call_t::merge_recovered_usage()
{
	if (recovered_malformed_stream) {
		// Both provider attempts consumed time/tokens. Preserve the original
		// reasoning so the expandable reasoning panel explains why recovery was
		// needed, while the valid replay exclusively supplies the executable call.
		result.input_tokens += malformed_stream_result.input_tokens;
		result.cached_input_tokens += malformed_stream_result.cached_input_tokens;
		result.output_tokens += malformed_stream_result.output_tokens;
		result.reasoning_tokens += malformed_stream_result.reasoning_tokens;
		result.latency_ms += malformed_stream_result.latency_ms;
		if (!malformed_stream_result.reasoning.empty()) {
			std::string combined = malformed_stream_result.reasoning;
			if (combined.size() + 2 < kMaxCompletionBytes
				&& !result.reasoning.empty()) combined += "\n\n";
			if (combined.size() < kMaxCompletionBytes) {
				combined.append(result.reasoning, 0,
					std::min(kMaxCompletionBytes - combined.size(),
						result.reasoning.size()));
			}
			result.reasoning.swap(combined);
		}
	}
}

bool completion_call_t::run()
{
	if (!initialize_request()) return false;
	use_stream = observer != NULL && !input.background;
	wire_input = input;
	wire_input.max_output_tokens = effective_max_output_tokens;
	build_completion_payload(provider, wire_input, use_stream, payload);
	scheduler_key = provider_scheduler_key(provider, api_key);
	if (!request_with_retries()) return false;
	if (!recover_reasoning_budget()) return false;
	if (!recover_tool_arguments()) return false;
	if (!retry_without_streaming()) return false;
	if (!fallback_without_native_tools()) return false;
	if (!parse_response()) return false;
	if (!poll_background_response()) return false;
	merge_recovered_usage();
	return true;
}

} // namespace

bool provider_client_t::complete(const provider_config_t& provider,
	const std::string& api_key, const completion_request_t& requested_input,
	completion_result_t& result, std::string& err,
	completion_stream_observer_t* observer)
{
	if (provider.protocol == "openai_images") {
		err = "image providers are only available in AI Assistant image mode";
		return false;
	}
	// Preserve elapsed time on every return, including schema/stream failures.
	struct elapsed_guard_t {
		completion_result_t& value;
		bool transport_retried = false;
		std::chrono::steady_clock::time_point started;
		explicit elapsed_guard_t(completion_result_t& output)
			: value(output), started(std::chrono::steady_clock::now()) {}
		~elapsed_guard_t() { value.transport_retry_attempted = transport_retried; value.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count(); }
	} elapsed_guard(result);
	completion_call_t call(provider, api_key, requested_input, result, err,
		observer, elapsed_guard.transport_retried);
	return call.run();
}

bool provider_client_t::retrieve_response(const provider_config_t& provider,
	const std::string& api_key, const std::string& response_id,
	completion_result_t& result, std::string& err)
{
	result = completion_result_t();
	if (provider.protocol != "openai_responses"
		|| responses_are_stateless(provider) || response_id.empty()
		|| response_id.size() > 256)
	{
		err = "invalid OpenAI Responses retrieve request";
		return ai_error("provider.client", "validate-response-retrieve", err);
	}
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
		provider_scheduler_key(provider, api_key), NULL, NULL, permit, err))
	{
		result.error_category = provider_error_cancelled;
		return ai_error("provider.client", "schedule-response-retrieve", err);
	}
	const bool retrieved = get_json(provider, api_key,
		responses_item_url(provider, response_id, false), body, status,
		latency, err);
	if (!retrieved) {
		classify_error(status, err, result);
		provider_request_scheduler_t::finish(permit, status,
			result.retryable_error, 0);
		return ai_error("provider.client", "retrieve-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	result.http_status = status;
	result.latency_ms = latency;
	if (!parse_completion_json(provider, body, result, err)) {
		result.error_category = result.incomplete_reason == "max_output_tokens"
			? provider_error_response_limit
			: (result.response_status == "cancelled"
				? provider_error_cancelled : provider_error_protocol);
		return ai_error("provider.client", "parse-retrieved-response", err);
	}
	return true;
}

bool provider_client_t::cancel_response(const provider_config_t& provider,
	const std::string& api_key, const std::string& response_id,
	completion_result_t& result, std::string& err)
{
	result = completion_result_t();
	if (provider.protocol != "openai_responses"
		|| responses_are_stateless(provider) || response_id.empty()
		|| response_id.size() > 256)
	{
		err = "invalid OpenAI Responses cancel request";
		return ai_error("provider.client", "validate-response-cancel", err);
	}
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
		provider_scheduler_key(provider, api_key), NULL, NULL, permit, err))
	{
		result.error_category = provider_error_cancelled;
		return ai_error("provider.client", "schedule-response-cancel", err);
	}
	const bool cancelled = post_json(provider, api_key,
		responses_item_url(provider, response_id, true), "{}", body,
		status, latency, err);
	if (!cancelled) {
		classify_error(status, err, result);
		provider_request_scheduler_t::finish(permit, status,
			result.retryable_error, 0);
		return ai_error("provider.client", "cancel-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	result.http_status = status;
	result.latency_ms = latency;
	result.response_id = node_text(body["id"]);
	result.response_status = node_text(body["status"]);
	if (result.response_id.empty()) {
		err = "OpenAI Responses cancel returned no response id";
		result.error_category = provider_error_protocol;
		return ai_error("provider.client", "parse-cancel-response", err);
	}
	return true;
}

bool provider_client_t::compact_response(const provider_config_t& provider,
	const std::string& api_key, const std::string& previous_response_id,
	const std::string& instructions, std::string& compaction_id,
	std::string& compacted_output_json, long long& input_tokens,
	long long& output_tokens, std::string& err)
{
	compaction_id.clear();
	compacted_output_json.clear();
	input_tokens = 0;
	output_tokens = 0;
	if (provider.protocol != "openai_responses"
		|| responses_are_stateless(provider)
		|| previous_response_id.empty() || previous_response_id.size() > 256)
	{
		err = "invalid OpenAI Responses compact request";
		return ai_error("provider.client", "validate-response-compact", err);
	}
	acl::json payload_json;
	acl::json_node& root = payload_json.create_node();
	root.add_text("model", provider.model.c_str());
	root.add_text("previous_response_id", previous_response_id.c_str());
	if (!instructions.empty()) root.add_text("instructions", instructions.c_str());
	if (!provider.responses_cache_ttl.empty()
		&& provider.responses_cache_ttl != "none"
		&& model_name_contains(provider, "gpt-5.6"))
	{
		acl::json_node& options = payload_json.create_node();
		root.add_child("prompt_cache_options", options);
		options.add_text("mode", "implicit");
		options.add_text("ttl", provider.responses_cache_ttl.c_str());
	}
	const acl::string& serialized = root.to_string();
	acl::json body;
	int status = 0;
	long long latency = 0;
	provider_request_permit_t permit;
	if (!provider_request_scheduler_t::acquire(
		provider_scheduler_key(provider, api_key), NULL, NULL, permit, err))
	{
		return ai_error("provider.client", "schedule-response-compact", err);
	}
	const bool compacted = post_json(provider, api_key,
		responses_compact_url(provider),
		std::string(serialized.c_str(), serialized.size()), body, status,
		latency, err);
	if (!compacted) {
		completion_result_t classified;
		classify_error(status, err, classified);
		provider_request_scheduler_t::finish(permit, status,
			classified.retryable_error, 0);
		return ai_error("provider.client", "compact-response", err);
	}
	provider_request_scheduler_t::finish(permit, status, false, 0);
	compaction_id = node_text(body["id"]);
	acl::json_node* output = body["output"];
	acl::json_node* output_array = output && output->is_array()
		? output : (output ? output->get_obj() : NULL);
	if (output_array != NULL && output_array->is_array()) {
		const acl::string& output_serialized = output_array->to_string();
		compacted_output_json.assign(output_serialized.c_str(),
			output_serialized.size());
	}
	acl::json_node* usage = body["usage"];
	input_tokens = node_number(object_child(usage, "input_tokens"));
	output_tokens = node_number(object_child(usage, "output_tokens"));
	if (compaction_id.empty() || compacted_output_json.empty()) {
		err = "OpenAI Responses compact returned no reusable output items";
		return ai_error("provider.client", "parse-compact-response", err);
	}
	return true;
}


} // namespace ai
} // namespace webcool
