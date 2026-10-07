#pragma once

#include "ai_provider_client.h"
#include <cstddef>
#include <string>
#include <vector>

namespace acl {
class json;
class json_node;
class http_header;
class http_request;
class openssl_conf;
}

namespace webcool {
namespace ai {
// Private implementation details shared by the provider client translation units.
namespace provider_detail {

// Reasoning streams have substantial SSE framing overhead. Transport bytes,
// final visible output and individual events therefore need separate limits.
const size_t kMaxCompletionBytes = 4 * 1024 * 1024;
const size_t kMaxStreamWireBytes = 32 * 1024 * 1024;
const size_t kMaxStreamEventBytes = 1024 * 1024;
const size_t kMaxToolArgumentsBytes = 512 * 1024;

// All provider adapters share this normalized URL representation. The API key
// is added only as an HTTP header or provider-required query parameter at the
// final request step and is never included in diagnostic endpoint strings.
struct parsed_url_t {
	bool use_ssl;
	std::string address;
	std::string host;
	std::string verify_host;
	std::string path;
};

struct stream_diagnostics_t {
	size_t event_count = 0;
	size_t reasoning_bytes = 0;
	size_t reasoning_events = 0;
	std::string finish_reason;
	std::string last_event_type;
};

struct streamed_tool_call_t {
	std::string key;
	std::string id;
	std::string name;
	std::string arguments;
};

// Common helpers.
long long effective_provider_output_limit(const provider_config_t& provider,
	long long requested);

void remember_provider_output_limit(const provider_config_t& provider,
	long long limit);

std::string lowercase_ascii(std::string value);

bool kimi_model_is(const provider_config_t& provider, const char* prefix);

bool kimi_model_always_thinks(const provider_config_t& provider);

bool model_name_contains(const provider_config_t& provider, const char* needle);

bool deepseek_responses_endpoint(const provider_config_t& provider);

bool kimi_responses_endpoint(const provider_config_t& provider);

bool qwen_responses_endpoint(const provider_config_t& provider);

std::string responses_reasoning_effort(const provider_config_t& provider,
	const std::string& requested);

bool is_openai_reasoning_protocol(const provider_config_t& provider);

bool reasoning_budget_exhausted(const std::string& err);

std::string test_url(const provider_config_t& provider);

std::string kimi_balance_url(const provider_config_t& provider);

std::string completion_url(const provider_config_t& provider, bool stream);

std::string responses_item_url(const provider_config_t& provider,
	const std::string& response_id, bool cancel);

std::string responses_compact_url(const provider_config_t& provider);

std::string node_text(acl::json_node* node);

long long node_number(acl::json_node* node);

bool node_bool(acl::json_node* node);

acl::json_node* object_child(acl::json_node* node, const char* name);

acl::json_node* first_array_item(acl::json_node* node);

acl::json_node* array_value(acl::json_node* node);

// Request helpers.
std::string canonical_tool_name(const std::string& wire);

bool build_completion_payload(const provider_config_t& provider,
	const completion_request_t& input, bool stream, std::string& payload);

// Response helpers.
bool parse_tool_arguments(acl::json_node* arguments,
	completion_tool_call_t& call);

void mirror_first_tool_call(completion_result_t& result);

void set_tool_call(const std::string& name, acl::json_node* arguments,
	completion_result_t& result, const std::string& id = "");

std::string message_content_text(acl::json_node* content);

std::string responses_reasoning_text(acl::json_node* item);

std::string responses_terminal_error(acl::json_node* response,
	std::string& status, std::string& incomplete_reason);

bool parse_completion_json(const provider_config_t& provider,
	acl::json& json, completion_result_t& result, std::string& err);

bool parse_completion(const provider_config_t& provider,
	const std::string& body, completion_result_t& result, std::string& err);

void copy_usage(const completion_result_t& source,
	completion_result_t& target);

void adopt_completed_stream_result(const completion_result_t& completed,
	completion_result_t& result, std::string& text_delta,
	std::string& reasoning_delta);

// Stream helpers.
size_t streamed_tool_argument_bytes(
	const std::vector<streamed_tool_call_t>& calls);

bool finalize_streamed_tool_calls(
	const std::vector<streamed_tool_call_t>& streamed_calls,
	completion_result_t& result, std::string& err);

bool parse_stream_line(const provider_config_t& provider,
	const std::string& raw_line, completion_result_t& result,
	std::vector<streamed_tool_call_t>& streamed_calls,
	std::string& text_delta,
	std::string& reasoning_delta,
	stream_diagnostics_t& diagnostics, std::string& err);

// Diagnostics helpers.
bool provider_request_cancelled(void* context);

void classify_error(int status, const std::string& err,
	completion_result_t& result);

std::string bounded_diagnostic_text(acl::json_node* node, size_t limit);

std::string bounded_transport_error(const char* value, size_t limit);

unsigned long rate_limit_retry_delay_seconds(const std::string& err,
	unsigned int attempt);

bool wait_for_rate_limit_retry(unsigned long seconds,
	completion_stream_observer_t* observer);

bool temporary_resource_transport_error(const std::string& err);

unsigned int initial_transport_retry_limit(const std::string& err,
	provider_error_category_t category);

bool wait_for_transport_retry(unsigned int attempt,
	completion_stream_observer_t* observer, const std::string& err);

std::string provider_scheduler_key(const provider_config_t& provider,
	const std::string& api_key);

// Transport helpers.
bool parse_http_url(const std::string& url, parsed_url_t& parsed,
	std::string& err);

bool configure_tls(acl::openssl_conf& conf, const std::string& host,
	std::string& err);

void add_auth_headers(acl::http_header& header,
	const provider_config_t& provider, const std::string& api_key);

std::string response_header(acl::http_request& request, const char* name);

int provider_connect_timeout_seconds();

bool post_json(const provider_config_t& provider, const std::string& api_key,
	const std::string& url, const std::string& payload, acl::json& body,
	int& status, long long& latency_ms, std::string& err);

bool get_json(const provider_config_t& provider, const std::string& api_key,
	const std::string& url, acl::json& body, int& status,
	long long& latency_ms, std::string& err);

bool post_json_stream(const provider_config_t& provider,
	const std::string& api_key, const std::string& url,
	const std::string& payload, completion_result_t& result,
	completion_stream_observer_t& observer, int& status,
	long long& latency_ms, std::string& err,
    const std::shared_ptr<provider_transport_session_t>& session);

} // namespace provider_detail
} // namespace ai
} // namespace webcool
