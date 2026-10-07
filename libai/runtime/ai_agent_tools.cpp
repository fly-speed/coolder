#include "stdafx.h"
#include "ai_agent_tools_internal.h"
#include "browser_debug.h"

namespace action { namespace agent_detail {

namespace workspace_tool_detail {

std::string finish_workspace_tool_result(workspace_tool_context_t& context)
{
	context.root.add_bool("truncated", context.trace.truncated);
	context.trace.ok = true;
	std::string result = serialize_json(context.root);
	// JSON escaping can expand each content byte to six bytes.
	const size_t result_limit = context.request.name == "workspace.read" || context.request.name == "workspace.read_batch"
		? context.read_batch_bytes * 6 + 16384 : kMaxToolResultBytes * 2;
	if (result.size() > result_limit) {
		context.trace.ok = false;
		context.trace.truncated = true;
		return tool_error_json("tool result exceeds the context limit");
	}
	return result;
}

std::string workspace_read_path_to_project(const std::string& project_path,
	bool live_source_view, const std::string& value)
{
	return live_source_view ? value : draft_path_to_project_path(project_path, value);
}

} // namespace workspace_tool_detail

using namespace workspace_tool_detail;

std::string execute_workspace_tool(webcool::ai::agent_workspace_t& workspace,
	const std::string& user_root, const std::string& project_path,
	bool allow_file_content,
	const agent_tool_request_t& request, agent_tool_trace_t& trace,
	std::vector<agent_change_proposal_t>* staged_changes,
	const std::string& run_id,
	const webcool::ai::sandbox_limits_t& sandbox_limits, bool chinese,
	const unavailable_validation_t* unavailable_validation,
	validation_cache_t* validation_cache,
	std::string* saved_proposal_batch,
    batch_validation_evidence_t* batch_validation)
{
	trace.name = request.name;
	trace.path = request.path;
	trace.query = request.query;
	trace.ok = false;
	trace.truncated = false;
	trace.native = false;
	const webcool::ai::ai_admin_policy_t read_policy = webcool::ai::ai_runtime_policy_get();
	const size_t read_chunk_bytes = read_policy.read_chunk_kib * 1024;
	const size_t read_file_limit_bytes = read_policy.read_file_limit_kib * 1024;
	const size_t read_batch_bytes = std::max(kMaxToolResultBytes, std::min<size_t>(64 * 1024, read_chunk_bytes * 8));
	if (request.name == "workspace.execute_batch") {
		return execute_workspace_batch(workspace, user_root, project_path, allow_file_content, request, trace, staged_changes, run_id, sandbox_limits, chinese, unavailable_validation, validation_cache, saved_proposal_batch, batch_validation);
	}
	const webcool::ai::agent_definition_t* definition =
		webcool::ai::agent_registry_t::instance().find("coding");
	const webcool::ai::agent_tool_t* registered_tool = definition == NULL ? NULL
		: webcool::ai::agent_registry_t::instance().find_tool(*definition,
			request.name);
	if (registered_tool == NULL || !registered_tool->model_enabled) {
		return tool_error_json("tool is not enabled for this agent run");
	}
    if (request.name.compare(0,8,"browser.")==0) {
        if (!allow_file_content) return tool_error_json("Provider is not authorized to receive page content");
        const auto result=webcool::ai::browser_debug_tool(user_root,project_path,run_id,request.name,request.query,request.content);
        acl::json evidence(result.c_str()); const auto* ok=evidence["ok"];
        trace.ok=ok && ok->get_bool() && *ok->get_bool();
        return result;
    }
	std::string raw_path = request.path;
	if (raw_path.empty() && (request.name == "workspace.list"
		|| request.name == "workspace.search" || request.name == "code.symbols"
		|| request.name == "code.references" || request.name == "workspace.read_batch"
		|| request.name == "workspace.propose_batch"
		|| request.name == "workspace.validate")) {
		raw_path = project_path;
	}
	std::string path;
	std::string err;
	if (!resolve_project_tool_path(raw_path, project_path,
		request.name == "workspace.list" || request.name == "workspace.search"
			|| request.name == "code.symbols" || request.name == "code.references"
			|| request.name == "workspace.read_batch"
			|| request.name == "workspace.propose_batch"
			|| request.name == "workspace.validate",
		path, err)) return tool_error_json(err);
	trace.path = path;
	if (!allow_file_content && registered_tool->requires_file_content) {
		return tool_error_json(
			"this AI provider is not allowed to receive file content");
	}
    // Until the first edit/build, read current formal files through the same
    // authorized workspace API. Creating a full dependency copy for inspection
    // alone is wasteful. Once a draft exists, every read uses that draft.
    const std::string draft_root = persistent_draft_root(user_root, project_path, run_id);
    struct stat draft_stat;
    const bool missing_draft = lstat(draft_root.c_str(), &draft_stat) != 0 && errno == ENOENT;
    const bool live_source_view = missing_draft && (staged_changes == NULL || staged_changes->empty());
    webcool::ai::agent_workspace_t draft_workspace(draft_root);
    if (!missing_draft) {
        std::map<std::string, std::string> mounts;
        if (!webcool::ai::agent_draft_store_t(user_root, project_path, run_id).readonly_dependencies(mounts, err)) return tool_error_json(err);
        draft_workspace.set_readonly_mounts(mounts);
    }
    webcool::ai::agent_workspace_t& read_workspace = live_source_view ? workspace : draft_workspace;
    std::string draft_path;
    if (!project_path_to_draft_path(project_path, path, draft_path))
        return tool_error_json("tool path cannot be mapped to the private draft");
    if (live_source_view) draft_path = path;
	// Coding changes must remain proposals until the user accepts the review.
	// Keep the legacy create/mkdir implementations unreachable from model output;
	// the private materialized draft and live review overlay are reached only
	// through the proposal tools below and never touch formal source.
	if (request.name == "workspace.create" || request.name == "workspace.mkdir") {
		return tool_error_json(
			"mutating workspace tools require review; return this operation in final changes");
	}

	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	workspace_tool_context_t context = {
		request,
		trace,
		read_batch_bytes,
		root,
		workspace,
		user_root,
		project_path,
		staged_changes,
		run_id,
		sandbox_limits,
		chinese,
		unavailable_validation,
		validation_cache,
		draft_workspace,
		read_workspace,
		path,
		draft_path,
		err,
		live_source_view,
		json,
		read_chunk_bytes,
		read_file_limit_bytes,
		saved_proposal_batch
	};
	if (request.name == "workspace.validate") {
		return execute_workspace_validate(context);
	}
	if (request.name == "workspace.list") {
		return execute_workspace_list(context);
	}
	if (request.name == "workspace.read_batch") {
		return execute_workspace_read_batch(context);
	}
	if (request.name == "workspace.read") {
		return execute_workspace_read(context);
	}
	if (request.name == "workspace.search" || request.name == "code.references") {
		return execute_workspace_search(context);
	}
	if (request.name == "code.symbols") {
		return execute_workspace_symbols(context);
	}
	if (request.name == "workspace.outline") {
		return execute_workspace_outline(context);
	}
	if (request.name == "workspace.propose_batch") {
		return execute_workspace_propose_batch(context);
	}
	if (request.name == "workspace.propose") {
		return execute_workspace_propose(context);
	}
	if (request.name == "workspace.patch_set") {
		return execute_workspace_patch_set(context);
	}
	if (request.name == "workspace.replace") {
		return execute_workspace_replace(context);
	}
	if (request.name == "workspace.propose_delete"
		|| request.name == "workspace.propose_move" || request.name == "workspace.propose_mkdir") {
		return execute_workspace_propose_operation(context);
	}
	if (request.name == "workspace.mkdir") {
		return execute_workspace_mkdir(context);
	}
	if (request.name == "workspace.create") {
		return execute_workspace_create(context);
	}
	return tool_error_json("tool is not enabled for this agent run");
}

} }
