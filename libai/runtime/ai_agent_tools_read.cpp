#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action { namespace agent_detail {

namespace workspace_tool_detail {

std::string execute_workspace_list(workspace_tool_context_t& context)
{
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!context.read_workspace.list(context.draft_path, entries, context.err)) {
		return workspace_inspection_error(context.err, context.path, context.chinese);
	}
	for (size_t i = 0; i < entries.size(); ++i) {
		entries[i].path = workspace_read_path_to_project(context.project_path, context.live_source_view, entries[i].path);
	}
	// The worktree already includes every proposal. Keep the old overlay only as
	// a defensive fallback for callers that deliberately omit a run identity.
	if (context.run_id.empty() && context.staged_changes != NULL) {
		const std::string prefix = context.path.empty() ? "" : context.path + "/";
		for (size_t i = 0; i < context.staged_changes->size(); ++i) {
			const agent_change_proposal_t& change = (*context.staged_changes)[i];
			std::vector<std::string> remove_paths;
			std::vector<std::pair<std::string, bool> > add_paths;
			if (change.operation == "delete" || change.operation == "move") {
				remove_paths.push_back(change.path);
			}
			if (change.operation == "write" || change.operation == "mkdir"
				|| change.operation == "move") {
				add_paths.push_back(std::make_pair(change.path,
					change.operation == "mkdir"));
			} else if (change.operation == "move") {
				add_paths.push_back(std::make_pair(change.target_path, false));
			}
			for (size_t j = 0; j < remove_paths.size(); ++j) {
				if (remove_paths[j].compare(0, prefix.size(), prefix) != 0) continue;
				const std::string remainder = remove_paths[j].substr(prefix.size());
				if (remainder.empty() || remainder.find('/') != std::string::npos) continue;
				for (std::vector<webcool::ai::workspace_entry_t>::iterator it =
					entries.begin(); it != entries.end();)
				{
					if (it->path == remove_paths[j]) it = entries.erase(it);
					else ++it;
				}
			}
			for (size_t j = 0; j < add_paths.size(); ++j) {
				const std::string& staged_path = add_paths[j].first;
				if (staged_path.compare(0, prefix.size(), prefix) != 0) continue;
				const std::string remainder = staged_path.substr(prefix.size());
				if (remainder.empty()) continue;
				const size_t slash = remainder.find('/');
				const std::string child = prefix + (slash == std::string::npos
					? remainder : remainder.substr(0, slash));
				bool known = false;
				for (size_t k = 0; k < entries.size(); ++k) {
					if (entries[k].path == child) {
						known = true;
						break;
					}
				}
				if (known) continue;
				webcool::ai::workspace_entry_t entry;
				entry.path = child;
				entry.directory = slash != std::string::npos || add_paths[j].second;
				entry.size = (!entry.directory && change.operation == "write")
					? static_cast<long long>(change.content.size()) : 0;
				entry.modified_at = 0;
				entries.push_back(entry);
			}
		}
	}
	acl::json_node& items = context.json.create_array();
	context.root.add_child("entries", items);
	size_t estimated = 0;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (estimated + entries[i].path.size() + 80 > kMaxToolResultBytes) {
			context.trace.truncated = true;
			break;
		}
		acl::json_node& item = items.add_child(false, true);
		item.add_text("path", entries[i].path.c_str());
		item.add_bool("directory", entries[i].directory);
		item.add_number("size", entries[i].size);
		item.add_number("modified_at", entries[i].modified_at);
		estimated += entries[i].path.size() + 80;
	}
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_read_batch(workspace_tool_context_t& context)
{
    acl::json batch_json(context.request.content.c_str());
    acl::json_node* paths = batch_json.finish()
        ? json_array_node(&batch_json.get_root()) : NULL;
    if (!paths) return tool_error_json("read_batch content must be a JSON array of paths or {path, query} objects");
    std::vector<std::pair<std::string, std::string>> requests;
    for (auto* item = paths->first_child(); item; item = paths->next_child()) {
        auto* object = item->is_object() ? item : item->get_obj();
        const std::string path = object ? json_text((*object)["path"]) : json_text(item);
        const std::string query = object ? json_text((*object)["query"]) : "";
        std::string resolved;
        if (!resolve_project_tool_path(path, context.project_path, false, resolved, context.err))
            return workspace_inspection_error(context.err, path, context.chinese);
        if (object && (*object)["query"] && !(*object)["query"]->is_string())
            return tool_error_json("read_batch query must be a decimal byte-offset string");
        requests.emplace_back(resolved, query);
        if (requests.size() > 16) return tool_error_json("read_batch contains more than 16 paths");
    }
    if (requests.empty()) return tool_error_json("read_batch requires at least one path");
    auto& files = context.json.create_array();
    context.root.add_child("files", files);
    acl::json continuation_json;
    auto& continuation = continuation_json.create_array();
    auto& omitted = context.json.create_array();
    context.root.add_child("omitted_paths", omitted);
    size_t total = 0, returned = 0;
    for (const auto& request : requests) {
        const auto queue = [&](const std::string& query) {
            auto& next = continuation.add_child(false, true);
            next.add_text("path", request.first.c_str());
            next.add_text("query", query.c_str());
            context.trace.truncated = true;
        };
        // Keep all omitted paths, even when the payload budget is exhausted.
        if (context.read_batch_bytes - total < 4) {
            queue(request.second);
            omitted.add_array_text(request.first.c_str());
            continue;
        }
        std::string relative, content;
        bool truncated = false;
        if (!project_path_to_draft_path(context.project_path, request.first, relative))
            return tool_error_json("read_batch path cannot be mapped to draft");
        if (context.live_source_view) relative = request.first;
        if (!context.read_workspace.read(relative, content, truncated, context.err))
            return workspace_inspection_error(context.err, request.first, context.chinese);
        if (truncated) return tool_error_json("text file exceeds configured read file limit: " + request.first);
        webcool::ai::text_read_page_t page;
        if (!webcool::ai::read_text_page(content, request.second, page, context.err,
            std::min(context.read_chunk_bytes, context.read_batch_bytes - total), context.read_file_limit_bytes))
            return workspace_inspection_error(context.err, request.first, context.chinese);
        auto& file = files.add_child(false, true);
        file.add_text("path", request.first.c_str());
        file.add_text("file_sha256", webcool::ai::agent_workspace_t::content_sha256(content).c_str());
        file.add_text("content", page.content.c_str());
        file.add_number("offset", page.offset);
        file.add_number("next_offset", page.next_offset);
        file.add_number("total_bytes", page.total_bytes);
        file.add_bool("eof", page.eof);
        file.add_bool("truncated", !page.eof);
        if (!page.eof) {
            const std::string next = std::to_string(page.next_offset);
            file.add_text("next_query", next.c_str());
            file.add_text("read_hint", "Continue with next_content; do not repeat the original batch.");
            queue(next);
        }
        const bool pending = path_has_pending_proposal(context.staged_changes, request.first);
        file.add_bool("staged", pending);
        file.add_bool("pending_review", pending);
        file.add_text("source_view", context.live_source_view ? "formal_source" : "private_draft");
        total += page.content.size();
        ++returned;
    }
    context.root.add_number("requested_files", requests.size());
    context.root.add_number("returned_files", returned);
    context.root.add_text("next_content", serialize_json(continuation).c_str());
    context.root.add_text("read_hint", "If next_content is not [], pass it unchanged as workspace.read_batch content to read remaining pages and omitted files together.");
    return finish_workspace_tool_result(context);
}

