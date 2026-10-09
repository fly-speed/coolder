#pragma once
#include "../common/utf8_text.h"
#include "requirement_progress.h"
#include <string>
#include <vector>
#include <utility>
namespace webcool
{
namespace ai
{
// Requirements, recorded edits, model interpretation and verification have
// different authorities. Keep each visible without promoting claims to proof.
inline std::string task_delivery_summary(
	const std::string &request,
	const std::vector<std::pair<std::string, std::string>> &edits,
	const std::string &model_summary, const std::string &verification,
	bool chinese, const std::string &progress = "")
{
	auto excerpt = [](const std::string &value, size_t limit) {
		return utf8_prefix(value, limit) +
		       (value.size() > limit ? "…" : "");
	};
	std::string out = chinese ? "- 本轮需求：" : "- Requested work: ";
	// Do not use a session title: that deliberately retains only one sentence.
	out += excerpt(request, 600);
	out += "\n" + requirement_progress_summary(progress, chinese);
	if (edits.empty())
		out += chinese ?
			       "\n- 改动记录：本轮没有新增文件修订。" :
			       "\n- Recorded edits: no new file revisions in this run.";
	else {
		out += chinese ? "\n- 改动记录：已生成 " :
				 "\n- Recorded edits: generated ";
		out += std::to_string(edits.size()) +
		       (chinese ? " 项文件修订。" : " file revisions.");
		for (size_t i = 0; i < edits.size() && i < 1; ++i) {
			out += "\n  " + excerpt(edits[i].first, 180);
			if (!edits[i].second.empty())
				out += " — " + excerpt(edits[i].second, 100);
		}
		if (edits.size() > 1)
			out += chinese ?
				       "\n  其余修订见改动列表。" :
				       "\n  See the change list for remaining revisions.";
	}
	if (!model_summary.empty() && model_summary != "Done" &&
	    model_summary != "完成" && model_summary != "已完成") {
		out += chinese ?
			       "\n- AI 处理说明（功能完成情况仍需验证）：" :
			       "\n- AI work report (subject to verification): ";
		out += excerpt(model_summary, 300);
	}
	out += chinese ? "\n- 验证结论：" : "\n- Verification: ";
	out += excerpt(verification, 500);
	return out;
}
}
}
