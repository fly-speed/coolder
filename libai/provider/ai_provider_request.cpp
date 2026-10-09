#include "stdafx.h"
#include "ai_provider_client_internal.h"
#include "../common/utf8_text.h"
#include "../prompt/prompt_language.h"

#include <algorithm>

namespace webcool
{
namespace ai
{
namespace provider_detail
{

std::string wire_tool_name(const std::string &canonical)
{
	std::string name = canonical;
	for (size_t i = 0; i < name.size(); ++i) {
		if (name[i] == '.')
			name[i] = '_';
	}
	return name;
}

std::string canonical_tool_name(const std::string &wire)
{
	if (wire == "browser_patch_style")
		return "browser.patch_style";
	if (wire == "workspace_list")
		return "workspace.list";
	if (wire == "workspace_read")
		return "workspace.read";
	if (wire == "workspace_read_batch")
		return "workspace.read_batch";
	if (wire == "workspace_search")
		return "workspace.search";
	if (wire == "workspace_create")
		return "workspace.create";
	if (wire == "workspace_mkdir")
		return "workspace.mkdir";
	if (wire == "workspace_propose")
		return "workspace.propose";
	if (wire == "workspace_propose_batch")
		return "workspace.propose_batch";
	if (wire == "workspace_replace")
		return "workspace.replace";
	if (wire == "workspace_patch_set")
		return "workspace.patch_set";
	if (wire == "workspace_propose_delete")
		return "workspace.propose_delete";
	if (wire == "workspace_propose_move")
		return "workspace.propose_move";
	if (wire == "workspace_propose_mkdir")
		return "workspace.propose_mkdir";
	// Tool names use dot-separated namespaces and registry names must avoid
	// underscores. This reversible fallback lets future C++ tools work without a
	// provider-specific name table while preserving legacy workspace mappings.
	std::string canonical = wire;
	for (size_t i = 0; i < canonical.size(); ++i) {
		if (canonical[i] == '_')
			canonical[i] = '.';
	}
	return canonical;
}

void add_tool_parameters(acl::json &json, acl::json_node &parent,
			 const char *field, const agent_tool_t &tool,
			 bool uppercase, bool strict)
{
	acl::json_node &schema = json.create_node();
	parent.add_child(field, schema);
	schema.add_text("type", uppercase ? "OBJECT" : "object");
	acl::json_node &properties = json.create_node();
	schema.add_child("properties", properties);
	acl::json_node &required = json.create_array();
	schema.add_child("required", required);
	if (strict && !uppercase)
		schema.add_bool("additionalProperties", false);
	for (size_t i = 0; i < tool.parameters.size(); ++i) {
		const agent_tool_parameter_t &parameter = tool.parameters[i];
		acl::json_node &value = json.create_node();
		properties.add_child(parameter.name.c_str(), value);
		value.add_text("type", uppercase ? "STRING" : "string");
		value.add_text("description", parameter.description.c_str());
		if (parameter.required)
			required.add_array_text(parameter.name.c_str());
	}
}

void add_completion_tools(acl::json &json, acl::json_node &root,
			  const provider_config_t &provider,
			  const completion_request_t &input)
{
	if (input.tools.empty())
		return;
	// Official OpenAI endpoints require an explicit opt-in before the model may
	// return independent tool calls in one assistant turn. Compatible gateways
	// are intentionally left untouched because several reject unknown fields.
	if (provider.protocol == "openai_chat" ||
	    provider.protocol == "openai_responses") {
		root.add_bool("parallel_tool_calls", true);
	}
	// A proposal-only repair turn is different from an ordinary model turn: all
	// required source context is already present and another prose promise would
	// only consume tokens until the no-progress guard fires. Ask native protocols
	// to return an actual tool call. Kimi K2.7 Code accepts the OpenAI-compatible
	// tool schema but rejects tool_choice=required (observed as an immediate 4xx
	// for both streaming and ordinary JSON requests). Keep its tools enabled and
	// rely on the proposal-only protocol instruction instead of falling all the
	// way back to a slow tool-free response. Ollama is excluded for the same
	// compatibility reason across its supported versions.
	if (input.require_tool_call) {
		if (provider.protocol == "anthropic_messages") {
			acl::json_node &choice = json.create_node();
			root.add_child("tool_choice", choice);
			choice.add_text("type", "any");
		} else if (provider.protocol == "gemini_native") {
			acl::json_node &config = json.create_node();
			root.add_child("tool_config", config);
			acl::json_node &calling = json.create_node();
			config.add_child("function_calling_config", calling);
			calling.add_text("mode", "ANY");
		} else if (provider.protocol != "ollama" &&
			   !kimi_model_is(provider, "kimi-k2.7") &&
			   !kimi_model_is(provider, "kimi-k3")) {
			root.add_text("tool_choice", "required");
		}
	}
	acl::json_node &tools = json.create_array();
	root.add_child("tools", tools);
	acl::json_node *declarations = NULL;
	if (provider.protocol == "gemini_native") {
		acl::json_node &wrapper = tools.add_child(false, true);
		acl::json_node &list = json.create_array();
		wrapper.add_child("functionDeclarations", list);
		declarations = &list;
	}
	for (size_t i = 0; i < input.tools.size(); ++i) {
		acl::json_node &item = (declarations ? *declarations : tools)
					       .add_child(false, true);
		agent_tool_t tool = input.tools[i];
		tool.description = localized_tool_description(
			tool.description, input.ui_language);
		for (auto &parameter : tool.parameters) {
			parameter.description = localized_tool_description(
				parameter.description, input.ui_language);
		}
		const std::string wire = wire_tool_name(tool.name);
		if (provider.protocol == "openai_responses") {
			item.add_text("type", "function");
			item.add_text("name", wire.c_str());
			item.add_text("description", tool.description.c_str());
			bool all_required = true;
			for (size_t j = 0; j < tool.parameters.size(); ++j) {
				if (!tool.parameters[j].required)
					all_required = false;
			}
			const bool strict = input.strict_tools && all_required;
			// Qwen documents the flat Responses function schema but not OpenAI's
			// strict extension. Omit unknown fields instead of relying on them being
			// ignored by every DashScope gateway revision.
			if (!qwen_responses_endpoint(provider))
				item.add_bool("strict", strict);
			add_tool_parameters(json, item, "parameters", tool,
					    false, strict);
		} else if (provider.protocol == "anthropic_messages") {
			item.add_text("name", wire.c_str());
			item.add_text("description", tool.description.c_str());
			add_tool_parameters(json, item, "input_schema", tool,
					    false, false);
		} else if (provider.protocol == "gemini_native") {
			item.add_text("name", wire.c_str());
			item.add_text("description", tool.description.c_str());
			add_tool_parameters(json, item, "parameters", tool,
					    true, false);
		} else {
			item.add_text("type", "function");
			acl::json_node &function = json.create_node();
			item.add_child("function", function);
			function.add_text("name", wire.c_str());
			function.add_text("description",
					  tool.description.c_str());
			add_tool_parameters(json, function, "parameters", tool,
					    false, false);
		}
	}
}

void add_normalized_tool_arguments(acl::json_node &arguments,
				   const completion_tool_call_t &call)
{
	if (!call.path.empty())
		arguments.add_text("path", call.path.c_str());
	if (!call.query.empty())
		arguments.add_text("query", call.query.c_str());
	if (!call.old_text.empty()) {
		arguments.add_text("old_text", call.old_text.c_str());
	}
	if (!call.target_path.empty()) {
		arguments.add_text("target_path", call.target_path.c_str());
	}
	if (call.content_present || !call.content.empty()) {
		arguments.add_text("content", call.content.c_str());
	}
}

std::string normalized_tool_arguments_json(const completion_tool_call_t &call)
{
	acl::json json;
	acl::json_node &arguments = json.create_node();
	add_normalized_tool_arguments(arguments, call);
	const acl::string &serialized = arguments.to_string();
	return std::string(serialized.c_str(), serialized.size());
}

const completion_tool_output_t *
exchange_output(const completion_tool_exchange_t &exchange, size_t index)
{
	if (index < exchange.outputs.size())
		return &exchange.outputs[index];
	return NULL;
}

static void add_stateless_responses_input(
	const provider_config_t &provider, const completion_request_t &input,
	const std::vector<std::string> &encoded_images, acl::json &json,
	acl::json_node &root, bool deepseek_responses,
	bool reasoning_summary_items)
{
	// Stateless providers validate every function_call_output against a function_call
	// present in the same stateless input. Replay the typed items in order;
	// sending only an OpenAI previous_response_id produces HTTP 400. Always
	// use the sequence form even without tool history: some DeepSeek service
	// revisions reject a scalar input while tools are present, despite the
	// compatibility guide also documenting scalar strings.
	acl::json_node &items = json.create_array();
	root.add_child("input", items);
	acl::json_node &user = items.add_child(false, true);
	user.add_text("role", "user");
	if (input.images.empty()) {
		user.add_text("content", input.user_prompt.c_str());
	} else {
		acl::json_node &content = json.create_array();
		user.add_child("content", content);
		acl::json_node &text = content.add_child(false, true);
		text.add_text("type", "input_text");
		text.add_text("text", input.user_prompt.c_str());
		for (size_t i = 0; i < input.images.size(); ++i) {
			acl::json_node &image = content.add_child(false, true);
			image.add_text("type", "input_image");
			const std::string uri =
				"data:" + input.images[i].mime_type +
				";base64," + encoded_images[i];
			image.add_text("image_url", uri.c_str());
		}
	}
	for (size_t turn = 0; turn < input.tool_history.size(); ++turn) {
		const completion_tool_exchange_t &exchange =
			input.tool_history[turn];
		if (!exchange.preceding_instructions.empty()) {
			acl::json_node &guidance = items.add_child(false, true);
			guidance.add_text("role", "developer");
			guidance.add_text(
				"content",
				exchange.preceding_instructions.c_str());
		}
		// DeepSeek's Responses-compatible endpoint decides whether the
		// conversation is a thinking conversation from the already replayed
		// exchange, not only from this request's current effort switch.  A
		// resumed/repair turn deliberately sets effort=none, but DeepSeek still
		// requires a typed reasoning_text item before the historical function
		// call. This also applies to the final-answer turn after WebCool removes
		// the current tool definitions. In either case the faithful reasoning
		// value may be empty, but the typed item must remain present.
		const bool reasoning_replay_required =
			deepseek_responses && !exchange.calls.empty();
		if (reasoning_replay_required ||
		    (!exchange.reasoning_content.empty() &&
		     (deepseek_responses ||
		      kimi_responses_endpoint(provider)))) {
			acl::json_node &reasoning =
				items.add_child(false, true);
			reasoning.add_text("type", "reasoning");
			// Official DeepSeek accepts reasoning.content/reasoning_text. The
			// DashScope Responses gateway instead requires reasoning.summary to
			// be a list of summary_text parts, including on no-thinking recovery
			// turns where the faithfully replayed text is empty.
			acl::json_node &reasoning_content = json.create_array();
			reasoning.add_child(
				reasoning_summary_items ? "summary" : "content",
				reasoning_content);
			acl::json_node &reasoning_text =
				reasoning_content.add_child(false, true);
			reasoning_text.add_text("type",
						reasoning_summary_items ?
							"summary_text" :
							"reasoning_text");
			reasoning_text.add_text(
				"text", exchange.reasoning_content.c_str());
		}
		if (!exchange.assistant_text.empty()) {
			acl::json_node &assistant =
				items.add_child(false, true);
			assistant.add_text("role", "assistant");
			assistant.add_text("content",
					   exchange.assistant_text.c_str());
		}
		for (size_t i = 0; i < exchange.calls.size(); ++i) {
			acl::json_node &call = items.add_child(false, true);
			call.add_text("type", "function_call");
			const completion_tool_output_t *matching =
				exchange_output(exchange, i);
			const std::string call_id =
				!exchange.calls[i].id.empty() ?
					exchange.calls[i].id :
					(matching ?
						 matching->call_id :
						 "webcool_call_" +
							 std::to_string(i + 1));
			call.add_text("call_id", call_id.c_str());
			call.add_text(
				"name",
				wire_tool_name(exchange.calls[i].name).c_str());
			const std::string arguments =
				normalized_tool_arguments_json(
					exchange.calls[i]);
			call.add_text("arguments", arguments.c_str());
		}
		for (size_t i = 0; i < exchange.outputs.size(); ++i) {
			acl::json_node &output = items.add_child(false, true);
			output.add_text("type", "function_call_output");
			output.add_text("call_id",
					exchange.outputs[i].call_id.c_str());
			output.add_text("output",
					exchange.outputs[i].output.c_str());
		}
	}
	if (!input.turn_instructions.empty()) {
		acl::json_node &guidance = items.add_child(false, true);
		guidance.add_text("role", "developer");
		guidance.add_text("content", input.turn_instructions.c_str());
	}
}

static void
add_responses_payload(const provider_config_t &provider,
		      const completion_request_t &input, bool stream,
		      std::string &effective_effort,
		      const std::vector<std::string> &encoded_images,
		      acl::json &json, acl::json_node &root)
{
	const bool deepseek_responses = deepseek_responses_endpoint(provider);
	const bool qwen_responses = qwen_responses_endpoint(provider);
	const std::string responses_base = lowercase_ascii(provider.base_url);
	const bool dashscope_reasoning_items =
		responses_base.find("dashscope.aliyuncs.com") !=
			std::string::npos ||
		responses_base.find("dashscope-intl.aliyuncs.com") !=
			std::string::npos ||
		responses_base.find(".maas.aliyuncs.com") != std::string::npos;
	const bool stateless_responses =
		provider_client_t::responses_are_stateless(provider);
	root.add_text("model", provider.model.c_str());
	root.add_text("instructions", input.system_prompt.c_str());
	if (!stateless_responses && !input.previous_response_id.empty()) {
		root.add_text("previous_response_id",
			      input.previous_response_id.c_str());
	}
	if (stateless_responses) {
		add_stateless_responses_input(provider, input, encoded_images,
					      json, root, deepseek_responses,
					      dashscope_reasoning_items);
	} else if (!stateless_responses && !input.tool_outputs.empty()) {
		acl::json_node &outputs = json.create_array();
		root.add_child("input", outputs);
		for (size_t i = 0; i < input.tool_outputs.size(); ++i) {
			acl::json_node &item = outputs.add_child(false, true);
			item.add_text("type", "function_call_output");
			item.add_text("call_id",
				      input.tool_outputs[i].call_id.c_str());
			item.add_text("output",
				      input.tool_outputs[i].output.c_str());
		}
	} else {
		// Although the official Responses API accepts a scalar string, several
		// compatible implementations deserialize `input` strictly as a sequence.
		// Always send a one-message array so first turns, restored checkpoints and
		// provider aliases share the same interoperable wire shape.
		acl::json_node &messages = json.create_array();
		root.add_child("input", messages);
		acl::json_node &user = messages.add_child(false, true);
		user.add_text("role", "user");
		if (input.images.empty()) {
			user.add_text("content", input.user_prompt.c_str());
		} else {
			acl::json_node &content = json.create_array();
			user.add_child("content", content);
			acl::json_node &text = content.add_child(false, true);
			text.add_text("type", "input_text");
			text.add_text("text", input.user_prompt.c_str());
			for (size_t i = 0; i < input.images.size(); ++i) {
				acl::json_node &image =
					content.add_child(false, true);
				image.add_text("type", "input_image");
				const std::string uri =
					"data:" + input.images[i].mime_type +
					";base64," + encoded_images[i];
				image.add_text("image_url", uri.c_str());
			}
		}
	}
	root.add_number("max_output_tokens", input.max_output_tokens);
	root.add_bool("stream", stream);
	if (stream && !stateless_responses && !qwen_responses) {
		acl::json_node &stream_options = json.create_node();
		root.add_child("stream_options", stream_options);
		stream_options.add_bool("include_obfuscation", false);
	}
	root.add_bool("store",
		      stateless_responses ?
			      false :
			      (provider.response_state_mode == "stateful" ||
			       input.store));
	if (!stateless_responses && input.background && !qwen_responses) {
		root.add_bool("background", true);
	}
	if (!stateless_responses && input.compact_context && !qwen_responses) {
		// Native server-side compaction keeps the previous_response_id chain
		// usable while replacing old context with an opaque compaction item.
		// The threshold is intentionally below current reasoning-model context
		// limits so tool-heavy coding runs compact before a hard 400 failure.
		acl::json_node &management = json.create_array();
		root.add_child("context_management", management);
		acl::json_node &compaction = management.add_child(false, true);
		compaction.add_text("type", "compaction");
		compaction.add_number("compact_threshold", 120000);
	}
	if (!deepseek_responses && !qwen_responses &&
	    !input.prompt_cache_key.empty()) {
		root.add_text("prompt_cache_key",
			      input.prompt_cache_key.substr(0, 64).c_str());
	}
	if (!input.prompt_cache_ttl.empty() &&
	    input.prompt_cache_ttl != "none" &&
	    model_name_contains(provider, "gpt-5.6")) {
		acl::json_node &options = json.create_node();
		root.add_child("prompt_cache_options", options);
		options.add_text("mode", "implicit");
		options.add_text("ttl", input.prompt_cache_ttl.c_str());
	}
	if (!deepseek_responses && !qwen_responses &&
	    !input.safety_identifier.empty()) {
		root.add_text("safety_identifier",
			      input.safety_identifier.substr(0, 64).c_str());
	}
	if (!deepseek_responses && !qwen_responses &&
	    (!input.metadata_run_id.empty() ||
	     !input.metadata_session_id.empty())) {
		acl::json_node &metadata = json.create_node();
		root.add_child("metadata", metadata);
		if (!input.metadata_run_id.empty()) {
			metadata.add_text(
				"webcool_run_id",
				input.metadata_run_id.substr(0, 512).c_str());
		}
		if (!input.metadata_session_id.empty()) {
			metadata.add_text(
				"webcool_session_id",
				input.metadata_session_id.substr(0, 512)
					.c_str());
		}
	}
	// Kimi's Codex-compatible endpoint rejects the UI's default "auto".
	// Leave automatic tier selection to the server by omitting the field.
	if (!deepseek_responses && !qwen_responses &&
	    !input.service_tier.empty() &&
	    !(kimi_responses_endpoint(provider) &&
	      input.service_tier == "auto")) {
		root.add_text("service_tier", input.service_tier.c_str());
	}
	if (!qwen_responses && !input.text_verbosity.empty()) {
		acl::json_node &text = json.create_node();
		root.add_child("text", text);
		text.add_text("verbosity", input.text_verbosity.c_str());
	}
	effective_effort =
		responses_reasoning_effort(provider, effective_effort);
	if (!effective_effort.empty() ||
	    (!qwen_responses && !input.reasoning_summary.empty() &&
	     input.reasoning_summary != "none")) {
		acl::json_node &reasoning = json.create_node();
		root.add_child("reasoning", reasoning);
		if (!effective_effort.empty()) {
			reasoning.add_text("effort", effective_effort.c_str());
		}
		if (!qwen_responses && !input.reasoning_summary.empty() &&
		    input.reasoning_summary != "none") {
			reasoning.add_text("summary",
					   input.reasoning_summary.c_str());
		}
	}
}

static void
add_anthropic_payload(const provider_config_t &provider,
		      const completion_request_t &input, bool stream,
		      std::string &effective_effort,
		      const std::vector<std::string> &encoded_images,
		      acl::json &json, acl::json_node &root)
{
	root.add_text("model", provider.model.c_str());
	// Anthropic's ephemeral cache breakpoint keeps the stable coding policy
	// reusable across iterative tool turns. This field is emitted only for the
	// native Messages protocol; compatible gateways retain scalar system text.
	acl::json_node &system = json.create_array();
	root.add_child("system", system);
	acl::json_node &system_text = system.add_child(false, true);
	system_text.add_text("type", "text");
	system_text.add_text("text", input.system_prompt.c_str());
	acl::json_node &cache_control = json.create_node();
	system_text.add_child("cache_control", cache_control);
	cache_control.add_text("type", "ephemeral");
	root.add_number("max_tokens", input.max_output_tokens);
	root.add_bool("stream", stream);
	if (!effective_effort.empty()) {
		acl::json_node &reasoning = json.create_node();
		root.add_child("reasoning", reasoning);
		reasoning.add_text("effort", effective_effort.c_str());
	}
	acl::json_node &messages = json.create_array();
	root.add_child("messages", messages);
	acl::json_node &user = messages.add_child(false, true);
	user.add_text("role", "user");
	if (input.images.empty()) {
		user.add_text("content", input.user_prompt.c_str());
	} else {
		acl::json_node &content = json.create_array();
		user.add_child("content", content);
		acl::json_node &text = content.add_child(false, true);
		text.add_text("type", "text");
		text.add_text("text", input.user_prompt.c_str());
		for (size_t i = 0; i < input.images.size(); ++i) {
			acl::json_node &image = content.add_child(false, true);
			image.add_text("type", "image");
			acl::json_node &source = json.create_node();
			image.add_child("source", source);
			source.add_text("type", "base64");
			source.add_text("media_type",
					input.images[i].mime_type.c_str());
			source.add_text("data", encoded_images[i].c_str());
		}
	}
	for (size_t turn = 0; turn < input.tool_history.size(); ++turn) {
		const completion_tool_exchange_t &exchange =
			input.tool_history[turn];
		acl::json_node &assistant = messages.add_child(false, true);
		assistant.add_text("role", "assistant");
		acl::json_node &assistant_content = json.create_array();
		assistant.add_child("content", assistant_content);
		for (size_t i = 0; i < exchange.calls.size(); ++i) {
			acl::json_node &use =
				assistant_content.add_child(false, true);
			use.add_text("type", "tool_use");
			const completion_tool_output_t *output =
				exchange_output(exchange, i);
			const std::string id =
				!exchange.calls[i].id.empty() ?
					exchange.calls[i].id :
					(output ? output->call_id :
						  "webcool_call");
			use.add_text("id", id.c_str());
			use.add_text(
				"name",
				wire_tool_name(exchange.calls[i].name).c_str());
			acl::json_node &arguments = json.create_node();
			use.add_child("input", arguments);
			add_normalized_tool_arguments(arguments,
						      exchange.calls[i]);
		}
		acl::json_node &tool_user = messages.add_child(false, true);
		tool_user.add_text("role", "user");
		acl::json_node &tool_content = json.create_array();
		tool_user.add_child("content", tool_content);
		for (size_t i = 0; i < exchange.outputs.size(); ++i) {
			acl::json_node &result =
				tool_content.add_child(false, true);
			result.add_text("type", "tool_result");
			result.add_text("tool_use_id",
					exchange.outputs[i].call_id.c_str());
			result.add_text("content",
					exchange.outputs[i].output.c_str());
		}
	}
}

static void add_gemini_payload(const completion_request_t &input,
			       const std::vector<std::string> &encoded_images,
			       acl::json &json, acl::json_node &root)
{
	acl::json_node &instruction = json.create_node();
	root.add_child("system_instruction", instruction);
	acl::json_node &instruction_parts = json.create_array();
	instruction.add_child("parts", instruction_parts);
	acl::json_node &instruction_text =
		instruction_parts.add_child(false, true);
	instruction_text.add_text("text", input.system_prompt.c_str());
	acl::json_node &contents = json.create_array();
	root.add_child("contents", contents);
	acl::json_node &content = contents.add_child(false, true);
	content.add_text("role", "user");
	acl::json_node &parts = json.create_array();
	content.add_child("parts", parts);
	acl::json_node &part = parts.add_child(false, true);
	part.add_text("text", input.user_prompt.c_str());
	for (size_t i = 0; i < input.images.size(); ++i) {
		acl::json_node &image = parts.add_child(false, true);
		acl::json_node &inline_data = json.create_node();
		image.add_child("inline_data", inline_data);
		inline_data.add_text("mime_type",
				     input.images[i].mime_type.c_str());
		inline_data.add_text("data", encoded_images[i].c_str());
	}
	for (size_t turn = 0; turn < input.tool_history.size(); ++turn) {
		const completion_tool_exchange_t &exchange =
			input.tool_history[turn];
		acl::json_node &model = contents.add_child(false, true);
		model.add_text("role", "model");
		acl::json_node &model_parts = json.create_array();
		model.add_child("parts", model_parts);
		for (size_t i = 0; i < exchange.calls.size(); ++i) {
			acl::json_node &part_call =
				model_parts.add_child(false, true);
			acl::json_node &function_call = json.create_node();
			part_call.add_child("functionCall", function_call);
			function_call.add_text(
				"name",
				wire_tool_name(exchange.calls[i].name).c_str());
			acl::json_node &arguments = json.create_node();
			function_call.add_child("args", arguments);
			add_normalized_tool_arguments(arguments,
						      exchange.calls[i]);
		}
		acl::json_node &tool = contents.add_child(false, true);
		tool.add_text("role", "user");
		acl::json_node &tool_parts = json.create_array();
		tool.add_child("parts", tool_parts);
		for (size_t i = 0; i < exchange.outputs.size(); ++i) {
			acl::json_node &part_result =
				tool_parts.add_child(false, true);
			acl::json_node &function_response = json.create_node();
			part_result.add_child("functionResponse",
					      function_response);
			const std::string name =
				i < exchange.calls.size() ?
					wire_tool_name(exchange.calls[i].name) :
					"workspace_tool";
			function_response.add_text("name", name.c_str());
			acl::json_node &response = json.create_node();
			function_response.add_child("response", response);
			response.add_text("output",
					  exchange.outputs[i].output.c_str());
		}
	}
	acl::json_node &generation = json.create_node();
	root.add_child("generationConfig", generation);
	generation.add_number("maxOutputTokens", input.max_output_tokens);
}

static void add_chat_payload(const provider_config_t &provider,
			     const completion_request_t &input, bool stream,
			     std::string &effective_thinking,
			     std::string &effective_effort,
			     const std::vector<std::string> &encoded_images,
			     acl::json &json, acl::json_node &root)
{
	root.add_text("model", provider.model.c_str());
	root.add_bool("stream", stream);
	if (stream && provider.protocol != "ollama") {
		acl::json_node &options = json.create_node();
		root.add_child("stream_options", options);
		options.add_bool("include_usage", true);
	}
	if (provider.protocol == "ollama") {
		acl::json_node &options = json.create_node();
		root.add_child("options", options);
		options.add_number("num_predict", input.max_output_tokens);
	} else {
		// Kimi has deprecated max_tokens. Using the current field is especially
		// important for Code Plan gateways, which account rate limits against the
		// requested completion budget.
		if (kimi_model_is(provider, "kimi-")) {
			root.add_number("max_completion_tokens",
					input.max_output_tokens);
			// Kimi's documented prompt_cache_key should remain stable for every
			// tool turn and after reconnecting the same coding session.  Do not
			// send it to generic OpenAI-compatible gateways that may reject it.
			if (!input.prompt_cache_key.empty()) {
				root.add_text(
					"prompt_cache_key",
					input.prompt_cache_key.substr(0, 128)
						.c_str());
			}
		} else {
			root.add_number("max_tokens", input.max_output_tokens);
		}
		if (!effective_effort.empty() &&
		    !(effective_thinking == "disabled" &&
		      effective_effort == "none")) {
			root.add_text("reasoning_effort",
				      effective_effort.c_str());
		}
		if (!effective_thinking.empty()) {
			acl::json_node &thinking = json.create_node();
			root.add_child("thinking", thinking);
			thinking.add_text("type", effective_thinking.c_str());
			if (kimi_model_is(provider, "kimi-k2.6") &&
			    effective_thinking == "enabled") {
				thinking.add_text("keep", "all");
			}
		}
	}
	acl::json_node &messages = json.create_array();
	root.add_child("messages", messages);
	acl::json_node &system = messages.add_child(false, true);
	system.add_text("role", "system");
	system.add_text("content", input.system_prompt.c_str());
	acl::json_node &user = messages.add_child(false, true);
	user.add_text("role", "user");
	if (provider.protocol == "ollama") {
		user.add_text("content", input.user_prompt.c_str());
		if (!input.images.empty()) {
			acl::json_node &images = json.create_array();
			user.add_child("images", images);
			for (size_t i = 0; i < encoded_images.size(); ++i) {
				images.add_array_text(
					encoded_images[i].c_str());
			}
		}
	} else if (input.images.empty()) {
		user.add_text("content", input.user_prompt.c_str());
	} else {
		acl::json_node &content = json.create_array();
		user.add_child("content", content);
		acl::json_node &text = content.add_child(false, true);
		text.add_text("type", "text");
		text.add_text("text", input.user_prompt.c_str());
		for (size_t i = 0; i < input.images.size(); ++i) {
			acl::json_node &image = content.add_child(false, true);
			image.add_text("type", "image_url");
			acl::json_node &image_url = json.create_node();
			image.add_child("image_url", image_url);
			const std::string uri =
				"data:" + input.images[i].mime_type +
				";base64," + encoded_images[i];
			image_url.add_text("url", uri.c_str());
		}
	}
	for (size_t turn = 0; turn < input.tool_history.size(); ++turn) {
		const completion_tool_exchange_t &exchange =
			input.tool_history[turn];
		acl::json_node &assistant = messages.add_child(false, true);
		assistant.add_text("role", "assistant");
		// DeepSeek requires every assistant message to carry reasoning_content
		// when a thinking-mode request also carries tools.  This includes turns
		// produced by a temporary no-thinking recovery, where the faithful value
		// is the empty string.  Omitting that empty field causes DeepSeek to reject
		// the next tool turn with HTTP 400. Requests without both thinking and tools
		// retain the previous non-empty-only behavior, avoiding an unnecessary
		// extension on ordinary compatible endpoints. Assistant content must likewise
		// remain a non-null string when the model emitted only a tool call.
		assistant.add_text("content", exchange.assistant_text.c_str());
		const bool reasoning_replay_required =
			effective_thinking == "enabled" && !input.tools.empty();
		if (reasoning_replay_required ||
		    !exchange.reasoning_content.empty()) {
			assistant.add_text("reasoning_content",
					   exchange.reasoning_content.c_str());
		}
		acl::json_node &calls = json.create_array();
		assistant.add_child("tool_calls", calls);
		for (size_t i = 0; i < exchange.calls.size(); ++i) {
			acl::json_node &call = calls.add_child(false, true);
			call.add_text("id", (exchange.calls[i].id.empty() ?
						     ("webcool_call_" +
						      std::to_string(i + 1)) :
						     exchange.calls[i].id)
						    .c_str());
			call.add_text("type", "function");
			acl::json_node &function = json.create_node();
			call.add_child("function", function);
			function.add_text(
				"name",
				wire_tool_name(exchange.calls[i].name).c_str());
			const std::string arguments =
				normalized_tool_arguments_json(
					exchange.calls[i]);
			function.add_text("arguments", arguments.c_str());
		}
		for (size_t i = 0; i < exchange.outputs.size(); ++i) {
			acl::json_node &tool = messages.add_child(false, true);
			tool.add_text("role", "tool");
			tool.add_text("tool_call_id",
				      exchange.outputs[i].call_id.c_str());
			if (i < exchange.calls.size()) {
				tool.add_text(
					"name",
					wire_tool_name(exchange.calls[i].name)
						.c_str());
			}
			tool.add_text("content",
				      exchange.outputs[i].output.c_str());
		}
	}
}

static void normalize_completion_reasoning(const provider_config_t &provider,
					   const completion_request_t &input,
					   std::string &effective_thinking,
					   std::string &effective_effort)
{
	// A forced no-reasoning retry must override an earlier enabled toggle. This
	// keeps recovery deterministic when a model exhausted its reasoning budget.
	effective_thinking = input.thinking_mode;
	effective_effort = input.reasoning_effort;
	if (input.reasoning_effort == "none") {
		if (kimi_model_is(provider, "kimi-k2.7-code")) {
			// K2.7 Code always thinks. Kimi rejects type=disabled and does not
			// support reasoning_effort, so a recovery request must omit both fields.
			effective_thinking.clear();
			effective_effort.clear();
		} else if (kimi_model_is(provider, "kimi-k3")) {
			// K3 also cannot disable reasoning; "low" is its least expensive valid
			// recovery setting. The K2-only thinking object is invalid for K3.
			effective_thinking.clear();
			effective_effort = "low";
		} else {
			// DeepSeek and Kimi K2.6 expose an actual thinking off switch. Keep the
			// existing generic compatibility behavior for other Chat providers.
			effective_thinking = "disabled";
		}
	}
	if (kimi_model_is(provider, "kimi-k3")) {
		effective_thinking.clear();
		if (effective_effort.empty())
			effective_effort = "max";
	}
	// K2.7 Code always preserves thinking and accepts neither an effort level
	// nor the K2.6 thinking toggle. Do not send either field, even on retries.
	if (kimi_model_is(provider, "kimi-k2.7-code")) {
		effective_thinking.clear();
		effective_effort.clear();
	}
	if (kimi_model_is(provider, "kimi-k2.6")) {
		effective_effort.clear();
		if (effective_thinking.empty())
			effective_thinking = "enabled";
	}
	if (effective_effort.empty() && effective_thinking == "enabled" &&
	    !kimi_model_is(provider, "kimi-k2.")) {
		effective_effort = "high";
	} else if (effective_thinking == "disabled") {
		effective_effort = "none";
	}
}

bool build_completion_payload(const provider_config_t &provider,
			      const completion_request_t &input, bool stream,
			      std::string &payload)
{
	std::string effective_thinking;
	std::string effective_effort;
	normalize_completion_reasoning(provider, input, effective_thinking,
				       effective_effort);
	std::vector<std::string> encoded_images;
	encoded_images.reserve(input.images.size());
	for (size_t i = 0; i < input.images.size(); ++i) {
		acl::string encoded;
		encoded.base64_encode(input.images[i].data.data(),
				      input.images[i].data.size());
		encoded_images.push_back(
			std::string(encoded.c_str(), encoded.size()));
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	if (provider.protocol == "openai_responses") {
		add_responses_payload(provider, input, stream, effective_effort,
				      encoded_images, json, root);
	} else if (provider.protocol == "anthropic_messages") {
		add_anthropic_payload(provider, input, stream, effective_effort,
				      encoded_images, json, root);
	} else if (provider.protocol == "gemini_native") {
		add_gemini_payload(input, encoded_images, json, root);
	} else {
		add_chat_payload(provider, input, stream, effective_thinking,
				 effective_effort, encoded_images, json, root);
	}
	add_completion_tools(json, root, provider, input);
	const acl::string &serialized = root.to_string();
	// Legacy checkpoints may contain a partial UTF-8 character from byte-based
	// truncation. Repair the outbound copy; never rewrite project source.
	payload =
		valid_utf8(std::string(serialized.c_str(), serialized.size()));
	return true;
}

} // namespace provider_detail

using namespace provider_detail;

bool provider_client_t::serialize_completion_request(
	const provider_config_t &provider, const completion_request_t &input,
	bool stream, std::string &payload)
{
	return build_completion_payload(provider, input, stream, payload);
}

bool provider_client_t::configure_response_state(provider_config_t &provider,
						 const std::string &mode,
						 std::string &err)
{
	if (mode != "auto" && mode != "stateless" && mode != "stateful") {
		err = "response_state_mode must be auto, stateless or stateful";
		return false;
	}
	if (mode == "stateful" && (provider.protocol != "openai_responses" ||
				   deepseek_responses_endpoint(provider) ||
				   kimi_responses_endpoint(provider))) {
		err = "stateful requests are not supported by this provider; select auto or stateless";
		return false;
	}
	provider.response_state_mode = mode;
	if (mode == "stateful")
		provider.responses_store = true;
	return true;
}

bool provider_client_t::responses_are_stateless(
	const provider_config_t &provider)
{
	return (provider.protocol == "openai_responses" &&
		provider.response_state_mode == "stateless") ||
	       deepseek_responses_endpoint(provider) ||
	       kimi_responses_endpoint(provider);
}

bool provider_client_t::supports_reasoning_effort(
	const provider_config_t &provider)
{
	return (provider.protocol == "openai_responses" ||
		provider.protocol == "openai_chat" ||
		provider.protocol == "openai_compatible") &&
	       (kimi_model_is(provider, "kimi-k3") ||
		kimi_model_is(provider, "deepseek-v4") ||
		qwen_responses_endpoint(provider));
}

} // namespace ai
} // namespace webcool
