#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action { namespace agent_detail {

namespace workspace_tool_detail {

std::string execute_workspace_patch_set(workspace_tool_context_t& context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json("incremental review storage is unavailable");
	}
	if (context.request.content.empty() || context.request.content.size() > 512 * 1024
		|| context.request.content.find('\0') != std::string::npos)
	{
		return tool_error_json("patch_set JSON must contain at most 512 KiB");
	}
	acl::json patch_json(context.request.content.c_str());
	acl::json_node* replacements = patch_json.finish()
		? json_array_node(&patch_json.get_root()) : NULL;
	if (replacements == NULL) {
		return tool_error_json("patch_set content must be a JSON array");
	}
	std::string current;
	std::string formal_baseline;
	bool formal_baseline_available = false;
	for (size_t i = 0; i < context.staged_changes->size(); ++i) {
		if ((*context.staged_changes)[i].path == context.path
			&& (*context.staged_changes)[i].operation == "write")
		{
			current = (*context.staged_changes)[i].content;
			formal_baseline = (*context.staged_changes)[i].original_content;
			formal_baseline_available =
				(*context.staged_changes)[i].original_content_available;
			break;
		}
	}
	if (current.empty() && !formal_baseline_available) {
		bool truncated = false;
		if (!context.workspace.read(context.path, current, truncated, context.err) || truncated) {
			if (context.err.empty()) context.err = "cannot read complete file for patch_set";
			webcool::ai::ai_log_error("agent.runtime", "prepare-patch-set", context.err);
			return tool_error_json(context.err);
		}
	}
	const std::string unmodified_source = current;
	size_t replacement_count = 0;
	for (acl::json_node* item = replacements->first_child(); item != NULL;
		item = replacements->next_child())
	{
		if (++replacement_count > 32) {
			return tool_error_json("patch_set contains more than 32 replacements");
		}
		acl::json_node* object = item->is_object() ? item : item->get_obj();
		const std::string old_text = object ? json_text((*object)["old_text"]) : "";
		if (object == NULL || (*object)["new_text"] == NULL
			|| !(*object)["new_text"]->is_string())
		{
			return tool_error_json(prompt_text(prompt_id::patch_text_required, context.chinese));
		}
		const std::string new_text = object ? json_text((*object)["new_text"]) : "";
		if (old_text.empty() || old_text.size() > 256 * 1024
			|| new_text.size() > 256 * 1024)
		{
			return tool_error_json("patch_set contains an invalid replacement");
		}
		const size_t first = current.find(old_text);
		if (first == std::string::npos) {
			return edit_rebase_error("patch_set old_text does not match current content", context.path, unmodified_source, context.chinese);
		}
		if (current.find(old_text, first + old_text.size()) != std::string::npos) {
			return tool_error_json(
				"patch_set old_text is ambiguous; include more context");
		}
		current.replace(first, old_text.size(), new_text);
		if (current.size() > 1024 * 1024) {
			return tool_error_json("patched file exceeds 1 MiB");
		}
	}
	if (replacement_count == 0) {
		return tool_error_json("patch_set must contain at least one replacement");
	}
	std::vector<agent_change_proposal_t> candidate = *context.staged_changes;
	for (std::vector<agent_change_proposal_t>::iterator it = candidate.begin();
		it != candidate.end();)
	{
		if (it->path == context.path) it = candidate.erase(it);
		else ++it;
	}
	if (formal_baseline_available && current == formal_baseline) {
		context.staged_changes->swap(candidate);
		context.root.add_bool("staged", false);
		context.root.add_bool("proposal_withdrawn", true);
	} else {
		agent_change_proposal_t proposal;
		proposal.operation = "write";
		proposal.path = context.path;
		proposal.content = current;
		proposal.reason = "AI 多块原子补丁产生的待审查修改";
		candidate.push_back(proposal);
		std::vector<proposal_validation_error_t> failures;
		const size_t rejected = validate_change_proposals(context.workspace,
			context.project_path, candidate, &failures);
		if (rejected != 0 || candidate.empty()) {
			return proposal_error_json(failures, context.chinese);
		}
		context.staged_changes->swap(candidate);
		context.root.add_bool("staged", true);
	}
	context.root.add_text("path", context.path.c_str());
        append_edit_snapshot(context.json, context.root, current,
            webcool::ai::first_edit_offset(unmodified_source, current), context.chinese);
	context.root.add_number("replacement_count",
		static_cast<long long>(replacement_count));
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_replace(workspace_tool_context_t& context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json("incremental review storage is unavailable");
	}
	if (!context.request.content_present && context.request.content.empty()) {
		return tool_error_json(prompt_text(prompt_id::replace_content_required, context.chinese));
	}
	if (context.request.old_text.empty()) {
		return tool_error_json("old_text must not be empty");
	}
	if (context.request.old_text.size() > 256 * 1024
		|| context.request.content.size() > 256 * 1024
		|| context.request.old_text.find('\0') != std::string::npos
		|| context.request.content.find('\0') != std::string::npos)
	{
		return tool_error_json("replacement text exceeds the safe text limit");
	}
	// Continue editing the latest staged version when the same file has already
	// been proposed. Otherwise read the trusted formal source as the baseline.
	std::string current;
	bool found_staged_write = false;
	std::string formal_baseline;
	bool formal_baseline_available = false;
	for (size_t i = 0; i < context.staged_changes->size(); ++i) {
		if ((*context.staged_changes)[i].path == context.path
			&& (*context.staged_changes)[i].operation == "write")
		{
			current = (*context.staged_changes)[i].content;
			found_staged_write = true;
			formal_baseline = (*context.staged_changes)[i].original_content;
			formal_baseline_available =
				(*context.staged_changes)[i].original_content_available;
			break;
		}
	}
	if (!found_staged_write) {
		bool truncated = false;
		if (!context.workspace.read(context.path, current, truncated, context.err) || truncated) {
			if (context.err.empty()) context.err = "cannot read complete file for exact replacement";
			webcool::ai::ai_log_error("agent.runtime",
				"prepare-exact-replacement", context.err);
			return tool_error_json(context.err);
		}
	}
	const size_t first = current.find(context.request.old_text);
	if (first == std::string::npos) {
		return edit_rebase_error(
			"old_text does not match the current staged or formal file; "
			"read this file again and rebase the edit. A matching new fragment "
			"elsewhere is not evidence that this edit was applied.", context.path, current, context.chinese);
	}
	if (current.find(context.request.old_text, first + context.request.old_text.size())
		!= std::string::npos)
	{
		return tool_error_json(
			"old_text is ambiguous; include more surrounding context");
	}
	current.replace(first, context.request.old_text.size(), context.request.content);
	std::vector<agent_change_proposal_t> candidate = *context.staged_changes;
	for (std::vector<agent_change_proposal_t>::iterator it = candidate.begin();
		it != candidate.end();)
	{
		if (it->path == context.path) it = candidate.erase(it);
		else ++it;
	}
	if (formal_baseline_available && current == formal_baseline) {
		// A sequence of exact edits can intentionally return to the trusted
		// baseline. Remove the obsolete proposal instead of reporting a no-op as
		// a validation failure.
		context.staged_changes->swap(candidate);
		context.root.add_text("path", context.path.c_str());
		context.root.add_bool("staged", false);
		context.root.add_bool("proposal_withdrawn", true);
		context.root.add_bool("formal_source_changed", false);
	} else {
	agent_change_proposal_t proposal;
	proposal.operation = "write";
	proposal.path = context.path;
	proposal.content = current;
	proposal.reason = "AI 精确文本替换产生的待审查修改";
	candidate.push_back(proposal);
	std::vector<proposal_validation_error_t> failures;
	const size_t rejected = validate_change_proposals(context.workspace,
		context.project_path, candidate, &failures);
	if (rejected != 0 || candidate.empty()) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	context.root.add_text("path", context.path.c_str());
	context.root.add_bool("staged", true);
	context.root.add_bool("formal_source_changed", false);
	context.root.add_number("replaced_bytes",
		static_cast<long long>(context.request.old_text.size()));
	}
        append_edit_snapshot(context.json, context.root, current, first, context.chinese);
	return finish_workspace_tool_result(context);
}

} // namespace workspace_tool_detail

} }
