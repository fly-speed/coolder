#include "provider_stream_progress.h"
#pragma once

#include "../agent/agent_registry.h"
#include "ai_provider_store.h"

#include <memory>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

class provider_transport_session_t;

enum provider_error_category_t {
	provider_error_none,
	provider_error_network,
	provider_error_timeout,
	provider_error_authentication,
	provider_error_rate_limit,
	provider_error_invalid_request,
	provider_error_unavailable,
	provider_error_response_limit,
	provider_error_protocol,
	provider_error_cancelled,
	provider_error_unknown
};

struct provider_test_result_t {
	// Endpoint is returned for diagnostics but never contains an API key.
	int http_status;
	long long latency_ms;
	std::string endpoint;
	bool model_verified = false;
	bool responses_verified = false;
	bool tools_verified = false;
};

// Lightweight account/rate-limit information collected before a coding run.
// Providers do not expose a uniform limits API, so every field carries an
// explicit "known" flag and unknown must never be interpreted as unlimited.
struct provider_usage_probe_t {
	bool supported = false;
	bool checked = false;
	bool balance_known = false;
	bool can_start = true;
	int http_status = 0;
	long long latency_ms = 0;
	double available_balance = 0.0;
	std::string currency;
	std::string endpoint;
	std::string request_limit;
	std::string request_remaining;
	std::string request_reset;
	std::string token_limit;
	std::string token_remaining;
	std::string token_reset;
	std::string retry_after;
	std::string message;
};

// In-memory image supplied with one provider request. Temporary disk files are
// removed before the asynchronous run begins; only these bounded bytes remain.
struct completion_image_t {
	std::string name;
	std::string mime_type;
	std::string data;
};

// Result of one provider-native function call. OpenAI Responses can continue
// directly from a response ID when every call result is returned with its
// original call ID, avoiding retransmission of the complete tool transcript.
struct completion_tool_output_t {
	std::string call_id;
	std::string output;
};

// One normalized provider-native tool call. Providers can emit several calls
// in a single assistant turn; retaining the complete ordered set is required
// before the runtime can safely execute independent reads in parallel.
struct completion_tool_call_t {
	std::string id;
	std::string name;
	std::string path;
	std::string query;
	std::string old_text;
	std::string target_path;
	std::string content;
	bool content_present =
		false; // Explicit JSON string, including an intentional empty deletion.
};

// A completed native assistant/tool exchange. Chat-style providers require
// earlier calls and results to be replayed as typed role messages. The runtime
// keeps this bounded and falls back to its durable compact text checkpoint.
struct completion_tool_exchange_t {
	// Runtime developer instructions that preceded this assistant turn.
	std::string preceding_instructions;
	// Chat-style reasoning providers require the exact assistant turn to be
	// replayed before tool results.  Keeping these bounded per-run fields also
	// preserves a stable request prefix for provider-side context caching.
	std::string assistant_text;
	std::string reasoning_content;
	std::vector<completion_tool_call_t> calls;
	std::vector<completion_tool_output_t> outputs;
};

