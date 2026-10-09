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

// Task-scoped HTTP connection state shared by consecutive provider calls.
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

// Connection-test timing and verified provider capabilities.
struct provider_test_result_t {
	// Endpoint is returned for diagnostics but never contains an API key.
	int http_status;
	// Elapsed request time in milliseconds.
	long long latency_ms;
	// URL used for this provider operation; excludes credentials.
	std::string endpoint;
	// Whether the configured model passed the connection probe.
	bool model_verified = false;
	// Whether the Responses protocol passed its capability probe.
	bool responses_verified = false;
	// Whether the provider accepted the tool capability probe.
	bool tools_verified = false;
};

// Lightweight account/rate-limit information collected before a coding run.
// Providers do not expose a uniform limits API, so every field carries an
// explicit "known" flag and unknown must never be interpreted as unlimited.
struct provider_usage_probe_t {
	// Whether the requested capability is implemented or available.
	bool supported = false;
	// Whether the capability or usage probe was attempted.
	bool checked = false;
	// Whether the reported balance is available from the provider.
	bool balance_known = false;
	// Whether the usage probe permits starting a generation request.
	bool can_start = true;
	// HTTP status received from the provider or remote endpoint.
	int http_status = 0;
	// Elapsed request time in milliseconds.
	long long latency_ms = 0;
	// Provider-reported balance, meaningful only when balance_known.
	double available_balance = 0.0;
	// Currency label supplied with the provider balance.
	std::string currency;
	// URL used for this provider operation; excludes credentials.
	std::string endpoint;
	// Provider-reported request quota limit.
	std::string request_limit;
	// Provider-reported remaining request allowance.
	std::string request_remaining;
	// Provider-reported request quota reset value.
	std::string request_reset;
	// Provider-reported token quota limit.
	std::string token_limit;
	// Provider-reported remaining token allowance.
	std::string token_remaining;
	// Provider-reported token quota reset value.
	std::string token_reset;
	// Provider retry delay header retained in its original format.
	std::string retry_after;
	// Human-readable message associated with this record.
	std::string message;
};

// In-memory image supplied with one provider request. Temporary disk files are
// removed before the asynchronous run begins; only these bounded bytes remain.
struct completion_image_t {
	// Name used to identify this item in its containing collection.
	std::string name;
	// Media type describing the attached image bytes.
	std::string mime_type;
	// Payload bytes retained in memory.
	std::string data;
};

// Result of one provider-native function call. OpenAI Responses can continue
// directly from a response ID when every call result is returned with its
// original call ID, avoiding retransmission of the complete tool transcript.
struct completion_tool_output_t {
	// Provider-issued identifier linking a tool result to its call.
	std::string call_id;
	// Result payload returned by the operation.
	std::string output;
};

// One normalized provider-native tool call. Providers can emit several calls
// in a single assistant turn; retaining the complete ordered set is required
// before the runtime can safely execute independent reads in parallel.
struct completion_tool_call_t {
	// Identifier used to look up this record.
	std::string id;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Path of the file or resource associated with this record.
	std::string path;
	// Search expression or query supplied to the tool.
	std::string query;
	// Exact source text to locate before applying a replacement.
	std::string old_text;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Text payload associated with this operation.
	std::string content;
	// Whether the request explicitly supplied a content string.
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
	// Provider reasoning retained for replaying the assistant turn.
	std::string reasoning_content;
	// Ordered provider-native tool calls in this assistant turn.
	std::vector<completion_tool_call_t> calls;
	// Results paired with the tool calls in this exchange.
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
	// User-supplied task text for this request.
	std::string user_prompt;
	// Upper bound on generated tokens for one provider request.
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
	// Optional service-tier selection passed to the provider.
	std::string service_tier;
	// Requested verbosity of the provider's text response.
	std::string text_verbosity;
	// Requested form of the provider's reasoning summary.
	std::string reasoning_summary;
	// Provider-specific prompt cache retention setting.
	std::string prompt_cache_ttl;
	// Run identifier attached to provider request metadata.
	std::string metadata_run_id;
	// Session identifier attached to provider request metadata.
	std::string metadata_session_id;
	// Whether the Responses provider may retain the response.
	bool store = true;
	// Whether to request asynchronous background generation.
	bool background = false;
	// Whether to request provider-side context compaction.
	bool compact_context = false;
	// Whether to request strict provider tool schemas.
	bool strict_tools = true;
	// Schemas come from the agent registry. Provider adapters only translate the
	// neutral descriptions and never maintain a second hard-coded tool catalog.
	std::vector<agent_tool_t> tools;
	// Progress-repair turns have already gathered the required context and must
	// produce one concrete tool action. Protocol adapters translate this into the
	// provider's native required-tool policy when tools are available.
	bool require_tool_call = false;
	// Images supplied with or produced by this operation.
	std::vector<completion_image_t> images;
	// Native continuation is currently consumed only by the official Responses
	// protocol. Other adapters deliberately ignore it and keep their compatible
	// full-context fallback.
	std::string previous_response_id;
	// Native tool results sent with the next provider request.
	std::vector<completion_tool_output_t> tool_outputs;
	// Prior native assistant/tool exchanges retained for replay.
	std::vector<completion_tool_exchange_t> tool_history;
};

