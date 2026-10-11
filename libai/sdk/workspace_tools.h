#pragma once
#include "agent.h"
#include "../workspace/agent_workspace.h"
#include "../common/json_value.h"

namespace libai
{
namespace workspace_detail
{

// Serialize one workspace result within its owning JSON document.
inline tool_result_t serialized(acl::json_node &node)
{
	const auto &text = node.to_string();
	return tool_result_t::success(std::string(text.c_str(), text.size()));
}
// Build a string-argument schema for an existing workspace capability.
inline tool_spec_t spec(const std::string &name, const std::string &description,
    const std::vector<std::string> &fields, bool approval = false)
{
	tool_spec_t result;
	result.name = name;
	result.description = description;
	result.requires_approval = approval;
	acl::json json;
	auto &root = json.create_node();
	root.add_text("type", "object");
	auto &properties = json.create_node();
	root.add_child("properties", properties);
	auto &required = json.create_array();
	root.add_child("required", required);
	for (const auto &field : fields) {
		auto &value = json.create_node();
		properties.add_child(field.c_str(), value);
		value.add_text("type", "string");
		required.add_array_text(field.c_str());
	}
	root.add_bool("additionalProperties", false);
	const auto &text = root.to_string();
	result.parameters.assign(text.c_str(), text.size());
	return result;
}
// Validate required strings before invoking the confined workspace operation.
inline void bind(agent_t &agent, const tool_spec_t &spec,
    std::function<tool_result_t(acl::json &)> execute,
    const std::vector<std::string> &fields)
{
	agent.add_tool(spec,
	    [execute, fields](
	        const tool_call_t &call, const tool_context_t &context) {
		if (context.stopped())
			return tool_result_t::failure(
			    "workspace operation stopped");
		acl::json json(call.arguments.c_str());
		if (!json.finish() || !json.get_root().is_object())
			return tool_result_t::failure(
			    "arguments must be a JSON object");
		for (const auto &field : fields) {
			auto *value = json[field.c_str()];
			if (!value || !value->is_string())
				return tool_result_t::failure(
				    "required string argument: " + field);
		}
		return execute(json);
	});
}
} // namespace workspace_detail

// The root is chosen by the host. All paths are confined by the existing
// workspace implementation, with external account mounts disabled.
// Writes are opt-in and require the application's per-call authorization.
inline agent_t &add_workspace_tools(
    agent_t &agent, const std::string &root, bool writes = false)
{
	using namespace workspace_detail;
	using webcool::ai::json_value::string_text;
	auto workspace =
	    std::make_shared<webcool::ai::agent_workspace_t>(root, false);
	bind(agent,
	    spec("workspace.read",
	        "Read a relative text file and its SHA-256 baseline",
	        { "path" }),
	    [workspace](acl::json &arguments) {
		std::string content, error;
		bool truncated = false;
		if (!workspace->read(string_text(arguments["path"]), content,
		        truncated, error, false))
			return tool_result_t::failure(error);
		acl::json json;
		auto &result = json.create_node();
		result.add_text("content", content.c_str());
		result.add_bool("truncated", truncated);
		if (!truncated)
			result.add_text("sha256",
			    webcool::ai::agent_workspace_t::content_sha256(
			        content)
			        .c_str());
		return serialized(result);
	}, { "path" });
	bind(agent,
	    spec("workspace.list",
	        "List a relative directory; empty path means root", { "path" }),
	    [workspace](acl::json &arguments) {
		std::string error;
		std::vector<webcool::ai::workspace_entry_t> entries;
		if (!workspace->list(
		        string_text(arguments["path"]), entries, error, false))
			return tool_result_t::failure(error);
		acl::json json;
		auto &result = json.create_array();
		for (const auto &entry : entries) {
			auto &item = result.add_child(false, true);
			item.add_text("path", entry.path.c_str());
			item.add_bool("directory", entry.directory);
			item.add_number("size", entry.size);
		}
		return serialized(result);
	}, { "path" });
	bind(agent,
	    spec("workspace.search", "Search text beneath a relative directory",
	        { "path", "query" }),
	    [workspace](acl::json &arguments) {
		std::string error;
		bool truncated = false;
		std::vector<webcool::ai::workspace_match_t> matches;
		if (!workspace->search(string_text(arguments["path"]),
		        string_text(arguments["query"]), matches, truncated,
		        error))
			return tool_result_t::failure(error);
		acl::json json;
		auto &result = json.create_node();
		result.add_bool("truncated", truncated);
		auto &items = json.create_array();
		result.add_child("matches", items);
		for (const auto &match : matches) {
			auto &item = items.add_child(false, true);
			item.add_text("path", match.path.c_str());
			item.add_number(
			    "line", static_cast<long long>(match.line));
			item.add_text("text", match.text.c_str());
		}
		return serialized(result);
	}, { "path", "query" });
	if (writes) {
		bind(agent,
		    spec("workspace.create",
		        "Create a new file without overwriting an existing file",
		        { "path", "content" }, true),
		    [workspace](acl::json &arguments) {
			std::string error;
			if (!workspace->create_text_if_absent(
			        string_text(arguments["path"]),
			        string_text(arguments["content"]), error))
				return tool_result_t::failure(error);
			return tool_result_t::success("file created");
		}, { "path", "content" });
		bind(agent,
		    spec("workspace.replace",
		        "Replace a file only if its SHA-256 baseline still matches",
		        { "path", "expected_sha256", "content" }, true),
		    [workspace](acl::json &arguments) {
			std::string error;
			if (!workspace->replace_text_if_unchanged(
			        string_text(arguments["path"]),
			        string_text(arguments["expected_sha256"]),
			        string_text(arguments["content"]), error))
				return tool_result_t::failure(error);
			return tool_result_t::success("file replaced");
		}, { "path", "expected_sha256", "content" });
	}
	return agent;
}
} // namespace libai
