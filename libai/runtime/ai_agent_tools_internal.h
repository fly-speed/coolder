#pragma once
#include "coding_runtime.h"

namespace action
{
namespace agent_detail
{
// Describe why an edit no longer matches the source version it was based on.
std::string edit_rebase_error(const std::string &error, const std::string &path,
    const std::string &source, bool chinese);

// Serialize proposal-validation failures for model and browser feedback.
std::string proposal_error_json(
    const std::vector<proposal_validation_error_t> &failures, bool chinese);

// Identify read-only tools eligible for parallel batch execution.
bool is_parallel_read_tool(const std::string &name);

// Translate a draft-relative path back to the selected project.
std::string draft_path_to_project_path(
    const std::string &project_path, const std::string &draft_path);

// Attach a bounded source excerpt around the edit offset to JSON feedback.
void append_edit_snapshot(acl::json &json, acl::json_node &root,
    const std::string &source, size_t at, bool chinese);

// Check whether the path already has an unresolved staged change.
bool path_has_pending_proposal(
    const std::vector<agent_change_proposal_t> *staged_changes,
    const std::string &path);

// Convert a provider-native call into the runtime's normalized tool request.
agent_tool_request_t completion_tool_request(
    const webcool::ai::completion_tool_call_t &call);

// Classify failures encountered while inspecting proposal targets.
std::string workspace_inspection_error(
    const std::string &err, const std::string &path, bool chinese);

// Retain one source page together with its version and offset metadata.
bool remember_read_page(
    acl::json_node &page, webcool::ai::agent_read_context_t &context);

namespace workspace_tool_detail
{

// Shared state for a single authorized tool call. References remain valid until
// the selected handler returns; batch calls prepare their own state recursively.
struct workspace_tool_context_t {
	// Normalized workspace tool request being handled.
	const agent_tool_request_t &request;
	// Metadata describing the current tool execution.
	agent_tool_trace_t &trace;
	// Maximum aggregate source bytes returned by one read batch.
	size_t read_batch_bytes;
	// Response root node owned by the accompanying JSON document.
	acl::json_node &root;
	// Authorized workspace used by this operation.
	webcool::ai::agent_workspace_t &workspace;
	// Filesystem root belonging to the authenticated user.
	const std::string &user_root;
	// Logical path identifying the selected project.
	const std::string &project_path;
	// Unapplied file proposals retained for review and recovery.
	std::vector<agent_change_proposal_t> *staged_changes;
	// Identifier of the associated agent run.
	const std::string &run_id;
	// Resource limits applied to external command execution.
	const webcool::ai::sandbox_limits_t &sandbox_limits;
	// Whether user-facing output selects the Chinese prompt variant.
	bool chinese;
	// Tracks checks that cannot run in the current environment.
	const unavailable_validation_t *unavailable_validation;
	// Cached build/test evidence tied to the exact staged revision.
	validation_cache_t *validation_cache;
	// Private workspace containing the staged source revision.
	webcool::ai::agent_workspace_t &draft_workspace;
	// Workspace view used when resolving read-only tool requests.
	webcool::ai::agent_workspace_t &read_workspace;
	// Path of the file or resource associated with this record.
	const std::string &path;
	// Logical path mapped into the private draft workspace.
	const std::string &draft_path;
	// Caller-owned diagnostic populated when the operation fails.
	std::string &err;
	// Whether reads use the current staged source view.
	bool live_source_view;
	// JSON document owning nodes used to build the tool response.
	acl::json &json;
	// Maximum source bytes returned by one ordinary page read.
	size_t read_chunk_bytes;
	// Maximum source file size accepted by the read tools.
	size_t read_file_limit_bytes;
	// Bounded proposal payload retained for follow-up batch operations.
	std::string *saved_proposal_batch;
};

// Finalize the serialized tool response and its execution trace.
std::string finish_workspace_tool_result(workspace_tool_context_t &context);

// Resolve a read result's path against its project context.
std::string workspace_read_path_to_project(const std::string &project_path,
    bool live_source_view, const std::string &value);

// Execute the workspace batch tool against its authorized context.
std::string execute_workspace_batch(webcool::ai::agent_workspace_t &workspace,
    const std::string &user_root, const std::string &project_path,
    bool allow_file_content, const agent_tool_request_t &request,
    agent_tool_trace_t &trace,
    std::vector<agent_change_proposal_t> *staged_changes,
    const std::string &run_id,
    const webcool::ai::sandbox_limits_t &sandbox_limits, bool chinese,
    const unavailable_validation_t *unavailable_validation,
    validation_cache_t *validation_cache, std::string *saved_proposal_batch,
    batch_validation_evidence_t *batch_validation);

// Execute the workspace validate tool against its authorized context.
std::string execute_workspace_validate(workspace_tool_context_t &context);

// Execute the workspace list tool against its authorized context.
std::string execute_workspace_list(workspace_tool_context_t &context);

// Execute the workspace read batch tool against its authorized context.
std::string execute_workspace_read_batch(workspace_tool_context_t &context);

// Execute the workspace read tool against its authorized context.
std::string execute_workspace_read(workspace_tool_context_t &context);

// Execute the workspace search tool against its authorized context.
std::string execute_workspace_search(workspace_tool_context_t &context);

// Execute the workspace symbols tool against its authorized context.
std::string execute_workspace_symbols(workspace_tool_context_t &context);

// Execute the workspace outline tool against its authorized context.
std::string execute_workspace_outline(workspace_tool_context_t &context);

// Execute the workspace propose batch tool against its authorized context.
std::string execute_workspace_propose_batch(workspace_tool_context_t &context);

// Execute the workspace propose tool against its authorized context.
std::string execute_workspace_propose(workspace_tool_context_t &context);

// Execute the workspace patch set tool against its authorized context.
std::string execute_workspace_patch_set(workspace_tool_context_t &context);

// Execute the workspace replace tool against its authorized context.
std::string execute_workspace_replace(workspace_tool_context_t &context);

// Execute the workspace propose operation tool against its authorized
// context.
std::string execute_workspace_propose_operation(
    workspace_tool_context_t &context);

// Execute the workspace mkdir tool against its authorized context.
std::string execute_workspace_mkdir(workspace_tool_context_t &context);

// Execute the workspace create tool against its authorized context.
std::string execute_workspace_create(workspace_tool_context_t &context);

}
}
}
