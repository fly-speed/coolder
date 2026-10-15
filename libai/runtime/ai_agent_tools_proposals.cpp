#include "stdafx.h"
#include "ai_agent_tools_internal.h"
#include "../storage/agent_request_store.h"

namespace action
{
namespace agent_detail
{

namespace workspace_tool_detail
{

static std::string finish_proposal_batch(workspace_tool_context_t &context,
    size_t count, const std::vector<std::string> &unchanged_paths,
    const std::vector<std::string> &withdrawn_paths,
    const std::vector<std::string> &missing_deletes)
{
	context.root.add_number("staged_files",
	    static_cast<long long>(count -
	        std::min(count,
	            unchanged_paths.size() + withdrawn_paths.size() +
	                missing_deletes.size())));
	context.root.add_bool("staged", !context.staged_changes->empty());
	acl::json_node &withdrawn = context.json.create_array();
	context.root.add_child("withdrawn_paths", withdrawn);
	for (const auto &withdrawn_path : withdrawn_paths)
		withdrawn.add_array_text(withdrawn_path.c_str());
	acl::json_node &skipped = context.json.create_array();
	context.root.add_child("skipped_unchanged", skipped);
	if (!unchanged_paths.empty())
		context.root.add_text("next_action",
		    prompt_text(
		        prompt_id::unchanged_edit_hint, context.chinese));
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

static bool withdraw_created_proposal(
    std::vector<agent_change_proposal_t> &candidate,
    const std::string &item_path)
{
	bool withdraw = false;
	for (auto it = candidate.begin(); it != candidate.end();) {
		if (it->path != item_path) {
			++it;
			continue;
		}
		withdraw =
		    withdraw || (it->operation == "write" && it->creates_file);
		it = candidate.erase(it);
	}

	return withdraw;
}

// A rejection must identify the actual conflicting records. Retain the exact
// payload; the model chooses which record to keep, never an implicit last-wins.
static std::string duplicate_proposal_batch(workspace_tool_context_t &context,
    const std::string &payload,
    const std::map<std::string, std::vector<size_t>> &occurrences,
    const std::vector<acl::json_node *> &records)
{
    acl::json json; auto &root = json.create_node();
    root.add_bool("ok", false).add_text("error_code", "proposal_batch_duplicate_paths")
        .add_text("error", "propose_batch contains duplicate paths; no changes staged")
        .add_bool("formal_source_changed", false).add_number("staged_files", 0)
        .add_number("index_base", 0).add_number("total_items", records.size());
    auto &conflicts = json.create_array(); root.add_child("duplicate_paths", conflicts);
    size_t total = 0, emitted = 0, bytes = 0;
    for (const auto &entry : occurrences) {
        if (entry.second.size() < 2) continue;
        ++total;
        acl::json one; auto &group = one.create_node();
        group.add_text("path", entry.first.c_str());
        auto &items = one.create_array(); group.add_child("items", items);
        for (size_t index : entry.second) {
            auto *record = records[index];
            const auto content = json_text((*record)["content"]);
            auto &item = items.add_child(false, true);
            item.add_number("index", index)
                .add_text("operation", json_text((*record)["operation"]).empty() ? "write" : json_text((*record)["operation"]).c_str())
                .add_number("content_bytes", content.size())
                .add_text("content_sha256", libai::ai::agent_workspace_t::content_sha256(content).c_str());
        }
        const auto wire = serialize_json(group);
        if (bytes + wire.size() > 8 * 1024) continue;
        conflicts.add_child(json.duplicate_node(group)); bytes += wire.size(); ++emitted;
    }
    root.add_number("duplicate_path_count", total)
        .add_bool("duplicate_paths_complete", emitted == total);
    if (context.saved_proposal_batch && payload.size() <= libai::ai::kMaxSavedProposalBytes) {
        *context.saved_proposal_batch = payload;
        root.add_text("saved_batch", libai::ai::agent_workspace_t::content_sha256(payload).c_str());
        // Audit survives transcript compaction; replay remains scoped to this
        // running task, matching the existing capacity-recovery reference.
        acl::json archive; auto &record = archive.create_node();
        record.add_text("kind", "rejected_duplicate_proposal_batch")
            .add_text("run_id", context.run_id.c_str()).add_text("content", payload.c_str())
            .add_text("content_sha256", libai::ai::agent_workspace_t::content_sha256(payload).c_str())
            .add_bool("executed", false);
        libai::ai::agent_request_store_t store(context.user_root, context.project_path, context.run_id);
        std::string path, error;
        if (store.append_response(serialize_json(record), path, error)) root.add_text("response_archive", path.c_str());
        else root.add_text("archive_error", error.c_str());
        root.add_text("next_action", "The batch was rejected atomically; its files/directories were not created. Do not list missing directories or regenerate the whole batch. Choose the intended record for each duplicate. Inspect exact saved content with {saved_batch,inspect_index,query:\"0\"}; inspect_index is zero-based and query is a byte offset. Replay with {saved_batch,offset:0,limit:TOTAL,exclude_indices:[indices to discard]}. Keep one intended record per path; never discard conflicting content arbitrarily. TOTAL must respect max_items; use offset/limit slices for larger payloads. References last only for this running task.");
        root.add_number("max_items", libai::ai::kMaxProposalBatchItems);
    } else root.add_text("next_action", "No changes staged. Correct the reported duplicate paths and resubmit; do not rediscover directories or assume rejected files exist.");
    return serialize_json(root);
}

std::string execute_workspace_propose_batch(workspace_tool_context_t &context)
{
	if (context.staged_changes == NULL) {
		return tool_error_json(
		    "incremental review storage is unavailable");
	}
	std::string payload = context.request.content;
	long long offset = 0, limit = -1, inspect_index = -1;
	std::set<size_t> excluded;
	bool inspect = false;
	acl::json reference(payload.c_str());
	if (reference.finish() && reference.get_root().is_object() &&
	    !json_text(reference["saved_batch"]).empty()) {
		if (!context.saved_proposal_batch ||
		    context.saved_proposal_batch->empty() ||
		    json_text(reference["saved_batch"]) !=
		        libai::ai::agent_workspace_t::content_sha256(
		            *context.saved_proposal_batch))
			return tool_error_json(
			    "saved_batch is unavailable in this running task");
		inspect = reference["inspect_index"] != NULL;
		if (inspect) {
			inspect_index = json_number(reference["inspect_index"], -1);
			if (inspect_index < 0 || reference["exclude_indices"] || reference["offset"] || reference["limit"])
				return tool_error_json("saved_batch inspection requires one nonnegative inspect_index and optional query only");
			if (reference["query"] && !reference["query"]->is_string())
				return tool_error_json("saved_batch inspection query must be a decimal byte-offset string");
		} else {
			offset = json_number(reference["offset"], -1);
			limit = json_number(reference["limit"], -1);
			if (offset < 0 || limit < 1 || static_cast<size_t>(limit) > libai::ai::kMaxProposalBatchItems)
				return tool_error_json("saved_batch requires offset >= 0 and limit in 1-" + std::to_string(libai::ai::kMaxProposalBatchItems));
			if (reference["exclude_indices"]) {
				auto *array = json_array_node(reference["exclude_indices"]);
				if (!array) return tool_error_json("exclude_indices must be an array of unique zero-based indices");
				for (auto *item=array->first_child();item;item=array->next_child()) {
					const auto index=json_number(item,-1);
					if (index<0 || !excluded.insert(static_cast<size_t>(index)).second || excluded.size()>libai::ai::kMaxProposalBatchItems)
						return tool_error_json("exclude_indices must contain unique nonnegative integer indices within batch capacity");
				}
			}
		}
		payload = *context.saved_proposal_batch;
	}
	acl::json batch_json(payload.c_str());
	acl::json_node *items = batch_json.finish() ?
	    json_array_node(&batch_json.get_root()) :
	    NULL;
	if (items == NULL)
		return tool_error_json(
		    "propose_batch content must be a JSON array or saved_batch reference");
	std::vector<acl::json_node *> records;
	for (auto *item=items->first_child();item;item=items->next_child())
		records.push_back(item->is_object()?item:item->get_obj());
	if (!excluded.empty() && *excluded.rbegin() >= records.size())
		return tool_error_json("exclude_indices index is outside the saved batch");
	if (inspect) {
		if (static_cast<size_t>(inspect_index)>=records.size() || !records[inspect_index])
			return tool_error_json("inspect_index is outside the saved batch or not a file object");
		auto &item=*records[inspect_index]; std::string path;
		if (!resolve_project_tool_path(json_text(item["path"]), context.project_path, false, path, context.err))
			return tool_error_json(context.err);
		const auto content=json_text(item["content"]);
		libai::ai::text_read_page_t page;
		if (!libai::ai::read_text_page(content,json_text(reference["query"]),page,context.err,
		        std::min<size_t>(context.read_chunk_bytes,4096),libai::ai::kMaxProposalFileBytes))
			return tool_error_json(context.err);
		context.root.add_bool("inspection_only",true).add_text("source_view","rejected_proposal_record").add_bool("formal_source_changed",false)
		    .add_bool("staged",false).add_number("inspect_index",inspect_index)
		    .add_text("saved_batch",json_text(reference["saved_batch"]).c_str())
		    .add_text("path",path.c_str()).add_text("file_sha256",libai::ai::agent_workspace_t::content_sha256(content).c_str())
		    .add_text("content",page.content.c_str()).add_number("offset",page.offset)
		    .add_number("next_offset",page.next_offset).add_number("total_bytes",page.total_bytes).add_bool("eof",page.eof);
		if (!page.eof) context.root.add_text("next_query",std::to_string(page.next_offset).c_str());
		return finish_workspace_tool_result(context);
	}
	const auto selected_index = [&](size_t index) {
		return index>=static_cast<size_t>(offset) &&
		    (limit<0 || index-static_cast<size_t>(offset)<static_cast<size_t>(limit)) && !excluded.count(index);
	};
	size_t total = 0, selected = 0, selected_bytes = 0;
	for (acl::json_node *item = items->first_child(); item;
	     item = items->next_child(), ++total) {
		if (!selected_index(total))
			continue;
		++selected;
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (!object)
			continue;
		selected_bytes += json_text((*object)["content"]).size();
	}
	if (selected > libai::ai::kMaxProposalBatchItems ||
	    selected_bytes > libai::ai::kMaxProposalBatchBytes) {
		acl::json failure;
		acl::json_node &out = failure.get_root();
		out.add_bool("ok", false);
		out.add_text("code", "proposal_batch_capacity");
		out.add_text("error",
		    context.chinese ?
		        "批次超过容量，未暂存任何修改" :
		        "Batch exceeds capacity; no changes were staged");
		out.add_number(
		    "max_items", libai::ai::kMaxProposalBatchItems);
		out.add_number(
		    "max_content_bytes", libai::ai::kMaxProposalBatchBytes);
		out.add_number("received_items", selected);
		out.add_number("received_content_bytes", selected_bytes);
		if (!(context.saved_proposal_batch &&
		        payload.size() <= libai::ai::kMaxSavedProposalBytes))
			return serialize_json(out);
		*context.saved_proposal_batch = payload;
		out.add_text("saved_batch",
		    libai::ai::agent_workspace_t::content_sha256(payload)
		        .c_str());
		out.add_number("total_items", total);
		out.add_text("next_action",
		    prompt_text(
		        prompt_id::proposal_batch_replay, context.chinese));

		return serialize_json(out);
	}
	std::map<std::string,std::vector<size_t>> occurrences;
	for (size_t i=0;i<records.size();++i) {
		if (!selected_index(i)) continue;
		std::string path;
		if (!records[i] || !resolve_project_tool_path(json_text((*records[i])["path"]),
		        context.project_path,false,path,context.err)) return tool_error_json(context.err.empty()?"propose_batch item must be an object":context.err);
		occurrences[path].push_back(i);
	}
	for (const auto &entry:occurrences)
		if (entry.second.size()>1) return duplicate_proposal_batch(context,payload,occurrences,records);
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
		if (!selected_index(index)) continue;
		++count;
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		std::string item_path;
		const std::string item_content =
		    object ? json_text((*object)["content"]) : "";
		if (object == NULL ||
		    !resolve_project_tool_path(json_text((*object)["path"]),
		        context.project_path, false, item_path, context.err)) {
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
			const bool withdraw =
			    withdraw_created_proposal(candidate, item_path);
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
		if (item_content.size() > libai::ai::kMaxProposalFileBytes ||
		    bytes > libai::ai::kMaxProposalBatchBytes ||
		    item_content.find('\0') != std::string::npos) {
			return tool_error_json(
			    "propose_batch invalid text at " + item_path +
			    ": each file must be NUL-free and at most " +
			    std::to_string(libai::ai::kMaxProposalFileBytes) +
			    " bytes; no changes staged");
		}
		bool already_staged = false;
		for (const auto &existing : candidate) {
			if (!(existing.operation == "write" &&
			        existing.path == item_path &&
			        existing.content == item_content))
				continue;
			already_staged = true;
			break;
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
	const size_t rejected =
	    validate_change_proposals(context.workspace, context.project_path,
	        candidate, &failures, true, &unchanged_paths, &missing_deletes);
	if (rejected != 0) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	return finish_proposal_batch(
	    context, count, unchanged_paths, withdrawn_paths, missing_deletes);
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
		context.root.add_text("next_action",
		    prompt_text(
		        prompt_id::unchanged_edit_hint, context.chinese));
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
	const size_t rejected = validate_change_proposals(context.workspace,
	    context.project_path, candidate, &failures, true, &unchanged_paths);
	if (rejected != 0) {
		return proposal_error_json(failures, context.chinese);
	}
	context.staged_changes->swap(candidate);
	context.root.add_text("path", context.path.c_str());
	context.root.add_bool("staged", !context.staged_changes->empty());
	acl::json_node &skipped = context.json.create_array();
	context.root.add_child("skipped_unchanged", skipped);
	if (!unchanged_paths.empty())
		context.root.add_text("next_action",
		    prompt_text(
		        prompt_id::unchanged_edit_hint, context.chinese));
	for (const auto &unchanged : unchanged_paths) {
		acl::json_node &item = skipped.add_child(false, true);
		item.add_text("path", unchanged.c_str());
		item.add_text("reason", "unchanged_content");
	}
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_propose_operation(
    workspace_tool_context_t &context)
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
		context.root.add_text(
		    "target_path", context.request.target_path.c_str());
	}
	context.root.add_bool("staged", true);
	context.root.add_bool("formal_source_changed", false);
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_mkdir(workspace_tool_context_t &context)
{
	if (!context.workspace.create_directory_if_absent(
	        context.path, context.err)) {
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
