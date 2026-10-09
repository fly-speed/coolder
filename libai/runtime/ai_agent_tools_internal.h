#pragma once
#include "coding_runtime.h"

namespace action
{
namespace agent_detail
{
std::string edit_rebase_error(const std::string &error, const std::string &path,
			      const std::string &source, bool chinese);

std::string
proposal_error_json(const std::vector<proposal_validation_error_t> &failures,
		    bool chinese);

bool is_parallel_read_tool(const std::string &name);

std::string draft_path_to_project_path(const std::string &project_path,
				       const std::string &draft_path);

void append_edit_snapshot(acl::json &json, acl::json_node &root,
			  const std::string &source, size_t at, bool chinese);

bool path_has_pending_proposal(
	const std::vector<agent_change_proposal_t> *staged_changes,
	const std::string &path);

agent_tool_request_t
completion_tool_request(const webcool::ai::completion_tool_call_t &call);

std::string workspace_inspection_error(const std::string &err,
				       const std::string &path, bool chinese);

bool remember_read_page(acl::json_node &page,
			webcool::ai::agent_read_context_t &context);

namespace workspace_tool_detail
{

// Shared state for a single authorized tool call. References remain valid until
// the selected handler returns; batch calls prepare their own state recursively.
struct workspace_tool_context_t {
	const agent_tool_request_t &request;
	agent_tool_trace_t &trace;
	size_t read_batch_bytes;
	acl::json_node &root;
	webcool::ai::agent_workspace_t &workspace;
	const std::string &user_root;
	const std::string &project_path;
	std::vector<agent_change_proposal_t> *staged_changes;
	const std::string &run_id;
	const webcool::ai::sandbox_limits_t &sandbox_limits;
	bool chinese;
	const unavailable_validation_t *unavailable_validation;
	validation_cache_t *validation_cache;
	webcool::ai::agent_workspace_t &draft_workspace;
	webcool::ai::agent_workspace_t &read_workspace;
	const std::string &path;
	const std::string &draft_path;
	std::string &err;
	bool live_source_view;
	acl::json &json;
	size_t read_chunk_bytes;
	size_t read_file_limit_bytes;
	std::string *saved_proposal_batch;
};

std::string finish_workspace_tool_result(workspace_tool_context_t &context);

std::string workspace_read_path_to_project(const std::string &project_path,
					   bool live_source_view,
					   const std::string &value);

std::string execute_workspace_batch(
	webcool::ai::agent_workspace_t &workspace, const std::string &user_root,
	const std::string &project_path, bool allow_file_content,
	const agent_tool_request_t &request, agent_tool_trace_t &trace,
	std::vector<agent_change_proposal_t> *staged_changes,
	const std::string &run_id,
	const webcool::ai::sandbox_limits_t &sandbox_limits, bool chinese,
	const unavailable_validation_t *unavailable_validation,
	validation_cache_t *validation_cache, std::string *saved_proposal_batch,
	batch_validation_evidence_t *batch_validation);

std::string execute_workspace_validate(workspace_tool_context_t &context);

std::string execute_workspace_list(workspace_tool_context_t &context);

std::string execute_workspace_read_batch(workspace_tool_context_t &context);

std::string execute_workspace_read(workspace_tool_context_t &context);

std::string execute_workspace_search(workspace_tool_context_t &context);

std::string execute_workspace_symbols(workspace_tool_context_t &context);

std::string execute_workspace_outline(workspace_tool_context_t &context);

std::string execute_workspace_propose_batch(workspace_tool_context_t &context);

std::string execute_workspace_propose(workspace_tool_context_t &context);

std::string execute_workspace_patch_set(workspace_tool_context_t &context);

std::string execute_workspace_replace(workspace_tool_context_t &context);

std::string
execute_workspace_propose_operation(workspace_tool_context_t &context);

std::string execute_workspace_mkdir(workspace_tool_context_t &context);

std::string execute_workspace_create(workspace_tool_context_t &context);

}
}
}
