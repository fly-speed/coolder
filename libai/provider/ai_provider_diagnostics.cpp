#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include "fiber/fiber_base.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>

namespace webcool {
namespace ai {
namespace provider_detail {

void classify_error(int status, const std::string& err,
	completion_result_t& result)
{
	result.http_status = status;
	if (err == "AI provider returned invalid tool arguments (expected JSON object)") {
		result.error_category = provider_error_protocol;
		result.retryable_error = false;
		return;
	}
	result.retryable_error = false;
	// Local policy deadlines are terminal for this request, including runtime
	// recovery paths that classify errors again after the transport returns.
	if (err.find("AI provider request_total_timeout") == 0
		|| err.find("AI provider connection_idle_timeout") == 0
		|| err.find("AI provider effective_output_stalled") == 0) {
		result.error_category = provider_error_timeout;
		return;
	}
	if (status == 401 || status == 403) {
		result.error_category = provider_error_authentication;
	} else if (status == 408) {
		result.error_category = provider_error_timeout;
		result.retryable_error = true;
	} else if (status == 429 || (err.compare(0, strlen("AI provider streaming error"), "AI provider streaming error") == 0
		&& (err.find("(code=rate_limit_exceeded)") != std::string::npos
			|| err.find("(code=overloaded)") != std::string::npos
			|| err.find("(code=insufficient_quota)") != std::string::npos))) {
		result.error_category = provider_error_rate_limit;
		// Billing and quota 429 responses require account changes. Retrying them
		// wastes the user's time and also consumes rate-limit capacity.
		result.retryable_error = err.find("credit_balance_exhausted")
			== std::string::npos
			&& err.find("exceeded_current_quota_error") == std::string::npos
			&& err.find("spend_limit_exceeded") == std::string::npos
			&& err.find("usage_limit_exceeded") == std::string::npos
			&& err.find("insufficient_quota") == std::string::npos;
	} else if (status == 400 || status == 404 || status == 409
		|| status == 422)
	{
		result.error_category = provider_error_invalid_request;
	} else if (status >= 500 && status <= 599) {
		result.error_category = provider_error_unavailable;
		result.retryable_error = true;
	} else if (err.find("exceeds 4 MiB") != std::string::npos
		|| err.find("exceeds 32 MiB") != std::string::npos) {
		result.error_category = provider_error_response_limit;
	} else if (reasoning_budget_exhausted(err)) {
		result.error_category = provider_error_response_limit;
	} else if (err.find("invalid streaming") != std::string::npos
		|| err.find("invalid streamed") != std::string::npos
		|| err.find("streaming response contains no output") != std::string::npos
		|| err.find("streaming response contains no visible") != std::string::npos)
	{
		result.error_category = provider_error_protocol;
	} else if (err.find("cannot read AI provider streaming response")
		!= std::string::npos)
	{
		// The request reached the provider and received HTTP success, but the
		// socket/TLS stream ended before the response body completed. HTTP status
		// alone therefore cannot classify this transport-level failure. Check this
		// before the generic timeout wording: the diagnostic field name
		// `stream_idle_timeout_seconds` is present even for ECONNRESET and must not
		// turn a peer reset into a timeout classification.
		result.error_category = provider_error_network;
		result.retryable_error = true;
	} else if (err.find("timed out") != std::string::npos
		|| (err.find("request failed") == std::string::npos
			&& err.find("timeout") != std::string::npos))
	{
		result.error_category = provider_error_timeout;
		result.retryable_error = true;
	} else if (status == 0 && err.find("request failed") != std::string::npos) {
		result.error_category = provider_error_network;
		result.retryable_error = true;
	} else result.error_category = provider_error_unknown;
}

std::string bounded_diagnostic_text(acl::json_node* node, size_t limit) {
	if (node == NULL || node->is_null()) return "";
	const char* value = node->is_string() ? node->get_string() : node->get_text();
	if (value == NULL) return "";
	std::string safe;
	for (size_t i = 0; value[i] != '\0' && safe.size() < limit; ++i) {
		const unsigned char ch = static_cast<unsigned char>(value[i]);
		if (ch == '\r' || ch == '\n' || ch == '\t') safe.push_back(' ');
		else if (ch >= 32 && ch != 127) safe.push_back(static_cast<char>(ch));
	}
	return safe;
}

std::string compatible_error_message(acl::json_node* node, unsigned depth = 0) {
	if (node == NULL || depth > 4) return "";
	if (node->is_string()) return bounded_diagnostic_text(node, 768);
	const char* fields[] = {"message", "msg", "detail", "error_message",
		"error_description", "error", "errors"};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		const std::string message = compatible_error_message(
			object_child(node, fields[i]), depth + 1);
		if (!message.empty()) return message;
	}
	acl::json_node* array = node->is_array() ? node : node->get_obj();
	if (array != NULL && array->is_array()) {
		unsigned count = 0;
		for (acl::json_node* item = array->first_child(); item && count++ < 8;
			item = array->next_child()) {
			const std::string message = compatible_error_message(item, depth + 1);
			if (!message.empty()) return message;
		}
	}
	return "";
}

std::string bounded_transport_error(const char* value, size_t limit) {
	if (value == NULL) return "";
	std::string safe;
	for (size_t i = 0; value[i] != '\0' && safe.size() < limit; ++i) {
		const unsigned char ch = static_cast<unsigned char>(value[i]);
		if (ch == '\r' || ch == '\n' || ch == '\t') safe.push_back(' ');
		else if (ch >= 32 && ch != 127) safe.push_back(static_cast<char>(ch));
	}
	return safe;
}

void append_http_detail(std::string& details, const char* name,
	const std::string& value)
{
	if (value.empty()) return;
	if (!details.empty()) details += ", ";
	details += name;
	details += "=";
	details += value;
}

unsigned long rate_limit_retry_delay_seconds(const std::string& err,
	unsigned int attempt)
{
	// Prefer the standardized Retry-After detail retained by describe_http_error.
	// Kimi also repeats the delay in its message, so keep that as a fallback for
	// gateways which strip response headers.
	const char* markers[] = { "retry_after=", "try again after " };
	for (size_t marker_index = 0;
		marker_index < sizeof(markers) / sizeof(markers[0]); ++marker_index)
	{
		const size_t marker = err.find(markers[marker_index]);
		if (marker == std::string::npos) continue;
		size_t cursor = marker + strlen(markers[marker_index]);
		unsigned long seconds = 0;
		bool found_digit = false;
		while (cursor < err.size() && err[cursor] >= '0' && err[cursor] <= '9') {
			found_digit = true;
			seconds = seconds * 10 + static_cast<unsigned long>(err[cursor] - '0');
			if (seconds >= 60) return 60;
			++cursor;
		}
		if (found_digit) return std::max(1UL, seconds);
	}
	// Explicit engine overload inside SSE needs more recovery time than a
	// one-second token-bucket boundary. Keep the same three-retry upper bound.
	if (err.compare(0, strlen("AI provider streaming error"),
		"AI provider streaming error") == 0)
		return 5UL << std::min(attempt, 2U);
	// A missing header gets a small bounded exponential backoff. Provider calls
	// run in fibers, so this wait does not block unrelated WebCool users.
	return std::min(8UL, 1UL << std::min(attempt, 3U));
}

bool wait_for_rate_limit_retry(unsigned long seconds,
	completion_stream_observer_t* observer)
{
	unsigned long remaining_ms = std::max(1UL, seconds) * 1000UL;
	while (remaining_ms > 0) {
		if (observer != NULL && observer->cancel_requested()) return false;
		const unsigned long slice = std::min(remaining_ms, 250UL);
		acl::fiber::delay(slice);
		remaining_ms -= slice;
	}
	return observer == NULL || !observer->cancel_requested();
}

bool temporary_resource_transport_error(const std::string& err) {
	// EAGAIN/EWOULDBLOCK has different numeric values across macOS, Linux and
	// Winsock. Match ACL's stable human-readable form instead of hard-coding one
	// platform's errno, while keeping the retry narrowly scoped to resource
	// pressure rather than replaying every ambiguous network failure.
	const std::string lower = lowercase_ascii(err);
	return lower.find("resource temporarily unavailable") != std::string::npos
		|| lower.find("operation would block") != std::string::npos
		|| lower.find("would block") != std::string::npos;
}

bool connection_phase_error(const std::string& lower) {
    return lower.find("phase=connect-send-or-wait-response-headers") != std::string::npos
        || lower.find("phase=dns") != std::string::npos
        || lower.find("phase=tcp_connect") != std::string::npos
        || lower.find("phase=tls_handshake") != std::string::npos;
}

unsigned int initial_transport_retry_limit(const std::string& err,
	provider_error_category_t category)
{
	if (category == provider_error_network
		&& temporary_resource_transport_error(err)) return 2;
	// A connect timeout happens before an HTTP status is available. Retry it once
	// after a short fiber-aware delay, but do not broadly replay arbitrary network
	// failures because a POST whose delivery is ambiguous may already be billed.
	const std::string lower = lowercase_ascii(err);
	if (category == provider_error_timeout
		&& connection_phase_error(lower)
		&& lower.find("timed out") != std::string::npos) return 1;
	return 0;
}

bool wait_for_transport_retry(unsigned int attempt,
	completion_stream_observer_t* observer, const std::string& err)
{
	// A failed connect/TLS handshake benefits from a real backoff; retrying after
	// only 250 ms repeatedly hit the same Kimi edge-node failure. Local descriptor
	// pressure still uses the shorter delay so server resource recovery remains
	// responsive. Fiber delays do not block other WebCool users.
	const std::string lower = lowercase_ascii(err);
	const bool connect_timeout = connection_phase_error(lower)
		&& lower.find("timed out") != std::string::npos;
	unsigned long remaining_ms = connect_timeout
		? (attempt == 0 ? 2000UL : 5000UL)
		: (attempt == 0 ? 250UL : 750UL);
	while (remaining_ms > 0) {
		if (observer != NULL && observer->cancel_requested()) return false;
		const unsigned long slice = std::min(remaining_ms, 125UL);
		acl::fiber::delay(slice);
		remaining_ms -= slice;
	}
	return observer == NULL || !observer->cancel_requested();
}

bool provider_request_cancelled(void* context) {
	completion_stream_observer_t* observer =
		static_cast<completion_stream_observer_t*>(context);
	return observer != NULL && observer->cancel_requested();
}

std::string provider_scheduler_key(const provider_config_t& provider,
	const std::string& api_key)
{
	// std::hash is used only as an opaque, process-local grouping token. The API
	// key itself is never retained by the scheduler or included in diagnostics.
	// Endpoint and model keep unrelated accounts/deployments in separate queues.
	std::ostringstream out;
	out << provider.protocol << '\n' << provider.base_url << '\n'
		<< provider.model << '\n' << std::hex
		<< static_cast<unsigned long long>(std::hash<std::string>()(api_key));
	return out.str();
}

} // namespace provider_detail

using namespace provider_detail;

std::string provider_client_t::describe_http_error(int status,
	const std::string& body, const std::string& request_id,
	const std::string& retry_after, const std::string& reset_requests,
	const std::string& reset_tokens)
{
	std::string message;
	std::string type;
	std::string code;
	std::string body_request_id;
	std::string body_fields;
	bool recognized_json = false;
	const size_t first = body.find_first_not_of(" \r\n\t");
	if (first != std::string::npos && (body[first] == '{'
		|| body[first] == '[' || body[first] == '"')) {
		acl::json parsed(body.c_str());
		if (parsed.finish()) {
			recognized_json = true;
			acl::json_node* error = parsed["error"];
			if (error != NULL && error->is_string()) {
				// Ollama and several OpenAI-compatible gateways use {"error":"..."}.
				message = bounded_diagnostic_text(error, 768);
			} else if (error != NULL) {
				message = bounded_diagnostic_text(object_child(error, "message"), 768);
				type = bounded_diagnostic_text(object_child(error, "type"), 128);
				code = bounded_diagnostic_text(object_child(error, "code"), 128);
				// Gemini uses status where OpenAI uses type/code.
				if (type.empty()) type = bounded_diagnostic_text(
					object_child(error, "status"), 128);
			}
			// Some compatible APIs put these fields at the document root.
			if (message.empty()) message = bounded_diagnostic_text(parsed["message"], 768);
			if (message.empty()) message = compatible_error_message(&parsed.get_root());
			if (type.empty()) type = bounded_diagnostic_text(parsed["type"], 128);
			if (code.empty()) code = bounded_diagnostic_text(parsed["code"], 128);
			body_request_id = bounded_diagnostic_text(parsed["request_id"], 128);
			// Unknown schemas still leave a structural diagnostic. Do not dump
			// arbitrary response values, which may echo credentials or source.
			acl::json_node& root = parsed.get_root();
			for (acl::json_node* field = root.first_child(); field
				&& body_fields.size() < 256; field = root.next_child()) {
				const char* tag = field->tag_name();
				if (!tag) continue;
				if (!body_fields.empty()) body_fields += ",";
				for (size_t i = 0; tag[i] && i < 32; ++i) {
					const unsigned char ch = static_cast<unsigned char>(tag[i]);
					body_fields += std::isalnum(ch) || ch == '_' ? tag[i] : '?';
				}
			}
		}
	}

	std::string description = "AI provider returned HTTP "
		+ std::to_string(status);
	if (!message.empty()) description += ": " + message;
	else if (!body.empty() && !recognized_json) {
		description += ": provider returned an unrecognized non-JSON error body";
		if (body.find("Failed to parse the request body as JSON:") == 0)
			description += ": " + bounded_transport_error(body.c_str(), 768);
	} else if (!body.empty()) {
		description += ": provider JSON contained no recognized error details";
	} else if (body.empty()) {
		description += ": provider returned no readable error details";
	}

	std::string details;
	append_http_detail(details, "body_bytes", std::to_string(body.size()));
	if (message.empty()) append_http_detail(details, "json_fields", body_fields);
	append_http_detail(details, "code", code);
	append_http_detail(details, "type", type);
	append_http_detail(details, "request_id", request_id.empty()
		? body_request_id : request_id.substr(0, 128));
	append_http_detail(details, "retry_after", retry_after.substr(0, 64));
	append_http_detail(details, "reset_requests", reset_requests.substr(0, 64));
	append_http_detail(details, "reset_tokens", reset_tokens.substr(0, 64));
	if (!details.empty()) description += " [" + details + "]";
	return description;
}

const char* provider_client_t::error_category_name(
	provider_error_category_t category)
{
	switch (category) {
	case provider_error_none: return "none";
	case provider_error_network: return "network";
	case provider_error_timeout: return "timeout";
	case provider_error_authentication: return "authentication";
	case provider_error_rate_limit: return "rate_limit";
	case provider_error_invalid_request: return "invalid_request";
	case provider_error_unavailable: return "unavailable";
	case provider_error_response_limit: return "response_limit";
	case provider_error_protocol: return "protocol";
	case provider_error_cancelled: return "cancelled";
	default: return "unknown";
	}
}


} // namespace ai
} // namespace webcool
