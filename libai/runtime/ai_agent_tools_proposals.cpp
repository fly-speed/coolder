#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action
{
namespace agent_detail
{

namespace workspace_tool_detail
{

std::string execute_workspace_propose_batch(workspace_tool_context_t &context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json(
			"incremental review storage is unavailable");
	}
	std::string payload = context.request.content;
	long long offset = 0, limit = -1;
	acl::json reference(payload.c_str());
	if (reference.finish() &&
	    !json_text(reference["saved_batch"]).empty()) {
		if (!context.saved_proposal_batch ||
		    context.saved_proposal_batch->empty() ||
		    json_text(reference["saved_batch"]) !=
			    webcool::ai::agent_workspace_t::content_sha256(
				    *context.saved_proposal_batch))
			return tool_error_json(
				"saved_batch is unavailable in this running task");
		offset = json_number(reference["offset"], -1);
		limit = json_number(reference["limit"], -1);
		if (offset < 0 || limit < 1 ||
		    static_cast<size_t>(limit) >
			    webcool::ai::kMaxProposalBatchItems)
			return tool_error_json(
				"saved_batch requires offset >= 0 and limit in 1-" +
				std::to_string(
					webcool::ai::kMaxProposalBatchItems));
		payload = *context.saved_proposal_batch;
	}
	acl::json batch_json(payload.c_str());
	acl::json_node *items =
		batch_json.finish() ? json_array_node(&batch_json.get_root()) :
				      NULL;
	if (items == NULL)
		return tool_error_json(
			"propose_batch content must be a JSON array or saved_batch reference");
	size_t total = 0, selected = 0, selected_bytes = 0;
	for (acl::json_node *item = items->first_child(); item;
	     item = items->next_child(), ++total) {
		if (total < static_cast<size_t>(offset) ||
		    (limit >= 0 && selected >= static_cast<size_t>(limit)))
			continue;
		++selected;
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		if (object)
			selected_bytes +=
				json_text((*object)["content"]).size();
	}
	if (selected > webcool::ai::kMaxProposalBatchItems ||
	    selected_bytes > webcool::ai::kMaxProposalBatchBytes) {
		acl::json failure;
		acl::json_node &out = failure.get_root();
		out.add_bool("ok", false);
		out.add_text("code", "proposal_batch_capacity");
		out.add_text(
			"error",
			context.chinese ?
				"批次超过容量，未暂存任何修改" :
				"Batch exceeds capacity; no changes were staged");
		out.add_number("max_items",
			       webcool::ai::kMaxProposalBatchItems);
		out.add_number("max_content_bytes",
			       webcool::ai::kMaxProposalBatchBytes);
		out.add_number("received_items", selected);
		out.add_number("received_content_bytes", selected_bytes);
		if (context.saved_proposal_batch &&
		    payload.size() <= webcool::ai::kMaxSavedProposalBytes) {
			*context.saved_proposal_batch = payload;
			out.add_text(
				"saved_batch",
				webcool::ai::agent_workspace_t::content_sha256(
					payload)
					.c_str());
			out.add_number("total_items", total);
			out.add_text(
				"next_action",
				prompt_text(prompt_id::proposal_batch_replay,
					    context.chinese));
		}
		return serialize_json(out);
	}
	std::vector<agent_change_proposal_t> candidate =
		*context.staged_changes;
	std::vector<std::string> unchanged_paths;
	std::vector<std::string> withdrawn_paths;
	std::vector<std::string> missing_deletes;
	std::set<std::string> batch_paths;
	size_t count = 0;
	size_t bytes = 0;
	size_t index = 0;
	for (acl::json_node *item = items->first_child(); item != NULL;
	     item = items->next_child(), ++index) {
		if (index < static_cast<size_t>(offset))
			continue;
		if (limit >= 0 && count >= static_cast<size_t>(limit))
			break;
		++count;
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		std::string item_path;
		const std::string item_content =
			object ? json_text((*object)["content"]) : "";
		if (object == NULL ||
		    !resolve_project_tool_path(json_text((*object)["path"]),
					       context.project_path, false,
					       item_path, context.err)) {
			return tool_error_json(context.err);
		}
		if (!batch_paths.insert(item_path).second)
			return tool_error_json(
				"propose_batch contains duplicate paths");
		std::string operation = json_text((*object)["operation"]);
		if (operation.empty())
			operation = "write";
		if (operation != "write" && operation != "delete")
			return tool_error_json(
				"propose_batch operation must be write or delete");
		if (operation == "delete") {
			if (!item_content.empty())
				return tool_error_json(
					"delete proposal must not contain file content");
			bool withdraw = false;
			for (auto it = candidate.begin();
			     it != candidate.end();) {
				if (it->path != item_path) {
					++it;
					continue;
				}
				withdraw =
					withdraw || (it->operation == "write" &&
						     it->creates_file);
				it = candidate.erase(it);
			}
			if (withdraw)
				withdrawn_paths.push_back(item_path);
			else {
				agent_change_proposal_t proposal;
				proposal.operation = "delete";
				proposal.path = item_path;
				proposal.reason =
					json_text((*object)["reason"]);
				candidate.push_back(proposal);
			}
			continue;
		}
		bytes += item_content.size();
		if (item_content.size() > webcool::ai::kMaxProposalFileBytes ||
		    bytes > webcool::ai::kMaxProposalBatchBytes ||
		    item_content.find('\0') != std::string::npos) {
			return tool_error_json(
				"propose_batch invalid text at " + item_path +
				": each file must be NUL-free and at most " +
				std::to_string(
					webcool::ai::kMaxProposalFileBytes) +
				" bytes; no changes staged");
		}
		bool already_staged = false;
		for (const auto &existing : candidate) {
			if (existing.operation == "write" &&
			    existing.path == item_path &&
			    existing.content == item_content) {
				already_staged = true;
				break;
			}
		}
		if (already_staged) {
			unchanged_paths.push_back(item_path);
			continue;
		}
		for (std::vector<agent_change_proposal_t>::iterator it =
			     candidate.begin();
		     it != candidate.end();) {
			if (it->path == item_path)
				it = candidate.erase(it);
			else
				++it;
		}
		agent_change_proposal_t proposal;
		proposal.operation = "write";
		proposal.path = item_path;
		proposal.content = item_content;
		proposal.reason = json_text((*object)["reason"]);
		if (proposal.reason.empty())
			proposal.reason = "AI 批量生成的待审查文件";
		candidate.push_back(proposal);
	}
	if (count == 0)
		return tool_error_json(
			"propose_batch requires at least one file");
	std::vector<proposal_validation_error_t> failures;
	const size_t rejected = validate_change_proposals(
		context.workspace, context.project_path, candidate, &failures,
		true, &unchanged_paths, &missing_deletes);
	if (rejected != 0) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	context.root.add_number(
		"staged_files",
		static_cast<long long>(
			count -
			std::min(count, unchanged_paths.size() +
						withdrawn_paths.size() +
						missing_deletes.size())));
	context.root.add_bool("staged", !context.staged_changes->empty());
	acl::json_node &withdrawn = context.json.create_array();
	context.root.add_child("withdrawn_paths", withdrawn);
	for (const auto &withdrawn_path : withdrawn_paths)
		withdrawn.add_array_text(withdrawn_path.c_str());
	acl::json_node &skipped = context.json.create_array();
	context.root.add_child("skipped_unchanged", skipped);
	if (!unchanged_paths.empty())
		context.root.add_text(
			"next_action",
			prompt_text(prompt_id::unchanged_edit_hint,
				    context.chinese));
	for (const auto &unchanged : unchanged_paths) {
		acl::json_node &item = skipped.add_child(false, true);
		item.add_text("path", unchanged.c_str());
		item.add_text("reason", "unchanged_content");
	}
	acl::json_node &missing = context.json.create_array();
	context.root.add_child("skipped_missing_deletes", missing);
	for (const auto &missing_path : missing_deletes)
		missing.add_array_text(missing_path.c_str());
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_propose(workspace_tool_context_t &context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json(
			"incremental review storage is unavailable");
	}
	if (context.request.content.size() > 256 * 1024 ||
	    context.request.content.find('\0') != std::string::npos) {
		return tool_error_json(
			"staged generated file must be text no larger than 256 KiB");
	}
	for (const auto &prior : *context.staged_changes) {
		if (prior.operation != "write" || prior.path != context.path ||
		    prior.content != context.request.content)
			continue;
		context.root.add_text("path", context.path.c_str());
		context.root.add_bool("staged", true);
		context.root.add_bool("formal_source_changed", false);
		acl::json_node &skipped = context.json.create_array();
		context.root.add_child("skipped_unchanged", skipped);
		acl::json_node &item = skipped.add_child(false, true);
		item.add_text("path", context.path.c_str());
		item.add_text("reason", "unchanged_content");
		context.root.add_text(
			"next_action",
			prompt_text(prompt_id::unchanged_edit_hint,
				    context.chinese));
		context.trace.ok = true;
		return serialize_json(context.root);
	}
	// Replace an earlier proposal for the same path. This lets a model refine
	// one file over several turns without creating duplicate review entries.
	std::vector<agent_change_proposal_t> candidate =
		*context.staged_changes;
	for (std::vector<agent_change_proposal_t>::iterator it =
		     candidate.begin();
	     it != candidate.end();) {
		if (it->path == context.path)
			it = candidate.erase(it);
		else
			++it;
	}
	agent_change_proposal_t proposal;
	proposal.operation = "write";
	proposal.path = context.path;
	proposal.content = context.request.content;
	proposal.reason = "AI 增量生成的待审查文件";
	candidate.push_back(proposal);
	std::vector<proposal_validation_error_t> failures;
	std::vector<std::string> unchanged_paths;
	const size_t rejected = validate_change_proposals(
		context.workspace, context.project_path, candidate, &failures,
		true, &unchanged_paths);
	if (rejected != 0) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	context.root.add_text("path", context.path.c_str());
	context.root.add_bool("staged", !context.staged_changes->empty());
	acl::json_node &skipped = context.json.create_array();
	context.root.add_child("skipped_unchanged", skipped);
	if (!unchanged_paths.empty())
		context.root.add_text(
			"next_action",
			prompt_text(prompt_id::unchanged_edit_hint,
				    context.chinese));
	for (const auto &unchanged : unchanged_paths) {
		acl::json_node &item = skipped.add_child(false, true);
		item.add_text("path", unchanged.c_str());
		item.add_text("reason", "unchanged_content");
	}
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string
execute_workspace_propose_operation(workspace_tool_context_t &context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json(
			"incremental review storage is unavailable");
	}
	std::vector<agent_change_proposal_t> candidate =
		*context.staged_changes;
	agent_change_proposal_t prior;
	bool had_prior = false;
	for (std::vector<agent_change_proposal_t>::iterator it =
		     candidate.begin();
	     it != candidate.end();) {
		if (it->path == context.path) {
			prior = *it;
			had_prior = true;
			it = candidate.erase(it);
		} else
			++it;
	}
	if (context.request.name == "workspace.propose_delete" && had_prior &&
	    prior.operation == "write" && prior.creates_file) {
		// Deleting an unaccepted new file simply withdraws its proposal. The
		// formal workspace never contained this path.
		context.staged_changes->swap(candidate);
		context.root.add_text("path", context.path.c_str());
		context.root.add_bool("staged", false);
		context.root.add_bool("proposal_withdrawn", true);
		context.root.add_bool("formal_source_changed", false);
		context.root.add_bool("truncated", false);
		context.trace.ok = true;
		return serialize_json(context.root);
	}
	agent_change_proposal_t proposal;
	proposal.path = context.path;
	if (context.request.name == "workspace.propose_delete") {
		proposal.operation = "delete";
		proposal.reason = "AI 建议删除的待审查文件";
	} else if (context.request.name == "workspace.propose_mkdir") {
		proposal.operation = "mkdir";
		proposal.reason = "AI 建议创建的待审查目录";
	} else if (had_prior && prior.operation == "write" &&
		   prior.creates_file) {
		// Moving a newly proposed file is equivalent to retargeting its pending
		// write. This keeps the operation reviewable without requiring a source
		// file that does not yet exist on disk.
		proposal.operation = "write";
		proposal.path = context.request.target_path;
		proposal.content = prior.content;
		proposal.reason = "AI 重命名的待审查新文件";
	} else {
		proposal.operation = "move";
		proposal.target_path = context.request.target_path;
		proposal.reason = "AI 建议移动的待审查文件";
	}
	candidate.push_back(proposal);
	std::vector<proposal_validation_error_t> failures;
	const size_t rejected = validate_change_proposals(
		context.workspace, context.project_path, candidate, &failures);
	if (rejected != 0 || candidate.empty()) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	context.root.add_text("path", context.path.c_str());
	if (!context.request.target_path.empty()) {
		context.root.add_text("target_path",
				      context.request.target_path.c_str());
	}
	context.root.add_bool("staged", true);
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_mkdir(workspace_tool_context_t &context)
{
	if (!context.workspace.create_directory_if_absent(context.path,
							  context.err)) {
		return tool_error_json(context.err);
	}
	context.root.add_text("path", context.path.c_str());
	context.root.add_bool("created", true);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_create(workspace_tool_context_t &context)
{
	if (context.request.content.size() > 256 * 1024 ||
	    context.request.content.find('\0') != std::string::npos) {
		return tool_error_json(
			"new generated file must be text no larger than 256 KiB");
	}
	if (!context.workspace.create_text_if_absent(
		    context.path, context.request.content, context.err)) {
		return tool_error_json(context.err);
	}
	context.root.add_text("path", context.path.c_str());
	context.root.add_bool("created", true);
	return finish_workspace_tool_result(context);
}

} // namespace workspace_tool_detail

}
}