std::string execute_workspace_read(workspace_tool_context_t& context)
{
	std::string content;
	bool truncated = false;
	if (!context.read_workspace.read(context.draft_path, content, truncated, context.err)) {
		return workspace_inspection_error(context.err, context.path, context.chinese);
	}
	if (truncated) return tool_error_json("text file exceeds 1 MiB; select a smaller file");
	context.root.add_text("file_sha256",
		webcool::ai::agent_workspace_t::content_sha256(content).c_str());
	webcool::ai::text_read_page_t page;
	if (!webcool::ai::read_text_page(content, context.request.query, page, context.err,
		context.read_chunk_bytes, context.read_file_limit_bytes, true))
		return workspace_inspection_error(context.err, context.path, context.chinese);
	content = page.content;
	truncated = !page.eof;
	context.root.add_number("offset", page.offset);
	context.root.add_number("next_offset", page.next_offset);
	context.root.add_number("total_bytes", page.total_bytes);
	context.root.add_bool("eof", page.eof);
	if (!page.eof) {
		context.root.add_text("next_query", std::to_string(page.next_offset).c_str());
		context.root.add_text("read_hint", "Continue workspace.read with the same path and query=next_query; do not restart at offset 0.");
	}
	context.root.add_text("path", context.path.c_str());
	context.root.add_text("content", content.c_str());
	const bool pending = path_has_pending_proposal(context.staged_changes, context.path);
	context.root.add_bool("staged", pending);
	context.root.add_bool("pending_review", pending);
	context.root.add_text("source_view", context.live_source_view ? "formal_source" : "private_draft");
	context.trace.truncated = truncated;
	return finish_workspace_tool_result(context);
}

} // namespace workspace_tool_detail

} }
