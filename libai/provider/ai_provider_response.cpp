#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include "../common/ai_error_log.h"

namespace webcool
{
namespace ai
{
namespace provider_detail
{

bool parse_tool_arguments(
    acl::json_node *arguments, completion_tool_call_t &call)
{
	acl::json_node *object = arguments && arguments->is_object() ?
	    arguments :
	    (arguments ? arguments->get_obj() : NULL);
	if (object != NULL && object->is_object()) {
		call.path = node_text((*object)["path"]);
		call.query = node_text((*object)["query"]);
		call.old_text = node_text((*object)["old_text"]);
		call.target_path = node_text((*object)["target_path"]);
		call.content_present = (*object)["content"] != NULL &&
		    (*object)["content"]->is_string();
		call.content =
		    call.content_present ? node_text((*object)["content"]) : "";
		return true;
	}
	const std::string encoded = node_text(arguments);
	if (encoded.empty())
		return false;
	acl::json json(encoded.c_str());
	if (!json.finish() || !json.get_root().is_object())
		return false;
	call.path = node_text(json["path"]);
	call.query = node_text(json["query"]);
	call.old_text = node_text(json["old_text"]);
	call.target_path = node_text(json["target_path"]);
	call.content_present =
	    json["content"] != NULL && json["content"]->is_string();
	call.content = call.content_present ? node_text(json["content"]) : "";
	return true;
}

bool empty_tool_arguments(acl::json_node *arguments)
{
	if (arguments == NULL || arguments->is_null())
		return true;
	acl::json_node *array = array_value(arguments);
	if (array != NULL)
		return array->first_child() == NULL;
	std::string encoded = node_text(arguments);
	encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
	                  [](char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	}),
	    encoded.end());
	return encoded.empty() || encoded == "[]" || encoded == "null";
}

void mirror_first_tool_call(completion_result_t &result)
{
	result.native_tool_call = !result.tool_calls.empty();
	if (!result.native_tool_call)
		return;
	const completion_tool_call_t &call = result.tool_calls.front();
	result.tool_name = call.name;
	result.tool_path = call.path;
	result.tool_query = call.query;
	result.tool_old_text = call.old_text;
	result.tool_target_path = call.target_path;
	result.tool_content = call.content;
}

void set_tool_call(const std::string &name, acl::json_node *arguments,
    completion_result_t &result, const std::string &id)
{
	if (name.empty() || result.tool_calls.size() >= 32)
		return;
	completion_tool_call_t call;
	call.id = id;
	call.name = canonical_tool_name(name);
	// A few compatible providers encode a parameterless function call as
	// null, an empty string, or []. Accept that representation only for the
	// genuinely parameterless validator; required-path tools still fail closed.
	if ((!parse_tool_arguments(arguments, call)) &&
	    (call.name != "workspace.validate" ||
	        !empty_tool_arguments(arguments))) {
		result.error_category = provider_error_protocol;
		return;
	}
	result.tool_calls.push_back(call);
	mirror_first_tool_call(result);
}

void append_text(std::string &target, const std::string &value)
{
	if (value.empty())
		return;
	if (!target.empty())
		target += "\n";
	target += value;
}

std::string message_content_text(acl::json_node *content)
{
	const std::string scalar = node_text(content);
	if (!scalar.empty())
		return scalar;
	acl::json_node *parts = array_value(content);
	std::string result;
	for (acl::json_node *part = parts ? parts->first_child() : NULL;
	     part != NULL; part = parts->next_child()) {
		std::string value = node_text(part);
		if (value.empty())
			value = node_text(object_child(part, "text"));
		if (value.empty())
			value = node_text(object_child(part, "refusal"));
		result += value;
	}
	return result;
}

// Return the exact plaintext carried by a Responses reasoning output item.
// DeepSeek requires this text to be replayed on the following tool request;
// summaries are useful for display but are not a protocol substitute.
std::string responses_reasoning_text(acl::json_node *item)
{
	acl::json_node *content = array_value(object_child(item, "content"));
	std::string result;
	for (acl::json_node *part = content ? content->first_child() : NULL;
	     part != NULL; part = content->next_child()) {
		if (!(node_text(object_child(part, "type")) ==
		        "reasoning_text"))
			continue;
		result += node_text(object_child(part, "text"));
	}
	return result;
}

std::string responses_terminal_error(acl::json_node *response,
    std::string &status, std::string &incomplete_reason)
{
	status = node_text(object_child(response, "status"));
	incomplete_reason = node_text(object_child(
	    object_child(response, "incomplete_details"), "reason"));
	acl::json_node *error = object_child(response, "error");
	std::string message = node_text(object_child(error, "message"));
	std::string code = node_text(object_child(error, "code"));
	if (status != "failed" && status != "cancelled" &&
	    status != "incomplete") {
		return "";
	}
	std::string out = "AI provider response " + status;
	if (!incomplete_reason.empty())
		out += ": " + incomplete_reason;
	if (!message.empty())
		out += ": " + message;
	if (code.empty())
		return out;
	out += " (code=" + code + ")";
	return out;
}

static bool parse_response_output(
    acl::json_node *item, completion_result_t &result)
{

	const std::string type = node_text(object_child(item, "type"));
	if (type == "function_call") {
		set_tool_call(node_text(object_child(item, "name")),
		    object_child(item, "arguments"), result,
		    node_text(object_child(item, "call_id")));
	} else if (type == "reasoning") {
		const std::string reasoning_text =
		    responses_reasoning_text(item);
		if (!reasoning_text.empty()) {
			result.reasoning += reasoning_text;
		} else {
			// OpenAI may expose only a summary unless encrypted reasoning was
			// requested. Preserve the former display fallback for that case.
			acl::json_node *summary =
			    array_value(object_child(item, "summary"));
			for (acl::json_node *part =
			         summary ? summary->first_child() : NULL;
			     part != NULL; part = summary->next_child()) {
				append_text(result.reasoning,
				    node_text(object_child(part, "text")));
			}
		}
	} else if (result.text.empty() && type == "message") {
		acl::json_node *content =
		    array_value(object_child(item, "content"));
		for (acl::json_node *part = content ? content->first_child() :
		                                      NULL;
		     part != NULL; part = content->next_child()) {
			std::string visible =
			    node_text(object_child(part, "text"));
			if (visible.empty()) {
				visible =
				    node_text(object_child(part, "refusal"));
			}
			append_text(result.text, visible);
		}
	}

	return true;
}

static bool parse_responses_completion(
    acl::json &json, completion_result_t &result, std::string &err)
{
	result.response_id = node_text(json["id"]);
	result.response_status = node_text(json["status"]);
	result.incomplete_reason =
	    node_text(object_child(json["incomplete_details"], "reason"));
	std::string terminal_error;
	if (result.response_status == "failed" ||
	    result.response_status == "cancelled" ||
	    result.response_status == "incomplete") {
		terminal_error =
		    "AI provider response " + result.response_status;
		if (!result.incomplete_reason.empty()) {
			terminal_error += ": " + result.incomplete_reason;
		}
		const std::string message =
		    node_text(object_child(json["error"], "message"));
		const std::string code =
		    node_text(object_child(json["error"], "code"));
		if (!message.empty())
			terminal_error += ": " + message;
		if (!code.empty())
			terminal_error += " (code=" + code + ")";
	}
	if (!terminal_error.empty()) {
		err = terminal_error;
		return false;
	}
	result.text = node_text(json["output_text"]);
	acl::json_node *output = array_value(json["output"]);
	for (acl::json_node *item = output ? output->first_child() : NULL;
	     item != NULL; item = output->next_child()) {
		if (!parse_response_output(item, result))
			break;
	}
	acl::json_node *usage = json["usage"];
	result.input_tokens = node_number(object_child(usage, "input_tokens"));
	result.output_tokens =
	    node_number(object_child(usage, "output_tokens"));
	result.cached_input_tokens = node_number(object_child(
	    object_child(usage, "input_tokens_details"), "cached_tokens"));
	result.cache_usage_available = object_child(object_child(usage, "input_tokens_details"), "cached_tokens") != NULL;
	result.reasoning_tokens = node_number(object_child(
	    object_child(usage, "output_tokens_details"), "reasoning_tokens"));
	return true;
}

bool parse_completion_json(const provider_config_t &provider, acl::json &json,
    completion_result_t &result, std::string &err)
{
	if (provider.protocol == "openai_responses") {
		if (!parse_responses_completion(json, result, err))
			return false;
	} else if (provider.protocol == "anthropic_messages") {
		acl::json_node *content = array_value(json["content"]);
		for (acl::json_node *item = content ? content->first_child() :
		                                      NULL;
		     item != NULL; item = content->next_child()) {
			const std::string type =
			    node_text(object_child(item, "type"));
			if (type == "tool_use") {
				set_tool_call(
				    node_text(object_child(item, "name")),
				    object_child(item, "input"), result,
				    node_text(object_child(item, "id")));
			} else if (type == "thinking") {
				append_text(result.reasoning,
				    node_text(object_child(item, "thinking")));
			} else if (type == "text") {
				append_text(result.text,
				    node_text(object_child(item, "text")));
			}
		}
		acl::json_node *usage = json["usage"];
		parse_anthropic_usage(usage, result);
	} else if (provider.protocol == "gemini_native") {
		acl::json_node *candidate =
		    first_array_item(json["candidates"]);
		acl::json_node *content = object_child(candidate, "content");
		acl::json_node *parts =
		    array_value(object_child(content, "parts"));
		for (acl::json_node *part = parts ? parts->first_child() : NULL;
		     part != NULL; part = parts->next_child()) {
			acl::json_node *call =
			    object_child(part, "functionCall");
			if (call != NULL) {
				set_tool_call(
				    node_text(object_child(call, "name")),
				    object_child(call, "args"), result,
				    node_text(object_child(call, "id")));
			}
			if (node_bool(object_child(part, "thought"))) {
				append_text(result.reasoning,
				    node_text(object_child(part, "text")));
			} else {
				append_text(result.text,
				    node_text(object_child(part, "text")));
			}
		}
		acl::json_node *usage = json["usageMetadata"];
		result.input_tokens =
		    node_number(object_child(usage, "promptTokenCount"));
		result.output_tokens =
		    node_number(object_child(usage, "candidatesTokenCount"));
		result.cached_input_tokens =
		    node_number(object_child(usage, "cachedContentTokenCount"));
		result.cache_usage_available = object_child(usage, "cachedContentTokenCount") != NULL;
	} else {
		acl::json_node *choice = first_array_item(json["choices"]);
		const std::string finish_reason =
		    node_text(object_child(choice, "finish_reason"));
		if (finish_reason == "length" ||
		    finish_reason == "max_tokens") {
			result.incomplete_reason = "max_output_tokens";
		}
		acl::json_node *message = provider.protocol == "ollama" ?
		    (json["message"] ? json["message"]->get_obj() : NULL) :
		    object_child(choice, "message");
		result.text =
		    message_content_text(object_child(message, "content"));
		if (result.text.empty()) {
			result.text =
			    node_text(object_child(message, "refusal"));
		}
		result.reasoning =
		    node_text(object_child(message, "reasoning_content"));
		if (result.reasoning.empty()) {
			result.reasoning =
			    node_text(object_child(message, "reasoning"));
		}
		if (result.reasoning.empty()) {
			result.reasoning =
			    node_text(object_child(message, "thinking"));
		}
		acl::json_node *calls =
		    array_value(object_child(message, "tool_calls"));
		for (acl::json_node *call = calls ? calls->first_child() : NULL;
		     call != NULL; call = calls->next_child()) {
			acl::json_node *function =
			    object_child(call, "function");
			if (!(function != NULL))
				continue;
			set_tool_call(node_text(object_child(function, "name")),
			    object_child(function, "arguments"), result,
			    node_text(object_child(call, "id")));
		}
		if (provider.protocol == "ollama") {
			result.input_tokens =
			    node_number(json["prompt_eval_count"]);
			result.output_tokens = node_number(json["eval_count"]);
		} else {
			acl::json_node *usage = json["usage"];
			result.input_tokens =
			    node_number(object_child(usage, "prompt_tokens"));
			result.output_tokens = node_number(
			    object_child(usage, "completion_tokens"));
			parse_chat_cache_usage(usage, result);
			result.reasoning_tokens = node_number(object_child(
			    object_child(usage, "completion_tokens_details"),
			    "reasoning_tokens"));
		}
	}
	if (result.error_category == provider_error_protocol) {
		result.tool_calls.clear();
		result.native_tool_call = false;
		err =
		    "AI provider returned invalid tool arguments (expected JSON object)";
		return false;
	}
	// A background Responses create/retrieve call legitimately has no output
	// while it is queued or running.  Its opaque ID and status are the useful
	// result; rejecting it here would prevent the caller from entering the
	// bounded polling loop or resuming that response after a service restart.
	const bool pending_response = provider.protocol == "openai_responses" &&
	    (result.response_status == "queued" ||
	        result.response_status == "in_progress") &&
	    !result.response_id.empty();
	if (result.text.empty() && !result.native_tool_call &&
	    !pending_response) {
		err =
		    "AI provider response contains no text output or tool call";
		return false;
	}
	if (result.text.size() > kMaxCompletionBytes) {
		err = "AI provider response exceeds 4 MiB";
		return false;
	}
	if (!(result.reasoning.size() > kMaxCompletionBytes))
		return true;
	err = "AI provider reasoning exceeds 4 MiB";
	return false;
}

// Recorded responses and streaming fallbacks still enter as strings. Keep this
// narrow adapter so the normal HTTP path can parse directly into acl::json.
bool parse_completion(const provider_config_t &provider,
    const std::string &body, completion_result_t &result, std::string &err)
{
	acl::json json(body.c_str());
	if (json.finish())
		return parse_completion_json(provider, json, result, err);
	err = "AI provider returned invalid JSON";
	return false;
}

// Normalize cache-aware input totals to include both hits and misses.
void parse_chat_cache_usage(acl::json_node *usage, completion_result_t &result)
{
	auto *hit = object_child(usage, "prompt_cache_hit_tokens");
	auto *miss = object_child(usage, "prompt_cache_miss_tokens");
	auto *cached = object_child(object_child(usage, "prompt_tokens_details"), "cached_tokens");
	if (hit || miss || cached) {
		result.cache_usage_available = true;
		result.cached_input_tokens = hit ? node_number(hit) : cached ? node_number(cached) :
		    std::max(0LL, result.input_tokens - node_number(miss));
		if (hit && miss)
			result.input_tokens = node_number(hit) + node_number(miss);
	}
}
void parse_anthropic_usage(acl::json_node *usage, completion_result_t &result)
{
	auto *input = object_child(usage, "input_tokens");
	auto *read = object_child(usage, "cache_read_input_tokens");
	auto *write = object_child(usage, "cache_creation_input_tokens");
	// message_delta can carry output only. Preserve the input from message_start.
	if (input) result.input_tokens = node_number(input) + node_number(read) + node_number(write);
	if (read || write) {
		result.cache_usage_available = true;
		if (read) result.cached_input_tokens = node_number(read);
	}
	if (auto *output = object_child(usage, "output_tokens"))
		result.output_tokens = node_number(output);
}

void copy_usage(const completion_result_t &source, completion_result_t &target)
{
	target.cache_usage_available = target.cache_usage_available || source.cache_usage_available;
	if (source.input_tokens > 0)
		target.input_tokens = source.input_tokens;
	if (source.cache_usage_available || source.cached_input_tokens > 0) {
		target.cached_input_tokens = source.cached_input_tokens;
	}
	if (source.output_tokens > 0)
		target.output_tokens = source.output_tokens;
	if (source.reasoning_tokens > 0) {
		target.reasoning_tokens = source.reasoning_tokens;
	}
}

void adopt_completed_stream_result(const completion_result_t &completed,
    completion_result_t &result, std::string &text_delta,
    std::string &reasoning_delta)
{
	if (result.response_id.empty())
		result.response_id = completed.response_id;
	if (result.response_status.empty())
		result.response_status = completed.response_status;
	if (result.incomplete_reason.empty()) {
		result.incomplete_reason = completed.incomplete_reason;
	}
	// Some compatible gateways buffer the answer and expose it only in their
	// terminal event. Do not append it after real deltas, or the answer would be
	// duplicated for providers that send both incremental and completed forms.
	if (result.text.empty() && text_delta.empty() &&
	    !completed.text.empty()) {
		text_delta = completed.text;
	}
	if (reasoning_delta.empty() && !completed.reasoning.empty()) {
		if (result.reasoning.empty()) {
			reasoning_delta = completed.reasoning;
		} else if (completed.reasoning.compare(0,
		               result.reasoning.size(),
		               result.reasoning) == 0) {
			// The terminal Responses event is authoritative. Append only a tail
			// that was absent from the incremental stream, avoiding duplication.
			reasoning_delta =
			    completed.reasoning.substr(result.reasoning.size());
		}
	}
	if (!result.native_tool_call && completed.native_tool_call) {
		result.tool_calls = completed.tool_calls;
		mirror_first_tool_call(result);
	}
	copy_usage(completed, result);
}

} // namespace provider_detail