// Provider-neutral request consumed by the handwritten HTTP protocol adapters.
struct completion_request_t {
	// In-memory, task-scoped transport state; never serialized or checkpointed.
	std::shared_ptr<provider_transport_session_t> transport_session;
	// These two fields are sensitive and must never enter system logs. Coding
	// runs may retain them only in the user's protected project-local request
	// archive, as an explicit diagnostic feature.
	std::string system_prompt;
	// For stateless Responses, append changing runtime instructions after history.
	std::string turn_instructions;
	// Interface language captured per run; never shared between users.
	std::string ui_language = "zh";
	std::string user_prompt;
	long long max_output_tokens;
	// Optional provider reasoning control. Recovery uses "none" only after a
	// reasoning model has exhausted its entire output-token budget.
	std::string reasoning_effort;
	// Explicit vendor thinking toggle: empty uses provider defaults, while
	// "enabled"/"disabled" are translated by the selected protocol adapter.
	std::string thinking_mode;
	// Stable opaque identifier used only by providers that document a prompt
	// cache key (currently Kimi). It contains no API key or project content.
	std::string prompt_cache_key;
	// OpenAI Responses request controls. They are ignored by every other
	// protocol adapter so compatible providers retain their existing payloads.
	std::string safety_identifier;
	std::string service_tier;
	std::string text_verbosity;
	std::string reasoning_summary;
	std::string prompt_cache_ttl;
	std::string metadata_run_id;
	std::string metadata_session_id;
	bool store = true;
	bool background = false;
	bool compact_context = false;
	bool strict_tools = true;
	// Schemas come from the agent registry. Provider adapters only translate the
	// neutral descriptions and never maintain a second hard-coded tool catalog.
	std::vector<agent_tool_t> tools;
	// Progress-repair turns have already gathered the required context and must
	// produce one concrete tool action. Protocol adapters translate this into the
	// provider's native required-tool policy when tools are available.
	bool require_tool_call = false;
	std::vector<completion_image_t> images;
	// Native continuation is currently consumed only by the official Responses
	// protocol. Other adapters deliberately ignore it and keep their compatible
	// full-context fallback.
	std::string previous_response_id;
	std::vector<completion_tool_output_t> tool_outputs;
	std::vector<completion_tool_exchange_t> tool_history;
};

// Transient completion result. Text and tool arguments are kept in memory only
// and are excluded from the persistent agent run audit.
struct completion_result_t {
	bool tool_arguments_recovery_attempted = false;
	bool transport_retry_attempted = false;
	bool connection_reused = false;
	// Bounded failed-response diagnostic for the owning project's audit only.
	// Never add it to a model prompt or the normal browser result serializer.
	std::string error_response_excerpt;
	std::string error_response_content_type;
	std::string error_response_content_encoding;
	std::string text;
	// Opaque provider identifier used only for the immediately following native
	// continuation request; it is not a secret and is never used across users.
	std::string response_id;
	std::string response_status;
	std::string incomplete_reason;
	// Provider-supplied reasoning/thinking text is kept separate from the final
	// answer so callers can present it in a collapsible, clearly labelled view.
	std::string reasoning;
	long long input_tokens = 0;
	long long cached_input_tokens = 0;
	long long output_tokens = 0;
	long long reasoning_tokens = 0;
	// Actual value sent after applying a provider capability learned from a
	// rejected request. This can be lower than the administrator policy ceiling.
	long long effective_max_output_tokens = 0;
	long long latency_ms = 0;
	int http_status = 0;
	provider_error_category_t error_category = provider_error_none;
	bool retryable_error = false;
	// True when this logical model turn first exhausted its output budget in
	// reasoning and succeeded only after the client forced a direct/no-thinking
	// recovery request. The agent loop uses this signal to keep later turns in
	// direct mode instead of immediately falling back into the same costly loop.
	bool reasoning_budget_recovered = false;
	bool native_tool_call = false;
	std::vector<completion_tool_call_t> tool_calls;
	// Compatibility mirror of tool_calls.front(). New agent code consumes the
	// vector; these fields keep older callers and stored test fixtures readable.
	std::string tool_name;
	std::string tool_path;
	std::string tool_query;
	std::string tool_old_text;
	std::string tool_target_path;
	std::string tool_content;
};

// Receives normalized text fragments while the provider response is arriving.
// Returning false aborts the HTTP body read; observers must not persist or log
// fragments because they can contain source code or other sensitive text.
class completion_stream_observer_t {
public:
	virtual ~completion_stream_observer_t()
	{
	}
	// Called immediately before each HTTP request, including transport fallbacks
	// and automatic reasoning retries. Agent runtimes use this hook to archive
	// the exact JSON body in the selected project. API keys are HTTP headers and
	// are therefore never present in payload.
	virtual bool on_request_payload(const std::string &payload,
					std::string &err)
	{
		(void)payload;
		(void)err;
		return true;
	}
	// Lets the transport notice cancellation between response fragments, even
	// when the provider has not produced any text delta yet.
	virtual bool cancel_requested() const
	{
		return false;
	}
	virtual void
	on_stream_progress(const provider_stream_progress_t &progress)
	{
		(void)progress;
	}
	virtual bool on_text_delta(const std::string &delta) = 0;
	// Defaulting to success preserves source compatibility for observers that do
	// not need to expose provider reasoning.
	virtual bool on_reasoning_delta(const std::string &delta)
	{
		(void)delta;
		return true;
	}
	// Background Responses expose their durable ID before generation finishes.
	// Observers can checkpoint it immediately so a service restart can retrieve
	// the same response instead of issuing a duplicate billable request.
	virtual bool on_response_state(const std::string &response_id,
				       const std::string &status)
	{
		(void)response_id;
		(void)status;
		return true;
	}
};

