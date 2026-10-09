#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action
{
namespace agent_detail
{

namespace workspace_tool_detail
{

static bool overlay_search_change(workspace_tool_context_t &context, size_t i,
    std::vector<webcool::ai::workspace_match_t> &matches, bool &truncated)
{

	const agent_change_proposal_t &change = (*context.staged_changes)[i];
	if (!path_is_in_project(context.path, change.path))
		return true;
	// Formal matches for a staged file are stale. Remove them and index the
	// private proposed contents in memory for this response only.
	for (std::vector<webcool::ai::workspace_match_t>::iterator it =
	         matches.begin();
	     it != matches.end();) {
		if (it->path == change.path)
			it = matches.erase(it);
		else
			++it;
	}
	if (change.operation != "write")
		return true;
	size_t begin = 0;
	unsigned long line = 1;
	while (begin <= change.content.size() && matches.size() < 200) {
		const size_t end = change.content.find('\n', begin);
		std::string text = change.content.substr(begin,
		    end == std::string::npos ? std::string::npos : end - begin);
		if (text.find(context.request.query) != std::string::npos) {
			webcool::ai::workspace_match_t match;
			match.path = change.path;
			match.line = line;
			if (text.size() > 500)
				text.resize(500);
			match.text = text;
			matches.push_back(match);
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
		++line;
	}
	if (!(matches.size() >= 200))
		return true;
	truncated = true;

	return true;
}

std::string execute_workspace_search(workspace_tool_context_t &context)
{
	if (context.request.name == "code.references" &&
	    (context.request.query.empty() ||
	        context.request.query.size() > 256)) {
		return tool_error_json(
		    "reference query must contain 1-256 bytes");
	}
	std::vector<webcool::ai::workspace_match_t> matches;
	bool truncated = false;
	if (!context.read_workspace.search(context.draft_path,
	        context.request.query, matches, truncated, context.err)) {
		return workspace_inspection_error(
		    context.err, context.path, context.chinese);
	}
	for (size_t i = 0; i < matches.size(); ++i) {
		matches[i].path =
		    workspace_read_path_to_project(context.project_path,
		        context.live_source_view, matches[i].path);
	}
	if (context.run_id.empty() && context.staged_changes != NULL) {
		for (size_t i = 0; i < context.staged_changes->size(); ++i) {
			if (!overlay_search_change(
			        context, i, matches, truncated))
				break;
		}
	}
	acl::json_node &items = context.json.create_array();
	context.root.add_child(context.request.name == "code.references" ?
	        "references" :
	        "matches",
	    items);
	size_t estimated = 0;
	for (size_t i = 0; i < matches.size(); ++i) {
		if (estimated + matches[i].path.size() +
		        matches[i].text.size() + 80 >
		    kMaxToolResultBytes) {
			truncated = true;
			break;
		}
		acl::json_node &item = items.add_child(false, true);
		item.add_text("path", matches[i].path.c_str());
		item.add_number(
		    "line", static_cast<long long>(matches[i].line));
		item.add_text("text", matches[i].text.c_str());
		estimated +=
		    matches[i].path.size() + matches[i].text.size() + 80;
	}
	context.trace.truncated = truncated;
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_symbols(workspace_tool_context_t &context)
{
	if (context.request.query.empty() ||
	    context.request.query.size() > 256) {
		return tool_error_json("symbol query must contain 1-256 bytes");
	}
	if (context.staged_changes != NULL &&
	    !context.staged_changes->empty()) {
		// The persisted project index intentionally describes formal source. While
		// a run has unpublished generations, query the authoritative draft tree so
		// a newly created or renamed symbol is visible immediately. Results are
		// bounded textual symbol candidates; workspace.outline supplies structural
		// details for the selected file.
		std::vector<webcool::ai::workspace_match_t> matches;
		bool truncated = false;
		if (!context.read_workspace.search(context.draft_path,
		        context.request.query, matches, truncated, context.err))
			return workspace_inspection_error(
			    context.err, context.path, context.chinese);
		acl::json_node &items = context.json.create_array();
		context.root.add_child("symbols", items);
		size_t estimated = 0;
		for (size_t i = 0; i < matches.size() && i < 100; ++i) {
			const std::string full_path =
			    workspace_read_path_to_project(context.project_path,
			        context.live_source_view, matches[i].path);
			if (estimated + full_path.size() +
			        matches[i].text.size() + 80 >
			    kMaxToolResultBytes) {
				truncated = true;
				break;
			}
			acl::json_node &item = items.add_child(false, true);
			item.add_text("path", full_path.c_str());
			item.add_number(
			    "line", static_cast<long long>(matches[i].line));
			item.add_text("kind", "draft_match");
			item.add_text("text", matches[i].text.c_str());
			estimated +=
			    full_path.size() + matches[i].text.size() + 80;
		}
		context.root.add_number("revision", 0);
		context.root.add_bool("draft", true);
		context.root.add_bool(
		    "truncated", truncated || matches.size() > 100);
		context.trace.truncated = truncated || matches.size() > 100;
		context.trace.ok = true;
		const std::string result = serialize_json(context.root);
		if (!(result.size() > kMaxToolResultBytes * 2))
			return result;
		context.trace.ok = false;
		context.trace.truncated = true;
		return tool_error_json("tool result exceeds the context limit");
	}
	webcool::ai::agent_project_store_t project_store(context.user_root);
	std::vector<webcool::ai::agent_project_record_t> projects;
	if (!project_store.list(0, projects, context.err))
		return workspace_inspection_error(
		    context.err, context.path, context.chinese);
	const webcool::ai::agent_project_record_t *project = NULL;
	for (size_t i = 0; i < projects.size(); ++i) {
		if (!(projects[i].project_path == context.project_path))
			continue;
		project = &projects[i];
		break;
	}
	if (project == NULL)
		return tool_error_json("project semantic index is unavailable");
	webcool::ai::agent_project_index_store_t store(context.user_root);
	webcool::ai::agent_project_index_snapshot_t snapshot;
	// Refresh on every semantic query. The index implementation fingerprints
	// files and only reparses changed sources, so this keeps results current
	// without turning each query into a full-project source scan.
	if (!store.refresh(
	        *project, context.workspace, snapshot, context.err)) {
		return workspace_inspection_error(
		    context.err, context.path, context.chinese);
	}
	std::string needle = context.request.query;
	for (size_t i = 0; i < needle.size(); ++i) {
		needle[i] = static_cast<char>(
		    std::tolower(static_cast<unsigned char>(needle[i])));
	}
	acl::json_node &items = context.json.create_array();
	context.root.add_child("symbols", items);
	size_t count = 0;
	for (size_t i = 0; i < snapshot.files.size() && count < 100; ++i) {
		const webcool::ai::agent_project_index_entry_t &file =
		    snapshot.files[i];
		if (!path_is_in_project(context.path, file.path))
			continue;
		for (size_t j = 0; j < file.symbols.size() && count < 100;
		     ++j) {
			std::string haystack = file.symbols[j].text;
			for (size_t k = 0; k < haystack.size(); ++k) {
				haystack[k] = static_cast<char>(std::tolower(
				    static_cast<unsigned char>(haystack[k])));
			}
			if (haystack.find(needle) == std::string::npos)
				continue;
			acl::json_node &item = items.add_child(false, true);
			item.add_text("path", file.path.c_str());
			item.add_number("line",
			    static_cast<long long>(file.symbols[j].line));
			item.add_text("kind", file.symbols[j].kind.c_str());
			item.add_text("text", file.symbols[j].text.c_str());
			if (!file.module_id.empty()) {
				item.add_text("module", file.module_id.c_str());
			}
			++count;
		}
	}
	context.root.add_number("revision", snapshot.revision);
	context.root.add_bool("truncated", count >= 100);
	context.trace.truncated = count >= 100;
	return finish_workspace_tool_result(context);
}

std::string execute_workspace_outline(workspace_tool_context_t &context)
{
	std::vector<webcool::ai::workspace_outline_item_t> outline;
	bool truncated = false;
	if (!context.read_workspace.outline(
	        context.draft_path, outline, truncated, context.err)) {
		return workspace_inspection_error(
		    context.err, context.path, context.chinese);
	}
	context.root.add_text("path", context.path.c_str());
	acl::json_node &items = context.json.create_array();
	context.root.add_child("items", items);
	size_t estimated = 0;
	for (size_t i = 0; i < outline.size(); ++i) {
		if (estimated + outline[i].kind.size() +
		        outline[i].text.size() + 64 >
		    kMaxToolResultBytes) {
			truncated = true;
			break;
		}
		acl::json_node &item = items.add_child(false, true);
		item.add_number(
		    "line", static_cast<long long>(outline[i].line));
		item.add_text("kind", outline[i].kind.c_str());
		item.add_text("text", outline[i].text.c_str());
		estimated +=
		    outline[i].kind.size() + outline[i].text.size() + 64;
	}
	context.trace.truncated = truncated;
	return finish_workspace_tool_result(context);
}

} // namespace workspace_tool_detail

}
}
