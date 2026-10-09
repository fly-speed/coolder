#pragma once
#include "../common/json_value.h"
#include "../common/utf8_text.h"
namespace webcool
{
namespace ai
{
// Model implementation claims are deliberately separate from runtime acceptance.
inline std::string normalize_requirement_progress(acl::json_node *value)
{
	auto *items = json_value::array_value(value);
	acl::json json;
	auto &result = json.create_array();
	size_t count = 0;
	for (auto *item = items ? items->first_child() : NULL;
	     item && count < 12; item = items->next_child()) {
		auto text = [&](const char *key) {
			return json_value::string_text(
			    json_value::object_child(item, key));
		};
		const std::string requirement =
		    utf8_prefix(text("requirement"), 240);
		if (requirement.empty())
			continue;
		std::string status = text("status"),
		            evidence = utf8_prefix(text("evidence"), 360);
		if (status != "implemented" && status != "partial" &&
		    status != "not_implemented")
			status = "unknown";
		// A bare completion assertion has no explanatory value.
		if ((status == "implemented" || status == "partial") &&
		    evidence.empty())
			status = "unknown";
		auto &entry = result.add_child(false, true);
		entry.add_text("requirement", requirement.c_str());
		entry.add_text("status", status.c_str());
		entry.add_text("evidence", evidence.c_str());
		entry.add_text(
		    "remaining", utf8_prefix(text("remaining"), 360).c_str());
		entry.add_text("source", "model_report");
		entry.add_bool("independently_verified", false);
		++count;
	}
	return count ? std::string(result.to_string().c_str()) : "";
}
// Format recorded progress against the task's acceptance requirements.
inline std::string requirement_progress_summary(
    const std::string &value, bool chinese)
{
	acl::json json(value.c_str());
	if (value.empty() || !json.finish())
		return chinese ?
		    "- 需求完成程度：无法确认。未取得逐项实施报告，文件修订数不能代表功能完成。" :
		    "- Requirement progress: unknown. No per-requirement implementation report; file counts do not establish completion.";
	auto *items = json_value::array_value(&json.get_root());
	std::string out = chinese ?
	    "- 需求完成程度（AI 实施报告，未经独立验收）：" :
	    "- Requirement progress (AI report, not independently verified):";
	for (auto *item = items ? items->first_child() : NULL; item;
	     item = items->next_child()) {
		auto text = [&](const char *key) {
			return json_value::string_text(
			    json_value::object_child(item, key));
		};
		const std::string status = text("status");
		const std::string label = status == "implemented" ?
		    (chinese ? "已实现，待验收" :
		               "implemented, acceptance pending") :
		    status == "partial" ?
		    (chinese ? "部分完成" : "partially implemented") :
		    status == "not_implemented" ?
		    (chinese ? "未完成" : "not implemented") :
		    (chinese ? "无法确认" : "unknown");
		std::string line = "\n- " +
		    utf8_prefix(text("requirement"), 180) + "：" + label;
		if (!text("evidence").empty())
			line += "；" + utf8_prefix(text("evidence"), 180);
		if (!text("remaining").empty())
			line += (chinese ? "；待办：" : "; remaining: ") +
			    utf8_prefix(text("remaining"), 180);
		if (out.size() + line.size() > 1750) {
			out += chinese ?
			    "\n- 其余事项见逐项实施报告。" :
			    "\n- More items in the implementation report.";
			break;
		}
		out += line;
	}
	return out;
}
}
}
