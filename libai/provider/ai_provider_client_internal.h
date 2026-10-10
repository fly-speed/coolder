#pragma once

#include "ai_provider_client.h"
#include <cstddef>
#include <string>
#include <vector>

namespace acl
{
// JSON document type supplied by the ACL library.
class json;
// JSON node type supplied by the ACL library.
class json_node;
// HTTP header collection type supplied by the ACL library.
class http_header;
// HTTP request and connection type supplied by the ACL library.
class http_request;
// TLS configuration type supplied by the ACL library.
class openssl_conf;
}

namespace webcool
{
namespace ai
{
// Private implementation details shared by the provider client translation units.
namespace provider_detail
{

// Reasoning streams have substantial SSE framing overhead. Transport bytes,
// final visible output and individual events therefore need separate limits.
const size_t kMaxCompletionBytes = 4 * 1024 * 1024;
// Upper bound for stream wire bytes.
const size_t kMaxStreamWireBytes = 32 * 1024 * 1024;
// Upper bound for stream event bytes.
const size_t kMaxStreamEventBytes = 1024 * 1024;
// Upper bound for tool arguments bytes.
const size_t kMaxToolArgumentsBytes = 512 * 1024;

// All provider adapters share this normalized URL representation. The API key
// is added only as an HTTP header or provider-required query parameter at the
// final request step and is never included in diagnostic endpoint strings.
struct parsed_url_t {
	// Whether this connection uses TLS.
	bool use_ssl;
	// Network address used to establish the connection.
	std::string address;
	// Host name extracted from the provider URL.
	std::string host;
	// Server identity supplied to TLS certificate verification.
	std::string verify_host;
	// HTTP request path extracted from the configured URL.
	std::string path;
};

// Bounded counters and metadata explaining a streamed response.
struct stream_diagnostics_t {
	// Number of events observed for this operation.
	size_t event_count = 0;
	// Bytes of reasoning received so far.
	size_t reasoning_bytes = 0;
	// Number of reasoning stream events observed.
	size_t reasoning_events = 0;
	// Provider-supplied reason generation stopped.
	std::string finish_reason;
	// Type of the most recently parsed stream event.
	std::string last_event_type;
};

// Incrementally assembled native tool call from provider stream events.
struct streamed_tool_call_t {
	// Lookup key for the associated resource or record.
	std::string key;
	// Identifier used to look up this record.
	std::string id;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Arguments passed to the selected command or tool.
	std::string arguments;
};

// Common helpers.
// Apply the provider's learned output ceiling to the requested token limit.
long long effective_provider_output_limit(
    const provider_config_t &provider, long long requested);

// Persist a provider output ceiling learned from a rejected request.
void remember_provider_output_limit(
    const provider_config_t &provider, long long limit);

// Lowercase ASCII letters without locale-dependent conversions.
std::string lowercase_ascii(std::string value);

// Match the configured Kimi model against a known family name.
bool kimi_model_is(const provider_config_t &provider, const char *prefix);

// Identify Kimi models that do not support disabling thinking.
bool kimi_model_always_thinks(const provider_config_t &provider);

// Test a normalized model name for the requested capability marker.
bool model_name_contains(const provider_config_t &provider, const char *needle);

// Recognize DeepSeek's Responses compatibility endpoint.
bool deepseek_responses_endpoint(const provider_config_t &provider);

// Recognize Kimi's Responses compatibility endpoint.
bool kimi_responses_endpoint(const provider_config_t &provider);

// Recognize Qwen's Responses compatibility endpoint.
bool qwen_responses_endpoint(const provider_config_t &provider);

// Select the reasoning setting accepted by this Responses provider.
std::string responses_reasoning_effort(
    const provider_config_t &provider, const std::string &requested);

// Recognize request formats that carry OpenAI-style reasoning controls.
bool is_openai_reasoning_protocol(const provider_config_t &provider);

// Detect output exhaustion caused by reasoning without useful completion.
bool reasoning_budget_exhausted(const std::string &err);

// Return the endpoint used by the provider connection probe.
std::string test_url(const provider_config_t &provider);

// Build the supported Kimi account-balance endpoint URL.
std::string kimi_balance_url(const provider_config_t &provider);

// Return the protocol-specific endpoint for model generation.
std::string completion_url(const provider_config_t &provider, bool stream);

// Build the URL addressing one provider response identifier.
std::string responses_item_url(const provider_config_t &provider,
    const std::string &response_id, bool cancel);

// Build the provider's Responses compaction endpoint URL.
std::string responses_compact_url(const provider_config_t &provider);

// Read a JSON scalar as text, using the supplied fallback when applicable.
std::string node_text(acl::json_node *node);

// Read a JSON number with the caller's fallback for missing values.
long long node_number(acl::json_node *node);

// Read a JSON boolean with the caller's fallback for missing values.
bool node_bool(acl::json_node *node);

// Return the named child of an object when it exists.
acl::json_node *object_child(acl::json_node *node, const char *name);

// Return the first array element when one is available.
acl::json_node *first_array_item(acl::json_node *node);

// Return the array represented by this JSON node, or null.
acl::json_node *array_value(acl::json_node *node);

// Request helpers.
// Map a provider wire name back to the runtime's canonical tool name.
std::string canonical_tool_name(const std::string &wire);

// Encode the provider-neutral request using the selected wire protocol.
bool build_completion_payload(const provider_config_t &provider,
    const completion_request_t &input, bool stream, std::string &payload);

// Response helpers.
// Validate and decode the native call's JSON arguments.
bool parse_tool_arguments(
    acl::json_node *arguments, completion_tool_call_t &call);

// Populate compatibility fields from the first normalized native call.
void mirror_first_tool_call(completion_result_t &result);

// Normalize provider call arguments and append the identified native call.
void set_tool_call(const std::string &name, acl::json_node *arguments,
    completion_result_t &result, const std::string &id = "");

// Extract textual message content from the provider response shape.
std::string message_content_text(acl::json_node *content);

// Extract reasoning text from a Responses output item.
std::string responses_reasoning_text(acl::json_node *item);

// Convert a terminal Responses failure into a normalized diagnostic.
std::string responses_terminal_error(acl::json_node *response,
    std::string &status, std::string &incomplete_reason);

// Decode the parsed provider JSON into the normalized result.
bool parse_completion_json(const provider_config_t &provider, acl::json &json,
    completion_result_t &result, std::string &err);

// Decode a non-streaming provider response into the normalized result.
bool parse_completion(const provider_config_t &provider,
    const std::string &body, completion_result_t &result, std::string &err);

// Copy provider token-accounting fields into the normalized result.
void copy_usage(const completion_result_t &source, completion_result_t &target);
void parse_chat_cache_usage(acl::json_node *usage, completion_result_t &result);
void parse_anthropic_usage(acl::json_node *usage, completion_result_t &result);

// Replace partial stream state with a complete result and remaining deltas.
void adopt_completed_stream_result(const completion_result_t &completed,
    completion_result_t &result, std::string &text_delta,
    std::string &reasoning_delta);

// Stream helpers.
// Count buffered argument bytes across partially assembled native calls.
size_t streamed_tool_argument_bytes(
    const std::vector<streamed_tool_call_t> &calls);

// Validate and finalize tool calls assembled from stream fragments.
bool finalize_streamed_tool_calls(
    const std::vector<streamed_tool_call_t> &streamed_calls,
    completion_result_t &result, std::string &err);

// Consume one protocol stream line and update the partial result.
bool parse_stream_line(const provider_config_t &provider,
    const std::string &raw_line, completion_result_t &result,
    std::vector<streamed_tool_call_t> &streamed_calls, std::string &text_delta,
    std::string &reasoning_delta, stream_diagnostics_t &diagnostics,
    std::string &err);

// Diagnostics helpers.
// Adapt the request observer's cancellation check to the scheduler callback.
bool provider_request_cancelled(void *context);

// Map an HTTP or transport failure into the runtime's error categories.
void classify_error(
    int status, const std::string &err, completion_result_t &result);

// Bound diagnostic text before adding it to tool feedback.
std::string bounded_diagnostic_text(acl::json_node *node, size_t limit);

// Reduce transport diagnostics to a bounded displayable message.
std::string bounded_transport_error(const char *value, size_t limit);

// Calculate the bounded retry delay for provider rate limiting.
unsigned long rate_limit_retry_delay_seconds(
    const std::string &err, unsigned int attempt);

// Wait cooperatively before a rate-limit retry, honoring cancellation.
bool wait_for_rate_limit_retry(
    unsigned long seconds, completion_stream_observer_t *observer);

// Recognize transport failures caused by transient resource pressure.
bool temporary_resource_transport_error(const std::string &err);

// Return the retry allowance for initial provider transport failures.
unsigned int initial_transport_retry_limit(
    const std::string &err, provider_error_category_t category);

// Wait before a transport retry while checking for cancellation.
bool wait_for_transport_retry(unsigned int attempt,
    completion_stream_observer_t *observer, const std::string &err);

// Build an opaque key for shared provider request admission.
std::string provider_scheduler_key(
    const provider_config_t &provider, const std::string &api_key);

// Transport helpers.
// Parse an HTTP or HTTPS URL into the validated transport fields.
bool parse_http_url(
    const std::string &url, parsed_url_t &parsed, std::string &err);

// Apply TLS settings to the provider's HTTP request.
bool configure_tls(
    acl::openssl_conf &conf, const std::string &host, std::string &err);

// Attach protocol-specific authentication headers to the HTTP request.
void add_auth_headers(acl::http_header &header,
    const provider_config_t &provider, const std::string &api_key);

// Read an HTTP response header for diagnostics or rate-limit handling.
std::string response_header(acl::http_request &request, const char *name);

// Return the bounded timeout for opening a provider connection.
int provider_connect_timeout_seconds();

// Send a JSON request and collect the bounded HTTP response.
bool post_json(const provider_config_t &provider, const std::string &api_key,
    const std::string &url, const std::string &payload, acl::json &body,
    int &status, long long &latency_ms, std::string &err);

// Fetch and parse a provider JSON document while recording HTTP status and
// timing.
bool get_json(const provider_config_t &provider, const std::string &api_key,
    const std::string &url, acl::json &body, int &status, long long &latency_ms,
    std::string &err);

// Send a JSON request and consume its response incrementally.
bool post_json_stream(const provider_config_t &provider,
    const std::string &api_key, const std::string &url,
    const std::string &payload, completion_result_t &result,
    completion_stream_observer_t &observer, int &status, long long &latency_ms,
    std::string &err,
    const std::shared_ptr<provider_transport_session_t> &session);

} // namespace provider_detail
} // namespace ai
} // namespace webcool