using namespace provider_detail;

bool provider_client_t::parse_completion_response(
    const provider_config_t &provider, const std::string &body,
    completion_result_t &result, std::string &err)
{
	result.text.clear();
	result.response_id.clear();
	result.response_status.clear();
	result.incomplete_reason.clear();
	result.reasoning.clear();
	result.input_tokens = 0;
	result.cached_input_tokens = 0;
	result.cache_usage_available = false;
	result.output_tokens = 0;
	result.reasoning_tokens = 0;
	result.latency_ms = 0;
	result.http_status = 0;
	result.error_category = provider_error_none;
	result.retryable_error = false;
	result.native_tool_call = false;
	result.tool_calls.clear();
	result.tool_name.clear();
	result.tool_path.clear();
	result.tool_query.clear();
	result.tool_old_text.clear();
	result.tool_target_path.clear();
	result.tool_content.clear();
	err.clear();
	if (parse_completion(provider, body, result, err))
		return true;
	result.error_category =
	    result.incomplete_reason == "max_output_tokens" ?
	    provider_error_response_limit :
	    (result.response_status == "cancelled" ? provider_error_cancelled :
	                                             provider_error_protocol);
	return ai_error("provider.client", "parse-recorded-completion", err);
}

} // namespace ai
} // namespace webcool
