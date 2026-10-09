#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action
{
namespace agent_detail
{

namespace workspace_tool_detail
{

static std::string parse_workspace_batch(const agent_tool_request_t &request,
    std::vector<agent_tool_request_t> &requests, bool &reads_only)
{
	acl::json batch_json(request.content.c_str());
	acl::json_node *items = batch_json.finish() ?
	    json_array_node(&batch_json.get_root()) :
	    NULL;
	if (items == NULL)
		return "provider tool batch must be a JSON array";
	reads_only = true;
	for (acl::json_node *item = items->first_child(); item != NULL;
	     item = items->next_child()) {
		if (requests.size() >= 16)
			return "provider tool batch contains more than 16 calls";
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL)
			return "provider tool batch item must be an object";
		agent_tool_request_t child;
		child.name = json_text((*object)["name"]);
		child.path = json_text((*object)["path"]);
		child.query = json_text((*object)["query"]);
		child.old_text = json_text((*object)["old_text"]);
		child.target_path = json_text((*object)["target_path"]);
		child.content_present = (*object)["content"] != NULL &&
		    (*object)["content"]->is_string();
		child.content = child.content_present ?
		    json_text((*object)["content"]) :
		    "";
		if (child.name.empty() ||
		    child.name == "workspace.execute_batch") {
			return "provider tool batch contains an invalid call";
		}
		if (child.name.compare(0, 8, "browser.") == 0)
			return "Browser tools must be called one at a time, outside workspace batches; observe each result before the next action";
		reads_only = reads_only && is_parallel_read_tool(child.name);
		requests.push_back(child);
	}
	if (!requests.empty())
		return "";
	return "provider tool batch requires at least one call";
}

static std::string serialize_workspace_batch(
    const std::vector<agent_tool_request_t> &requests,
    const std::vector<agent_tool_trace_t> &child_traces,
    const std::vector<std::string> &child_results, bool reads_only,
    const std::string &project_path, agent_tool_trace_t &trace)
{
	acl::json response_json;
	acl::json_node &response = response_json.create_node();
	bool all_ok = true;
	for (size_t i = 0; i < child_traces.size(); ++i) {
		all_ok = all_ok && child_traces[i].ok;
	}
	response.add_bool("ok", all_ok);
	response.add_bool("parallel", reads_only);
	acl::json_node &results = response_json.create_array();
	response.add_child("results", results);
	size_t result_bytes = 0;
	for (size_t i = 0; i < requests.size(); ++i) {
		acl::json_node &item = results.add_child(false, true);
		item.add_text("name", requests[i].name.c_str());
		item.add_text("path", child_traces[i].path.c_str());
		item.add_bool("ok", child_traces[i].ok);
		std::string value = child_results[i];
		// Each read result is already bounded by its configured page limit.
		// Preserve its complete JSON, including next_query, in native batches.
		const bool paged_read = requests[i].name == "workspace.read" ||
		    requests[i].name == "workspace.read_batch";
		const size_t remaining = result_bytes < kMaxToolResultBytes ?
		    kMaxToolResultBytes - result_bytes :
		    0;
		if (!paged_read && value.size() > remaining) {
			value = webcool::ai::utf8_prefix(value, remaining);
			trace.truncated = true;
		}
		item.add_text("result", value.c_str());
		result_bytes += value.size();
	}
	trace.path = project_path;
	// A parallel batch may return useful results for successful siblings, but
	// the batch itself is successful only when every requested observation was
	// obtained. This keeps retry/no-progress accounting honest.
	trace.ok = all_ok;
	return serialize_json(response);
}

static void cleanup_failed_provider_batch(const std::string &user_root,
    const std::string &project_path, const std::string &batch_run_id)
{
	std::string cleanup_err;
	if (!remove_persistent_worktree(
	        user_root, project_path, batch_run_id, cleanup_err)) {
		webcool::ai::ai_log_error("agent.runtime",
		    "cleanup-failed-provider-batch", cleanup_err);
	}
}

static void record_batch_validation(batch_validation_evidence_t &verified,
    const std::string &result, const std::string &baseline_before,
    const std::vector<agent_change_proposal_t> &candidate)
{
	verified = batch_validation_evidence_t();
	acl::json report(result.c_str());
	if (!baseline_before.empty() && report.finish() &&
	    json_bool(report["validation_passed"], false) &&
	    json_number(report["commands_executed"], 0) > 0) {
		verified.report = result;
		verified.draft = staged_cycle_fingerprint(candidate);
		verified.baseline = baseline_before;
	}
}

std::string execute_workspace_batch(webcool::ai::agent_workspace_t &workspace,
    const std::string &user_root, const std::string &project_path,
    bool allow_file_content, const agent_tool_request_t &request,
    agent_tool_trace_t &trace,
    std::vector<agent_change_proposal_t> *staged_changes,
    const std::string &run_id,
    const webcool::ai::sandbox_limits_t &sandbox_limits, bool chinese,
    const unavailable_validation_t *unavailable_validation,
    validation_cache_t *validation_cache, std::string *saved_proposal_batch,
    batch_validation_evidence_t *batch_validation)
{
	// This internal request is synthesized only after the provider adapter has
	// authenticated and decoded a multi-call assistant turn. It is not exposed
	// in the model registry. Independent read calls run concurrently in bounded
	// waves; any mutation batch is applied in wire order to a private candidate
	// vector and committed to the caller only when every operation succeeds.
	std::vector<agent_tool_request_t> requests;
	bool reads_only = true;
	const std::string parse_error =
	    parse_workspace_batch(request, requests, reads_only);
	if (!parse_error.empty())
		return tool_error_json(parse_error);

	std::vector<agent_tool_trace_t> child_traces(requests.size());
	std::vector<std::string> child_results(requests.size());
	auto execute_child = [=](const agent_tool_request_t child)
	    -> std::pair<agent_tool_trace_t, std::string> {
		webcool::ai::agent_workspace_t child_workspace(user_root);
		agent_tool_trace_t child_trace;
		const std::string result = execute_workspace_tool(
		    child_workspace, user_root, project_path,
		    allow_file_content, child, child_trace, staged_changes,
		    run_id, sandbox_limits, chinese, unavailable_validation,
		    validation_cache, saved_proposal_batch);
		return std::make_pair(child_trace, result);
	};
	if (reads_only) {
		const size_t concurrency = 4;
		for (size_t begin = 0; begin < requests.size();
		     begin += concurrency) {
			const size_t end =
			    std::min(requests.size(), begin + concurrency);
			std::vector<std::future<
			    std::pair<agent_tool_trace_t, std::string>>>
			    futures;
			for (size_t i = begin; i < end; ++i) {
				const agent_tool_request_t child = requests[i];
				futures.push_back(std::async(
				    std::launch::async, execute_child, child));
			}
			for (size_t i = begin; i < end; ++i) {
				const std::pair<agent_tool_trace_t, std::string>
				    value = futures[i - begin].get();
				child_traces[i] = value.first;
				child_results[i] = value.second;
			}
		}
	} else {
		if (staged_changes == NULL)
			return tool_error_json(
			    "provider mutation batch has no review storage");
		std::vector<agent_change_proposal_t> candidate =
		    *staged_changes;
		const std::string batch_run_id = new_run_id();
		if (batch_run_id.empty())
			return tool_error_json(
			    "cannot allocate private provider batch identity");
		size_t skipped_files = 0;
		std::string batch_err;
		if (!materialize_staged_worktree(user_root, project_path,
		        batch_run_id, candidate, skipped_files, batch_err)) {
			webcool::ai::ai_log_error("agent.runtime",
			    "initialize-provider-batch", batch_err);
			return tool_error_json(batch_err);
		}
		batch_validation_evidence_t verified;
		for (size_t i = 0; i < requests.size(); ++i) {
			const bool validating =
			    requests[i].name == "workspace.validate";
			const std::string baseline_before = validating ?
			    validation_baseline_fingerprint(
			        workspace, project_path) :
			    "";
			child_results[i] =
			    execute_workspace_tool(workspace, user_root,
			        project_path, allow_file_content, requests[i],
			        child_traces[i], &candidate, batch_run_id,
			        sandbox_limits, chinese, unavailable_validation,
			        validation_cache, saved_proposal_batch);
			if (!child_traces[i].ok) {
				cleanup_failed_provider_batch(
				    user_root, project_path, batch_run_id);
				acl::json failure_json;
				acl::json_node &failure =
				    failure_json.create_node();
				failure.add_bool("ok", false);
				failure.add_bool("rolled_back", true);
				failure.add_number("failed_call",
				    static_cast<long long>(i + 1));
				failure.add_text("error",
				    ("provider mutation batch failed at call " +
				        std::to_string(i + 1) + ": " +
				        child_results[i])
				        .c_str());
				failure.add_text(
				    "cause", child_results[i].c_str());
				return serialize_json(failure);
			}
			if (validating) {
				record_batch_validation(verified,
				    child_results[i], baseline_before,
				    candidate);
			}
			// Publish this candidate only to the batch-private tree so the next
			// operation observes every earlier edit in the same assistant turn.
			if (materialize_staged_worktree(user_root, project_path,
			        batch_run_id, candidate, skipped_files,
			        batch_err))
				continue;
			webcool::ai::ai_log_error("agent.runtime",
			    "advance-provider-batch", batch_err);
			std::string cleanup_err;
			if (remove_persistent_worktree(user_root, project_path,
			        batch_run_id, cleanup_err))
				return tool_error_json(batch_err);
			webcool::ai::ai_log_error("agent.runtime",
			    "cleanup-invalid-provider-batch", cleanup_err);

			return tool_error_json(batch_err);
		}
		if (batch_validation && !verified.report.empty() &&
		    verified.draft == staged_cycle_fingerprint(candidate) &&
		    verified.baseline ==
		        validation_baseline_fingerprint(
		            workspace, project_path)) {
			*batch_validation = verified;
		}
		staged_changes->swap(candidate);
		std::string cleanup_err;
		if (!remove_persistent_worktree(
		        user_root, project_path, batch_run_id, cleanup_err)) {
			// The accepted candidate remains valid. A stale private worktree is
			// harmless but must be logged for administrator cleanup.
			webcool::ai::ai_log_error("agent.runtime",
			    "cleanup-provider-batch", cleanup_err);
		}
	}

	return serialize_workspace_batch(requests, child_traces, child_results,
	    reads_only, project_path, trace);
}

} // namespace workspace_tool_detail

}
}
