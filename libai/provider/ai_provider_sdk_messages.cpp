#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include <map>

namespace webcool
{
namespace ai
{
namespace provider_detail
{

// Preserve typed arguments without imposing coding-tool field names.
static bool sdk_arguments(const libai::tool_call_t &call, acl::json &arguments)
{
	arguments.update(call.arguments.c_str());
	return arguments.finish() && arguments.get_root().is_object();
}

// Append one structured tool result to an Anthropic/Gemini user turn.
static void sdk_result_part(acl::json &json, acl::json_node &parts,
    const libai::message_t &message, const std::string &name, bool gemini)
{
	auto &part = parts.add_child(false, true);
	if (!gemini) {
		part.add_text("type", "tool_result");
		part.add_text("tool_use_id", message.call_id.c_str());
		part.add_text("content", message.text.c_str());
		part.add_bool("is_error", message.tool_error);
		return;
	}
	auto &function = json.create_node();
	part.add_child("functionResponse", function);
	function.add_text("id", message.call_id.c_str());
	function.add_text("name", name.c_str());
	auto &output = json.create_node();
	function.add_child("response", output);
	output.add_text("output", message.text.c_str());
}

// Copy a complete JSON object into the target document's lifetime.
static bool sdk_call_part(acl::json &json, acl::json_node &parts,
    const libai::tool_call_t &call, bool gemini)
{
	acl::json arguments;
	if (!sdk_arguments(call, arguments))
		return false;
	auto &part = parts.add_child(false, true);
	const auto name = wire_tool_name(call.name);
	if (!gemini) {
		part.add_text("type", "tool_use");
		part.add_text("id", call.id.c_str());
		part.add_text("name", name.c_str());
		part.add_child(
		    "input", json.duplicate_node(arguments.get_root()));
		return true;
	}
	auto &function = json.create_node();
	part.add_child("functionCall", function);
	function.add_text("id", call.id.c_str());
	function.add_text("name", name.c_str());
	function.add_child("args", json.duplicate_node(arguments.get_root()));
	return true;
}

// Text and native calls share one assistant/model content array.
static bool sdk_content_parts(acl::json &json, acl::json_node &parts,
    const libai::message_t &message, bool gemini,
    std::map<std::string, std::string> &names)
{
	if (!message.text.empty() || message.calls.empty()) {
		auto &text = parts.add_child(false, true);
		if (!gemini)
			text.add_text("type", "text");
		text.add_text("text", message.text.c_str());
	}
	for (const auto &call : message.calls) {
		if (!sdk_call_part(json, parts, call, gemini))
			return false;
		names[call.id] = wire_tool_name(call.name);
	}
	return true;
}

// OpenAI-style adapters retain arguments as a JSON string on the wire.
static bool sdk_chat_content(acl::json &json, acl::json_node &messages,
    acl::json_node &item, const libai::message_t &message, bool responses,
    std::map<std::string, std::string> &names)
{
	item.add_text("content", message.text.c_str());
	if (message.role == libai::message_t::role_t::tool)
		item.add_text("tool_call_id", message.call_id.c_str());
	if (!responses && !message.reasoning.empty())
		item.add_text("reasoning_content", message.reasoning.c_str());
	acl::json_node *calls = NULL;
	if (!message.calls.empty() && !responses) {
		calls = &json.create_array();
		item.add_child("tool_calls", *calls);
	}
	for (const auto &call : message.calls) {
		acl::json arguments;
		if (!sdk_arguments(call, arguments))
			return false;
		const auto name = wire_tool_name(call.name);
		names[call.id] = name;
		auto &entry =
		    (responses ? messages : *calls).add_child(false, true);
		entry.add_text(
		    "type", responses ? "function_call" : "function");
		entry.add_text(responses ? "call_id" : "id", call.id.c_str());
		acl::json_node *function = &entry;
		if (!responses) {
			function = &json.create_node();
			entry.add_child("function", *function);
		}
		function->add_text("name", name.c_str());
		function->add_text("arguments", call.arguments.c_str());
	}
	return true;
}

// SDK transcripts retain user/assistant/tool roles across independent runs.
// Legacy coding requests continue using their specialized serializers.
bool add_sdk_messages(const provider_config_t &provider,
    const completion_request_t &input, acl::json &json, acl::json_node &root)
{
	const bool responses = provider.protocol == "openai_responses";
	const bool anthropic = provider.protocol == "anthropic_messages";
	const bool gemini = provider.protocol == "gemini_native";
	const char *field = responses ? "input" :
	    gemini                    ? "contents" :
	                                "messages";
	json.remove(root[field]);
	if (responses && root["previous_response_id"])
		json.remove(root["previous_response_id"]);
	auto &messages = json.create_array();
	root.add_child(field, messages);
	if (!responses && !anthropic && !gemini &&
	    !input.system_prompt.empty()) {
		auto &system = messages.add_child(false, true);
		system.add_text("role", "system");
		system.add_text("content", input.system_prompt.c_str());
	}
	std::map<std::string, std::string> names;
	acl::json_node *grouped_tool = NULL, *grouped_parts = NULL;
	for (const auto &message : input.messages) {
		const bool assistant =
		    message.role == libai::message_t::role_t::assistant;
		const bool tool =
		    message.role == libai::message_t::role_t::tool;
		if (tool && !names.count(message.call_id))
			return false;
		const bool grouped =
		    tool && (anthropic || gemini) && grouped_tool;
		auto &item =
		    grouped ? *grouped_tool : messages.add_child(false, true);
		if (!tool) {
			grouped_tool = NULL;
			grouped_parts = NULL;
		}
		if (responses && tool) {
			item.add_text("type", "function_call_output");
			item.add_text("call_id", message.call_id.c_str());
			item.add_text("output", message.text.c_str());
			continue;
		}
		if (!grouped)
			item.add_text("role",
			    assistant ?
			        (gemini ? "model" : "assistant") :
			        (tool && !anthropic && !gemini ? "tool" :
			                                         "user"));
		if (!anthropic && !gemini) {
			if (!sdk_chat_content(json, messages, item, message,
			        responses, names))
				return false;
			continue;
		}
		auto &parts = grouped ? *grouped_parts : json.create_array();
		if (!grouped)
			item.add_child(gemini ? "parts" : "content", parts);
		if (tool) {
			grouped_tool = &item;
			grouped_parts = &parts;
			sdk_result_part(json, parts, message,
			    names[message.call_id], gemini);
		} else if (!sdk_content_parts(
		               json, parts, message, gemini, names)) {
			return false;
		}
	}
	return true;
}

} // namespace provider_detail
} // namespace ai
} // namespace webcool