// Transient completion result. Text and tool arguments are kept in memory only
// and are excluded from the persistent agent run audit.
struct completion_result_t {
	// Whether malformed tool arguments have already been retried.
	bool tool_arguments_recovery_attempted = false;
	// Whether this request already used its transport retry.
	bool transport_retry_attempted = false;
	// Whether the request reused an existing HTTP connection.
	bool connection_reused = false;
	// Bounded failed-response diagnostic for the owning project's audit only.
	// Never add it to a model prompt or the normal browser result serializer.
	std::string error_response_excerpt;
	// Content-Type header of the failed provider response.
	std::string error_response_content_type;
	// Content-Encoding header of the failed provider response.
	std::string error_response_content_encoding;
	// Text content presented or retained by this record.
	std::string text;
	// Opaque provider identifier used only for the immediately following native
	// continuation request; it is not a secret and is never used across users.
	std::string response_id;
	// Provider-reported lifecycle state of the response.
	std::string response_status;
	// Provider explanation for an incomplete response.
	std::string incomplete_reason;
	// Provider-supplied reasoning/thinking text is kept separate from the final
	// answer so callers can present it in a collapsible, clearly labelled view.
	std::string reasoning;
	// Provider-reported input token count.
	long long input_tokens = 0;
	// Input tokens served from the provider's prompt cache.
	long long cached_input_tokens = 0;
	// Provider-reported generated token count.
	long long output_tokens = 0;
	// Generated tokens attributed to provider reasoning.
	long long reasoning_tokens = 0;
	// Actual value sent after applying a provider capability learned from a
	// rejected request. This can be lower than the administrator policy ceiling.
	long long effective_max_output_tokens = 0;
	// Elapsed request time in milliseconds.
	long long latency_ms = 0;
	// HTTP status received from the provider or remote endpoint.
	int http_status = 0;
	// Normalized failure category used by recovery policy.
	provider_error_category_t error_category = provider_error_none;
	// Whether the provider failure is eligible for retry.
	bool retryable_error = false;
	// True when this logical model turn first exhausted its output budget in
	// reasoning and succeeded only after the client forced a direct/no-thinking
	// recovery request. The agent loop uses this signal to keep later turns in
	// direct mode instead of immediately falling back into the same costly loop.
	bool reasoning_budget_recovered = false;
	// Whether the result contains provider-native function calls.
	bool native_tool_call = false;
	// Tool calls reported or retained for this operation.
	std::vector<completion_tool_call_t> tool_calls;
	// Compatibility mirror of tool_calls.front(). New agent code consumes the
	// vector; these fields keep older callers and stored test fixtures readable.
	std::string tool_name;
	// Path argument mirrored from the first native tool call.
	std::string tool_path;
	// Query argument mirrored from the first native tool call.
	std::string tool_query;
	// Replacement source mirrored from the first native tool call.
	std::string tool_old_text;
	// Destination mirrored from the first native tool call.
	std::string tool_target_path;
	// Content argument mirrored from the first native tool call.
	std::string tool_content;
};