// Thin C++ adapters for supported model HTTP APIs. No vendor SDK, Python,
// JavaScript or Java runtime is required by WebCool itself.
class provider_client_t {
public:
	static std::shared_ptr<provider_transport_session_t>
	create_transport_session();
	// DeepSeek and Kimi expose the Responses wire format as a stateless compatibility
	// API.  Callers use this capability check to replay function_call and
	// function_call_output items instead of sending previous_response_id.
	static bool configure_response_state(provider_config_t &provider,
					     const std::string &mode,
					     std::string &err);
	static bool responses_are_stateless(const provider_config_t &provider);
	static bool
	supports_reasoning_effort(const provider_config_t &provider);
	static bool test_connection(const provider_config_t &provider,
				    const std::string &api_key,
				    provider_test_result_t &result,
				    std::string &err);
	// Probe a provider's non-generation account endpoint when one is known.
	// This deliberately never sends a chat/completion request, avoiding token
	// charges and preserving scarce model RPM for the actual coding task.
	static bool probe_usage_limits(const provider_config_t &provider,
				       const std::string &api_key,
				       provider_usage_probe_t &result,
				       std::string &err);
	static bool complete(const provider_config_t &provider,
			     const std::string &api_key,
			     const completion_request_t &input,
			     completion_result_t &result, std::string &err,
			     completion_stream_observer_t *observer = NULL);
	// Responses lifecycle primitives used by durable long-running coding tasks.
	// The methods reject non-Responses providers and never expose credentials in
	// diagnostics. retrieve_response waits only for one HTTP operation; callers
	// remain responsible for bounded, cancellable polling.
	static bool retrieve_response(const provider_config_t &provider,
				      const std::string &api_key,
				      const std::string &response_id,
				      completion_result_t &result,
				      std::string &err);
	static bool cancel_response(const provider_config_t &provider,
				    const std::string &api_key,
				    const std::string &response_id,
				    completion_result_t &result,
				    std::string &err);
	static bool compact_response(
		const provider_config_t &provider, const std::string &api_key,
		const std::string &previous_response_id,
		const std::string &instructions, std::string &compaction_id,
		std::string &compacted_output_json, long long &input_tokens,
		long long &output_tokens, std::string &err);

	// Parses a provider response without network access. Kept public so the
	// protocol adapters can be regression-tested with recorded minimal fixtures.
	static bool parse_completion_response(const provider_config_t &provider,
					      const std::string &body,
					      completion_result_t &result,
					      std::string &err);
	// Offline SSE/NDJSON parser used by regression tests and replay diagnostics.
	static bool
	parse_stream_response(const provider_config_t &provider,
			      const std::string &body,
			      completion_result_t &result, std::string &err,
			      completion_stream_observer_t *observer = NULL);
	// Offline request-shape regression hook. The returned JSON can contain
	// prompts and image bytes and therefore must never be logged by callers.
	static bool
	serialize_completion_request(const provider_config_t &provider,
				     const completion_request_t &input,
				     bool stream, std::string &payload);
	// Convert a non-2xx provider response into a bounded, UI-safe diagnostic.
	// Only common error fields are extracted from JSON; the raw provider body is
	// deliberately never returned because it may contain sensitive information.
	static std::string
	describe_http_error(int status, const std::string &body,
			    const std::string &request_id = "",
			    const std::string &retry_after = "",
			    const std::string &reset_requests = "",
			    const std::string &reset_tokens = "");
	static const char *
	error_category_name(provider_error_category_t category);
};

} // namespace ai
} // namespace webcool
