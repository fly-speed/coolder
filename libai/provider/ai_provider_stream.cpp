#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include "../common/ai_error_log.h"

namespace webcool
{
namespace ai
{
namespace provider_detail
{

streamed_tool_call_t &streamed_tool_call(
    std::vector<streamed_tool_call_t> &calls, const std::string &key)
{
	for (size_t i = 0; i < calls.size(); ++i) {
		if (!(calls[i].key == key))
			continue;
		return calls[i];
	}
	streamed_tool_call_t call;
	call.key = key;
	calls.push_back(call);
	return calls.back();
}

std::string streamed_call_key(acl::json_node *call, size_t fallback)
{
	// Delta protocols keep `index` stable while often sending the call ID only
	// in the first fragment. Prefer index so later argument fragments join the
	// same accumulator; use IDs for event schemas that omit an index.
	acl::json_node *index_node = object_child(call, "index");
	if (index_node != NULL) {
		return "index:" + std::to_string(node_number(index_node));
	}
	std::string key = node_text(object_child(call, "call_id"));
	if (key.empty())
		key = node_text(object_child(call, "id"));
	if (key.empty())
		return "index:" +
		    std::to_string(static_cast<long long>(fallback));
	return "id:" + key;
}

size_t streamed_tool_argument_bytes(
    const std::vector<streamed_tool_call_t> &calls)
{
	size_t total = 0;
	for (size_t i = 0; i < calls.size(); ++i)
		total += calls[i].arguments.size();
	return total;
}

bool finalize_streamed_tool_calls(
    const std::vector<streamed_tool_call_t> &streamed_calls,
    completion_result_t &result, std::string &err)
{
	// A terminal full-response event is authoritative and already populated the
	// vector. Otherwise decode every independently accumulated call in wire order.
	if (!result.tool_calls.empty())
		return true;
	for (size_t i = 0; i < streamed_calls.size(); ++i) {
		const streamed_tool_call_t &streamed = streamed_calls[i];
		if (streamed.name.empty())
			continue;
		completion_tool_call_t call;
		call.id = streamed.id;
		call.name = streamed.name;
		if (!streamed.arguments.empty()) {
			acl::json arguments(streamed.arguments.c_str());
			if (!arguments.finish()) {
				// Never guess missing braces or execute a partially streamed write:
				// an apparently harmless repair could turn truncated source into a
				// real workspace edit.  Keep only structural diagnostics here; the
				// argument body can contain private source code and must not enter logs.
				err =
				    "AI provider returned invalid streamed tool arguments"
				    " (tool=" +
				    streamed.name +
				    ", call_index=" + std::to_string(i) +
				    ", argument_bytes=" +
				    std::to_string(streamed.arguments.size()) +
				    ")";
				return false;
			}
			if (!parse_tool_arguments(
			        &arguments.get_root(), call)) {
				result.tool_calls.clear();
				result.native_tool_call = false;
				err =
				    "AI provider returned invalid streamed tool arguments (expected JSON object)";
				return false;
			}
		}
		result.tool_calls.push_back(call);
	}
	mirror_first_tool_call(result);
	return true;
}

static bool parse_responses_stream_event(const provider_config_t &provider,
    completion_result_t &result,
    std::vector<streamed_tool_call_t> &streamed_calls, std::string &text_delta,
    std::string &reasoning_delta, stream_diagnostics_t &diagnostics,
    std::string &err, acl::json &json, const std::string &wire_event_type)
{
	const std::string &type = wire_event_type;
	if (type == "response.output_text.delta") {
		text_delta = node_text(json["delta"]);
	} else if (type == "response.reasoning_summary_text.delta" ||
	    type == "response.reasoning_text.delta") {
		reasoning_delta = node_text(json["delta"]);
	} else if (type == "response.reasoning_summary_text.done" ||
	    type == "response.reasoning_text.done") {
		reasoning_delta =
		    result.reasoning.empty() ? node_text(json["text"]) : "";
	} else if (type == "response.refusal.delta") {
		text_delta = node_text(json["delta"]);
	} else if (type == "response.output_text.done") {
		text_delta = result.text.empty() ? node_text(json["text"]) : "";
	} else if (type == "response.refusal.done") {
		text_delta =
		    result.text.empty() ? node_text(json["refusal"]) : "";
	} else if (type == "response.function_call_arguments.delta" ||
	    type == "response.function_call_arguments.done") {
		std::string key = node_text(json["call_id"]);
		if (json["output_index"] != NULL || key.empty())
			key = "index:" +
			    std::to_string(node_number(json["output_index"]));
		else
			key = "id:" + key;
		streamed_tool_call_t &call =
		    streamed_tool_call(streamed_calls, key);
		const std::string call_id = node_text(json["call_id"]);
		if (!call_id.empty())
			call.id = call_id;
		const std::string arguments =
		    type == "response.function_call_arguments.done" ?
		    node_text(json["arguments"]) :
		    node_text(json["delta"]);
		if (type == "response.function_call_arguments.done")
			call.arguments = arguments;
		else
			call.arguments += arguments;
		const std::string name = node_text(json["name"]);
		if (!name.empty())
			call.name = canonical_tool_name(name);
	} else if (type == "response.output_item.added" ||
	    type == "response.output_item.done") {
		acl::json_node *item = json["item"];
		const std::string item_type =
		    node_text(object_child(item, "type"));
		if (item_type == "function_call") {
			const std::string key = json["output_index"] != NULL ?
			    "index:" +
			        std::to_string(
			            node_number(json["output_index"])) :
			    streamed_call_key(item, 0);
			streamed_tool_call_t &call =
			    streamed_tool_call(streamed_calls, key);
			call.id = node_text(object_child(item, "call_id"));
			call.name = canonical_tool_name(
			    node_text(object_child(item, "name")));
			if (type == "response.output_item.done" ||
			    call.arguments.empty()) {
				call.arguments =
				    node_text(object_child(item, "arguments"));
			}
		} else if (item_type == "reasoning" &&
		    type == "response.output_item.done") {
			const std::string complete_reasoning =
			    responses_reasoning_text(item);
			if (result.reasoning.empty()) {
				reasoning_delta = complete_reasoning;
			} else if (complete_reasoning.compare(0,
			               result.reasoning.size(),
			               result.reasoning) == 0) {
				reasoning_delta = complete_reasoning.substr(
				    result.reasoning.size());
			}
		}
	} else if (type == "response.created" ||
	    type == "response.in_progress") {
		if (result.response_id.empty()) {
			result.response_id =
			    node_text(object_child(json["response"], "id"));
		}
	} else if (type == "response.incomplete" || type == "response.failed" ||
	    type == "response.cancelled") {
		acl::json_node *response = json["response"];
		std::string status;
		std::string reason;
		err = responses_terminal_error(response, status, reason);
		if (err.empty()) {
			status = type.substr(strlen("response."));
			err = "AI provider response " + status;
		}
		result.response_status = status;
		result.incomplete_reason = reason;
		if (!(reason == "max_output_tokens"))
			return false;
		diagnostics.finish_reason = "length";

		return false;
	} else if (type == "response.completed") {
		acl::json_node *response = json["response"];
		if (response != NULL) {
			const acl::string &serialized = response->to_string();
			completion_result_t completed;
			std::string ignored;
			provider_config_t responses_provider = provider;
			responses_provider.protocol = "openai_responses";
			if (parse_completion(responses_provider,
			        std::string(
			            serialized.c_str(), serialized.size()),
			        completed, ignored)) {
				adopt_completed_stream_result(completed, result,
				    text_delta, reasoning_delta);
			} else if (completed.error_category ==
			    provider_error_protocol) {
				err = ignored;
				result.tool_calls.clear();
				result.native_tool_call = false;
				return false;
			}
		}
	}
	return true;
}

static bool parse_anthropic_stream_event(completion_result_t &result,
    std::vector<streamed_tool_call_t> &streamed_calls, std::string &text_delta,
    std::string &reasoning_delta, acl::json &json)
{
	const std::string type = node_text(json["type"]);
	if (type == "content_block_start") {
		acl::json_node *block = json["content_block"];
		if (node_text(object_child(block, "type")) == "tool_use") {
			const std::string key = "index:" +
			    std::to_string(node_number(json["index"]));
			streamed_tool_call_t &call =
			    streamed_tool_call(streamed_calls, key);
			call.id = node_text(object_child(block, "id"));
			call.name = canonical_tool_name(
			    node_text(object_child(block, "name")));
		}
	} else if (type == "content_block_delta") {
		acl::json_node *delta = json["delta"];
		const std::string delta_type =
		    node_text(object_child(delta, "type"));
		if (delta_type == "text_delta") {
			text_delta = node_text(object_child(delta, "text"));
		} else if (delta_type == "thinking_delta") {
			reasoning_delta =
			    node_text(object_child(delta, "thinking"));
		} else if (delta_type == "input_json_delta") {
			const std::string key = "index:" +
			    std::to_string(node_number(json["index"]));
			streamed_tool_call(streamed_calls, key).arguments +=
			    node_text(object_child(delta, "partial_json"));
		}
	}
	acl::json_node *usage = json["usage"];
	if (usage == NULL)
		usage = object_child(json["message"], "usage");
	parse_anthropic_usage(usage, result);
	return true;
}

static bool parse_gemini_stream_event(const provider_config_t &provider,
    completion_result_t &result, std::string &text_delta,
    std::string &reasoning_delta, std::string &err, acl::json &json,
    const std::string &line)
{
	completion_result_t chunk;
	std::string ignored;
	if (!parse_completion(provider, line, chunk, ignored)) {
		// Gemini may send a final usage-only event with no candidate text.
		acl::json_node *usage = json["usageMetadata"];
		if (usage == NULL) {
			err =
			    "AI provider returned an invalid Gemini streaming event";
			return false;
		}
		chunk.input_tokens =
		    node_number(object_child(usage, "promptTokenCount"));
		chunk.cached_input_tokens =
		    node_number(object_child(usage, "cachedContentTokenCount"));
		chunk.cache_usage_available = object_child(usage, "cachedContentTokenCount") != NULL;
		chunk.output_tokens =
		    node_number(object_child(usage, "candidatesTokenCount"));
	}
	text_delta = chunk.text;
	reasoning_delta = chunk.reasoning;
	copy_usage(chunk, result);
	if (!chunk.native_tool_call)
		return true;
	result.tool_calls.insert(result.tool_calls.end(),
	    chunk.tool_calls.begin(), chunk.tool_calls.end());
	mirror_first_tool_call(result);

	return true;
}

static bool parse_ollama_stream_event(completion_result_t &result,
    std::string &text_delta, std::string &reasoning_delta, acl::json &json)
{
	acl::json_node *message = json["message"];
	text_delta = node_text(object_child(message, "content"));
	reasoning_delta = node_text(object_child(message, "thinking"));
	if (reasoning_delta.empty()) {
		reasoning_delta = node_text(object_child(message, "reasoning"));
	}
	acl::json_node *calls =
	    array_value(object_child(message, "tool_calls"));
	for (acl::json_node *call = calls ? calls->first_child() : NULL;
	     call != NULL; call = calls->next_child()) {
		acl::json_node *function = object_child(call, "function");
		if (!(function != NULL))
			continue;
		set_tool_call(node_text(object_child(function, "name")),
		    object_child(function, "arguments"), result,
		    node_text(object_child(call, "id")));
	}
	const long long input_tokens = node_number(json["prompt_eval_count"]);
	const long long output_tokens = node_number(json["eval_count"]);
	if (input_tokens > 0)
		result.input_tokens = input_tokens;
	if (!(output_tokens > 0))
		return true;
	result.output_tokens = output_tokens;
	return true;
}

static bool parse_chat_stream_event(completion_result_t &result,
    std::vector<streamed_tool_call_t> &streamed_calls, std::string &text_delta,
    std::string &reasoning_delta, stream_diagnostics_t &diagnostics,
    acl::json &json)
{
	acl::json_node *choice = first_array_item(json["choices"]);
	acl::json_node *delta = object_child(choice, "delta");
	reasoning_delta = node_text(object_child(delta, "reasoning_content"));
	if (reasoning_delta.empty()) {
		reasoning_delta = node_text(object_child(delta, "reasoning"));
	}
	if (reasoning_delta.empty()) {
		reasoning_delta = node_text(object_child(delta, "thinking"));
	}
	const std::string finish_reason =
	    node_text(object_child(choice, "finish_reason"));
	if (!finish_reason.empty() && finish_reason.size() <= 128) {
		diagnostics.finish_reason = finish_reason;
	}
	text_delta = message_content_text(object_child(delta, "content"));
	if (text_delta.empty()) {
		text_delta = node_text(object_child(delta, "refusal"));
	}
	acl::json_node *calls = array_value(object_child(delta, "tool_calls"));
	// A few OpenAI-compatible gateways wrap a complete message in an SSE
	// data event instead of emitting delta objects. Accept that safe variant.
	acl::json_node *message = object_child(choice, "message");
	if (text_delta.empty() && message != NULL) {
		if (reasoning_delta.empty()) {
			reasoning_delta = node_text(
			    object_child(message, "reasoning_content"));
		}
		if (reasoning_delta.empty()) {
			reasoning_delta =
			    node_text(object_child(message, "reasoning"));
		}
		if (reasoning_delta.empty()) {
			reasoning_delta =
			    node_text(object_child(message, "thinking"));
		}
		text_delta =
		    message_content_text(object_child(message, "content"));
		if (text_delta.empty()) {
			text_delta =
			    node_text(object_child(message, "refusal"));
		}
		if (calls == NULL)
			calls =
			    array_value(object_child(message, "tool_calls"));
	}
	size_t call_position = 0;
	for (acl::json_node *call_node = calls ? calls->first_child() : NULL;
	     call_node != NULL;
	     call_node = calls->next_child(), ++call_position) {
		acl::json_node *function = object_child(call_node, "function");
		if (function == NULL)
			continue;
		const std::string key =
		    streamed_call_key(call_node, call_position);
		streamed_tool_call_t &call =
		    streamed_tool_call(streamed_calls, key);
		const std::string call_id =
		    node_text(object_child(call_node, "id"));
		// Chat deltas usually include id only in their first fragment.
		// An absent id in later fragments must not erase the original identity.
		if (!call_id.empty())
			call.id = call_id;
		const std::string name =
		    node_text(object_child(function, "name"));
		if (!name.empty())
			call.name = canonical_tool_name(name);
		call.arguments +=
		    node_text(object_child(function, "arguments"));
	}
	acl::json_node *usage = json["usage"];
	if (!(usage != NULL))
		return true;
	result.input_tokens = node_number(object_child(usage, "prompt_tokens"));
	result.output_tokens =
	    node_number(object_child(usage, "completion_tokens"));
	parse_chat_cache_usage(usage, result);
	result.reasoning_tokens = node_number(
	    object_child(object_child(usage, "completion_tokens_details"),
	        "reasoning_tokens"));

	return true;
}

bool parse_stream_line(const provider_config_t &provider,
    const std::string &raw_line, completion_result_t &result,
    std::vector<streamed_tool_call_t> &streamed_calls, std::string &text_delta,
    std::string &reasoning_delta, stream_diagnostics_t &diagnostics,
    std::string &err)
{
	text_delta.clear();
	reasoning_delta.clear();
	std::string line = raw_line;
	if (line.compare(0, 5, "data:") == 0) {
		line.erase(0, 5);
		while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
			line.erase(0, 1);
	} else if (provider.protocol != "ollama") {
		// SSE also contains `event:`, retry and comment lines.
		return true;
	}
	if (line.empty() || line == "[DONE]")
		return true;
	acl::json json(line.c_str());
	if (!json.finish()) {
		err = "AI provider returned an invalid streaming event";
		return false;
	}
	++diagnostics.event_count;
	const std::string wire_event_type = node_text(json["type"]);
	if (!wire_event_type.empty() && wire_event_type.size() <= 128) {
		diagnostics.last_event_type = wire_event_type;
	}
	// Some gateways encode provider errors inside an HTTP 200 stream. Surface
	// only bounded, recognized fields; never return or log the raw event body.
	acl::json_node *stream_error = json["error"];
	const bool typed_error = wire_event_type == "error";
	const bool top_level_error = wire_event_type.empty() &&
	    json["code"] != NULL && json["message"] != NULL &&
	    json["choices"] == NULL;
	if ((stream_error != NULL && !stream_error->is_null()) || typed_error ||
	    top_level_error) {
		// The official Responses stream uses {type:"error", message, code},
		// while compatible gateways commonly nest the same fields below `error`.
		// Accept both shapes so an HTTP-200 provider failure is not misreported as
		// an empty/unsupported stream.
		acl::json_node *error_source =
		    stream_error != NULL && !stream_error->is_null() ?
		    stream_error :
		    &json.get_root();
		std::string message = bounded_diagnostic_text(
		    object_child(error_source, "message"), 512);
		if (message.empty()) {
			message = bounded_diagnostic_text(
			    object_child(error_source, "detail"), 512);
		}
		if (message.empty() && stream_error != NULL &&
		    stream_error->is_string()) {
			message = bounded_diagnostic_text(stream_error, 512);
		}
		std::string code = bounded_diagnostic_text(
		    object_child(error_source, "code"), 128);
		err = "AI provider streaming error";
		if (!message.empty())
			err += ": " + message;
		if (!code.empty())
			err += " (code=" + code + ")";
		classify_error(200, err, result);
		return false;
	}
	// A few DeepSeek-compatible gateways occasionally return Chat Completions
	// chunks from a Responses endpoint. Prefer the unambiguous wire shape over
	// the configured protocol so useful text/tool calls are not discarded.
	const bool chat_event = json["choices"] != NULL;
	const bool responses_event = !chat_event &&
	    (provider.protocol == "openai_responses" ||
	        wire_event_type.compare(0, 9, "response.") == 0);
	if (responses_event) {
		if (!parse_responses_stream_event(provider, result,
		        streamed_calls, text_delta, reasoning_delta,
		        diagnostics, err, json, wire_event_type))
			return false;
	} else if (provider.protocol == "anthropic_messages") {
		if (!parse_anthropic_stream_event(result, streamed_calls,
		        text_delta, reasoning_delta, json))
			return false;
	} else if (provider.protocol == "gemini_native") {
		if (!parse_gemini_stream_event(provider, result, text_delta,
		        reasoning_delta, err, json, line))
			return false;
	} else if (provider.protocol == "ollama") {
		if (!parse_ollama_stream_event(
		        result, text_delta, reasoning_delta, json))
			return false;
	} else {
		if (!parse_chat_stream_event(result, streamed_calls, text_delta,
		        reasoning_delta, diagnostics, json))
			return false;
	}
	if (reasoning_delta.empty())
		return true;
	diagnostics.reasoning_bytes += reasoning_delta.size();
	++diagnostics.reasoning_events;

	return true;
}

} // namespace provider_detail

using namespace provider_detail;

bool provider_client_t::parse_stream_response(const provider_config_t &provider,
    const std::string &body, completion_result_t &result, std::string &err,
    completion_stream_observer_t *observer)
{
	result = completion_result_t();
	err.clear();
	std::vector<streamed_tool_call_t> streamed_calls;
	stream_diagnostics_t diagnostics;
	size_t begin = 0;
	while (begin <= body.size()) {
		const size_t end = body.find('\n', begin);
		std::string line = body.substr(begin,
		    end == std::string::npos ? std::string::npos : end - begin);
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.resize(line.size() - 1);
		std::string delta;
		std::string reasoning_delta;
		if (!parse_stream_line(provider, line, result, streamed_calls,
		        delta, reasoning_delta, diagnostics, err)) {
			if (!(result.error_category !=
			        provider_error_rate_limit))
				return ai_error("provider.client",
				    "parse-recorded-stream", err);
			result.error_category =
			    result.incomplete_reason == "max_output_tokens" ?
			    provider_error_response_limit :
			    (result.response_status == "cancelled" ?
			            provider_error_cancelled :
			            provider_error_protocol);
			return ai_error(
			    "provider.client", "parse-recorded-stream", err);
		}
		if (!reasoning_delta.empty()) {
			if (result.reasoning.size() + reasoning_delta.size() >
			    kMaxCompletionBytes) {
				err = "AI provider reasoning exceeds 4 MiB";
				result.error_category =
				    provider_error_response_limit;
				return ai_error("provider.client",
				    "reasoning-stream-limit", err);
			}
			result.reasoning += reasoning_delta;
			if (observer != NULL &&
			    !observer->on_reasoning_delta(reasoning_delta)) {
				err =
				    "AI provider streaming response cancelled";
				result.error_category =
				    provider_error_cancelled;
				return false;
			}
		}
		if (!delta.empty()) {
			result.text += delta;
			if (observer != NULL &&
			    !observer->on_text_delta(delta)) {
				err =
				    "AI provider streaming response cancelled";
				result.error_category =
				    provider_error_cancelled;
				return false;
			}
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	if (streamed_tool_argument_bytes(streamed_calls) >
	    kMaxToolArgumentsBytes) {
		err = "AI provider tool arguments exceed 512 KiB";
		result.error_category = provider_error_response_limit;
		return ai_error(
		    "provider.client", "parse-stream-tool-limit", err);
	}
	if (!finalize_streamed_tool_calls(streamed_calls, result, err)) {
		// Chat-completion providers report output truncation separately from the
		// partially emitted function arguments.  Preserve that stronger signal:
		// this is an output-budget failure, not an unsupported tool schema.
		if (diagnostics.finish_reason == "length") {
			result.incomplete_reason = "max_output_tokens";
			result.error_category = provider_error_response_limit;
			err +=
			    ". The provider stopped at its output-token limit before the"
			    " tool call JSON was complete.";
		} else {
			result.error_category = provider_error_protocol;
		}
		return ai_error(
		    "provider.client", "parse-stream-tool-arguments", err);
	}
	if (!(result.text.empty() && !result.native_tool_call))
		return true;
	// Offline replay follows the live transport behavior: a provider may
	// ignore stream=true and return one ordinary completion JSON document.
	completion_result_t buffered;
	std::string ignored;
	if (parse_completion(provider, body, buffered, ignored)) {
		result = buffered;
		if (observer != NULL && !result.reasoning.empty() &&
		    !observer->on_reasoning_delta(result.reasoning)) {
			err = "AI provider streaming response cancelled";
			result.error_category = provider_error_cancelled;
			return false;
		}
		if (!(observer != NULL && !result.text.empty() &&
		        !observer->on_text_delta(result.text)))
			return true;
		err = "AI provider streaming response cancelled";
		result.error_category = provider_error_cancelled;
		return false;
	}
	err =
	    "AI provider streaming response contains no visible text or tool call"
	    " (protocol=" +
	    provider.protocol +
	    ", received_bytes=" + std::to_string(body.size()) +
	    ", events=" + std::to_string(diagnostics.event_count);
	if (diagnostics.reasoning_bytes > 0) {
		err += ", reasoning_bytes=" +
		    std::to_string(diagnostics.reasoning_bytes);
	}
	if (!diagnostics.finish_reason.empty()) {
		err += ", finish_reason=" + diagnostics.finish_reason;
	}
	err += "). ";
	if (diagnostics.reasoning_bytes > 0 &&
	    diagnostics.finish_reason == "length") {
		err +=
		    "The model used the entire output-token budget for reasoning"
		    " before producing its answer.";
	} else {
		err +=
		    "The provider returned an unsupported event schema or produced"
		    " no answer.";
	}
	classify_error(0, err, result);
	return ai_error("provider.client", "validate-recorded-stream", err);
}

} // namespace ai
} // namespace webcool