// Receives normalized text fragments while the provider response is arriving.
// Returning false aborts the HTTP body read; observers must not persist or log
// fragments because they can contain source code or other sensitive text.
class completion_stream_observer_t {
public:
	// Allow concrete observers to be destroyed through this interface.
	virtual ~completion_stream_observer_t()
	{
	}
	// Called immediately before each HTTP request, including transport fallbacks
	// and automatic reasoning retries. Agent runtimes use this hook to archive
	// the exact JSON body in the selected project. API keys are HTTP headers and
	// are therefore never present in payload.
	virtual bool on_request_payload(
	    const std::string &payload, std::string &err)
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
	// Receive timing and byte counters without retaining source text.
	virtual void on_stream_progress(
	    const provider_stream_progress_t &progress)
	{
		(void)progress;
	}
	// Consume one text fragment; return false to stop reading the
	// response.
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
	virtual bool on_response_state(
	    const std::string &response_id, const std::string &status)
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
	// Create task-scoped connection state for provider HTTP reuse.
	static std::shared_ptr<provider_transport_session_t>
	create_transport_session();
	// DeepSeek and Kimi expose the Responses wire format as a stateless compatibility
	// API.  Callers use this capability check to replay function_call and
	// function_call_output items instead of sending previous_response_id.
	static bool configure_response_state(provider_config_t &provider,
	    const std::string &mode, std::string &err);
	// Report whether the provider requires replay instead of response-ID
	// continuation.
	static bool responses_are_stateless(const provider_config_t &provider);
	// Check whether this provider/model accepts reasoning-effort
	// controls.
	static bool supports_reasoning_effort(
	    const provider_config_t &provider);
	// Probe the configured endpoint and model without starting an agent
	// run.
	static bool test_connection(const provider_config_t &provider,
	    const std::string &api_key, provider_test_result_t &result,
	    std::string &err);
	// Probe a provider's non-generation account endpoint when one is known.
	// This deliberately never sends a chat/completion request, avoiding token
	// charges and preserving scarce model RPM for the actual coding task.
	static bool probe_usage_limits(const provider_config_t &provider,
	    const std::string &api_key, provider_usage_probe_t &result,
	    std::string &err);
	// Perform a model request, optionally stream deltas, and return its
	// normalized result.
	static bool complete(const provider_config_t &provider,
	    const std::string &api_key, const completion_request_t &input,
	    completion_result_t &result, std::string &err,
	    completion_stream_observer_t *observer = NULL);
	// Responses lifecycle primitives used by durable long-running coding tasks.
	// The methods reject non-Responses providers and never expose credentials in
	// diagnostics. retrieve_response waits only for one HTTP operation; callers
	// remain responsible for bounded, cancellable polling.
	static bool retrieve_response(const provider_config_t &provider,
	    const std::string &api_key, const std::string &response_id,
	    completion_result_t &result, std::string &err);
	// Ask a Responses provider to cancel the identified background
	// response.
	static bool cancel_response(const provider_config_t &provider,
	    const std::string &api_key, const std::string &response_id,
	    completion_result_t &result, std::string &err);
	// Compact a Responses conversation and return its continuation state.
	static bool compact_response(const provider_config_t &provider,
	    const std::string &api_key, const std::string &previous_response_id,
	    const std::string &instructions, std::string &compaction_id,
	    std::string &compacted_output_json, long long &input_tokens,
	    long long &output_tokens, std::string &err);

	// Parses a provider response without network access. Kept public so the
	// protocol adapters can be regression-tested with recorded minimal fixtures.
	static bool parse_completion_response(const provider_config_t &provider,
	    const std::string &body, completion_result_t &result,
	    std::string &err);
	// Offline SSE/NDJSON parser used by regression tests and replay diagnostics.
	static bool parse_stream_response(const provider_config_t &provider,
	    const std::string &body, completion_result_t &result,
	    std::string &err, completion_stream_observer_t *observer = NULL);
	// Offline request-shape regression hook. The returned JSON can contain
	// prompts and image bytes and therefore must never be logged by callers.
	static bool serialize_completion_request(
	    const provider_config_t &provider,
	    const completion_request_t &input, bool stream,
	    std::string &payload);
	// Convert a non-2xx provider response into a bounded, UI-safe diagnostic.
	// Only common error fields are extracted from JSON; the raw provider body is
	// deliberately never returned because it may contain sensitive information.
	static std::string describe_http_error(int status,
	    const std::string &body, const std::string &request_id = "",
	    const std::string &retry_after = "",
	    const std::string &reset_requests = "",
	    const std::string &reset_tokens = "");
	// Return the stable diagnostic name for a normalized provider
	// failure.
	static const char *error_category_name(
	    provider_error_category_t category);
};

} // namespace ai
} // namespace webcool
