#include "stdafx.h"
#include "../common/json_value.h"
#include "agent_protocol.h"
#include "../context/requirement_progress.h"

#include <cctype>

namespace webcool {
namespace ai {
namespace {

// The fallback protocol accepts one small JSON object, optionally wrapped in a
// Markdown code fence. Parsing failure is not logged as an error because many
// providers legitimately return plain text; the caller then treats it as the
// final answer instead of a tool instruction.

std::string node_text(acl::json_node* node) {
    return ::webcool::ai::json_value::scalar_text(node);
}

acl::json_node* object_child(acl::json_node* node, const char* name) {
    return ::webcool::ai::json_value::object_child(node, name);
}

acl::json_node* array_value(acl::json_node* node) {
    return ::webcool::ai::json_value::array_value(node);
}

std::string trim_ascii(const std::string& value) {
	size_t begin = 0;
	while (begin < value.size()
		&& std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
	size_t end = value.size();
	while (end > begin
		&& std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
	return value.substr(begin, end - begin);
}

std::string unwrap_json_fence(const std::string& value) {
	std::string text = trim_ascii(value);
	if (text.compare(0, 3, "```") != 0) return text;
	const size_t first_line = text.find('\n');
	const size_t closing = text.rfind("```");
	if (first_line == std::string::npos || closing == std::string::npos
		|| closing <= first_line) return text;
	return trim_ascii(text.substr(first_line + 1, closing - first_line - 1));
}

void truncate_utf8_bytes(std::string& value, size_t maximum_bytes) {
	if (value.size() <= maximum_bytes) return;
	size_t keep = maximum_bytes;
	while (keep > 0
		&& (static_cast<unsigned char>(value[keep]) & 0xc0) == 0x80) --keep;
	value.resize(keep);
}

} // namespace

bool parse_agent_protocol_message(const std::string& raw,
	agent_protocol_message_t& message)
{
	message = agent_protocol_message_t();
	const std::string candidate = unwrap_json_fence(raw);
	acl::json json(candidate.c_str());
	if (!json.finish()) return false;
	const std::string type = node_text(json["type"]);
	if (type == "final") {
		message.final_message = true;
		message.final_text = node_text(json["text"]);
		message.memory_summary = node_text(json["memory_summary"]);
		message.completion_summary = node_text(json["completion_summary"]);
		message.requirement_progress_json = normalize_requirement_progress(json["requirement_progress"]);
		message.session_title = node_text(json["session_title"]);
		if (message.session_title.size() > 120) {
			message.session_title.resize(120);
		}
		if (message.memory_summary.size() > 8 * 1024) {
			message.memory_summary.resize(8 * 1024);
		}
		truncate_utf8_bytes(message.completion_summary, 4 * 1024);
		acl::json_node* items = array_value(json["changes"]);
		if (items != NULL) {
			for (acl::json_node* item = items->first_child(); item != NULL
				&& message.changes.size() < 20; item = items->next_child())
			{
				agent_change_proposal_t change;
				change.operation = node_text(object_child(item, "operation"));
				if (change.operation.empty()) change.operation = "write";
				change.path = node_text(object_child(item, "path"));
				change.target_path = node_text(object_child(item, "target_path"));
				change.content = node_text(object_child(item, "content"));
				change.reason = node_text(object_child(item, "reason"));
				message.changes.push_back(change);
			}
		}
		return !message.final_text.empty();
	}
	// Some providers label an otherwise explicit patch_set call with its
	// operation name. Accept only this unambiguous pair; tool/path/content
	// validation and the normal staged-review transaction still apply.
	if (type != "tool_call"
		&& !(type == "patch_set"
			&& node_text(json["name"]) == "workspace.patch_set")) return false;
	message.tool.name = node_text(json["name"]);
	acl::json_node* arguments = json["arguments"];
	message.tool.path = node_text(object_child(arguments, "path"));
	message.tool.query = node_text(object_child(arguments, "query"));
	message.tool.old_text = node_text(object_child(arguments, "old_text"));
	message.tool.target_path = node_text(object_child(arguments, "target_path"));
	acl::json_node* content = object_child(arguments, "content");
	message.tool.content_present = content != NULL && content->is_string();
	message.tool.content = message.tool.content_present ? node_text(content) : "";
	return !message.tool.name.empty();
}

} // namespace ai
} // namespace webcool
