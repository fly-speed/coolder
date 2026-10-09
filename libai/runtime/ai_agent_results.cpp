#include "stdafx.h"
#include "coding_runtime.h"
namespace action
{
namespace agent_detail
{
void attach_change_preview_diffs(std::vector<agent_change_proposal_t> &changes,
    const webcool::ai::workspace_change_set_preview_t &preview)
{
	// Transaction preview sorts directory creation before child writes. Match by
	// validated operation/path identity rather than position so every generated
	// file retains the correct baseline and line diff.
	for (size_t i = 0; i < changes.size(); ++i) {
		for (size_t j = 0; j < preview.items.size(); ++j) {
			if (changes[i].operation !=
			        preview.items[j].operation ||
			    changes[i].path != preview.items[j].path ||
			    changes[i].target_path !=
			        preview.items[j].target_path)
				continue;
			changes[i].added_lines = preview.items[j].added_lines;
			changes[i].removed_lines =
			    preview.items[j].removed_lines;
			changes[i].diff = preview.items[j].diff;
			changes[i].original_content =
			    preview.items[j].original_content;
			changes[i].original_content_available = true;
			break;
		}
	}
}

void add_run_record_json(
    acl::json_node &item, const webcool::ai::agent_run_record_t &record)
{
	item.add_text("run_id", record.id.c_str());
	item.add_text("agent_id", record.agent_id.c_str());
	item.add_text("agent_version", record.agent_version.c_str());
	item.add_text("status", record.status.c_str());
	item.add_text("provider_id", record.provider_id.c_str());
	item.add_text("model", record.model.c_str());
	item.add_text("path", record.project_path.c_str());
	item.add_number("started_at", record.started_at);
	item.add_number("finished_at", record.finished_at);
	item.add_number("input_tokens", record.input_tokens);
	item.add_number("cached_input_tokens", record.cached_input_tokens);
	item.add_number("output_tokens", record.output_tokens);
	item.add_number("reasoning_tokens", record.reasoning_tokens);
	item.add_number("latency_ms", record.latency_ms);
	item.add_number("tool_calls", record.tool_calls);
	item.add_number("proposed_changes", record.proposed_changes);
	item.add_number("rejected_changes", record.rejected_changes);
	if (!record.provider_error_category.empty()) {
		item.add_text("provider_error_category",
		    record.provider_error_category.c_str());
	}
	if (record.provider_http_status > 0) {
		item.add_number(
		    "provider_http_status", record.provider_http_status);
	}
	item.add_bool(
	    "provider_error_retryable", record.provider_error_retryable);
	if (!record.error.empty())
		item.add_text("error", record.error.c_str());
}

void add_runtime_result_json(acl::json_node &root,
    const std::shared_ptr<agent_runtime_task_t> &task,
    const webcool::ai::agent_result_t *persisted, bool include_changes)
{
	if (!task) {
		root.add_bool("result_available", false);
		return;
	}
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	root.add_bool("cancel_requested", task->cancel_requested);
	root.add_bool("pause_requested", task->pause_requested);
	root.add_bool("paused", task->phase == "paused");
	root.add_bool(
	    "result_available", task->done && task->status == "completed");
	root.add_text("phase", task->phase.c_str());
	root.add_bool(
	    "provider_response_pending", task->provider_response_pending);
	if (!task->provider_response_id.empty()) {
		root.add_text(
		    "provider_response_id", task->provider_response_id.c_str());
	}
	root.add_number(
	    "event_version", static_cast<long long>(task->event_version));
	root.add_number("completed_tool_calls",
	    static_cast<long long>(task->completed_tool_calls));
	root.add_number("workspace_mutation_version",
	    static_cast<long long>(task->workspace_mutation_version));
	root.add_number("staged_change_version",
	    static_cast<long long>(task->staged_change_version));
	if (!task->last_workspace_mutation_path.empty()) {
		root.add_text("last_workspace_mutation_path",
		    task->last_workspace_mutation_path.c_str());
	}
	if (!task->session_id.empty()) {
		root.add_text("session_id", task->session_id.c_str());
	}
	root.add_text("task_contract_json",
	    persisted ? persisted->task_contract_json.c_str() :
	                task->task_contract_json.c_str());
	root.add_text("task_acceptance_json",
	    persisted ? persisted->task_acceptance_json.c_str() :
	                task->task_acceptance_json.c_str());
	root.add_text("task_acceptance_status",
	    persisted ? persisted->task_acceptance_status.c_str() :
	                task->task_acceptance_status.c_str());
	root.add_bool(
	    "restart_recovery_enabled", task->restart_recovery_enabled);
	root.add_bool("resumed_after_restart", task->resumed_after_restart);
	root.add_bool("resumed_from_progress", task->resumed_from_progress);
	root.add_bool("recovery_available", task->recovery_available);
	root.add_bool("browser_debug_confirmation_required",
	    task->browser_debug_confirmation_required);
	root.add_number("browser_debug_retry_count",
	    static_cast<long long>(task->browser_debug_retry_count));
	root.add_bool("result_persisted", !task->result_file.empty());
	root.add_bool("changes_applied",
	    persisted ? persisted->changes_applied : task->changes_applied);
	if (!task->changes_apply_error.empty()) {
		root.add_text(
		    "changes_apply_error", task->changes_apply_error.c_str());
	}
	if (!task->result_file.empty()) {
		root.add_text("result_file", task->result_file.c_str());
		root.add_text("result_decision",
		    persisted ? persisted->decision.c_str() :
		        task->result_decision.empty() ?
		                "pending" :
		                task->result_decision.c_str());
	}
	if (!task->progress_file.empty()) {
		root.add_text("progress_file", task->progress_file.c_str());
	}
	root.add_text("operation_log_file", operation_log_path(task).c_str());
	root.add_number("input_tokens", task->output.input_tokens);
	root.add_number(
	    "cached_input_tokens", task->output.cached_input_tokens);
	root.add_number("output_tokens", task->output.output_tokens);
	root.add_number("reasoning_tokens", task->output.reasoning_tokens);
	root.add_number("latency_ms", task->output.latency_ms);
	root.add_text("provider_error_category",
	    webcool::ai::provider_client_t::error_category_name(
	        task->output.error_category));
	root.add_bool("provider_error_retryable", task->output.retryable_error);
	if (task->output.http_status > 0) {
		root.add_number(
		    "provider_http_status", task->output.http_status);
	}
	if (!task->current_tool.empty()) {
		root.add_text("current_tool", task->current_tool.c_str());
	}
	acl::json_node &stream_progress =
	    root.add_child("stream_progress", true);
	stream_progress.add_text("phase", task->stream_progress.phase.c_str());
	stream_progress.add_number(
	    "elapsed_ms", task->stream_progress.elapsed_ms);
	stream_progress.add_number(
	    "first_byte_ms", task->stream_progress.first_byte_ms);
	stream_progress.add_number(
	    "last_effective_ms", task->stream_progress.last_effective_ms);
	stream_progress.add_number("received_bytes",
	    static_cast<long long>(task->stream_progress.received_bytes));
	stream_progress.add_number("tool_argument_bytes",
	    static_cast<long long>(task->stream_progress.tool_argument_bytes));
	if (!task->streamed_text.empty()) {
		root.add_text("streamed_text", task->streamed_text.c_str());
	}
	if (!task->streamed_reasoning.empty() &&
	    (!task->done || task->status != "completed" ||
	        task->output.reasoning.empty())) {
		root.add_text(
		    "streamed_reasoning", task->streamed_reasoning.c_str());
	}
	if (!task->completion_summary.empty()) {
		root.add_text(
		    "completion_summary", task->completion_summary.c_str());
	}
	// Incremental proposals are safe to expose while the model is still running:
	// they are review data only and have not changed the formal project tree.
	if (include_changes) {
		const auto &visible_changes =
		    persisted ? persisted->changes : task->changes;
		acl::json_node &changes = root.get_json().create_array();
		root.add_child("changes", changes);
		for (size_t i = 0; i < visible_changes.size(); ++i) {
			acl::json_node &item = changes.add_child(false, true);
			item.add_text(
			    "operation", visible_changes[i].operation.c_str());
			item.add_text("path", visible_changes[i].path.c_str());
			if (!visible_changes[i].target_path.empty()) {
				item.add_text("target_path",
				    visible_changes[i].target_path.c_str());
			}
			item.add_text(
			    "content", visible_changes[i].content.c_str());
			item.add_text(
			    "reason", visible_changes[i].reason.c_str());
			item.add_bool(
			    "creates_file", visible_changes[i].creates_file);
			item.add_bool("creates_directory",
			    visible_changes[i].creates_directory);
			item.add_number(
			    "added_lines", visible_changes[i].added_lines);
			item.add_number(
			    "removed_lines", visible_changes[i].removed_lines);
			if (!visible_changes[i].diff.empty()) {
				item.add_text(
				    "diff", visible_changes[i].diff.c_str());
			}
			item.add_text("original_content",
			    visible_changes[i].original_content.c_str());
			item.add_bool("original_content_available",
			    visible_changes[i].original_content_available);
			item.add_number("generation",
			    static_cast<long long>(
			        visible_changes[i].generation));
			item.add_text(
			    "base_hash", visible_changes[i].base_hash.c_str());
			item.add_text("draft_hash",
			    visible_changes[i].draft_hash.c_str());
			item.add_text("review_status",
			    visible_changes[i].review_status.c_str());
		}
	}
	if (!task->done || task->status != "completed")
		return;
	root.add_text("text", task->output.text.c_str());
	if (!task->output.reasoning.empty()) {
		root.add_text("reasoning", task->output.reasoning.c_str());
	} else if (!task->streamed_reasoning.empty()) {
		root.add_text("reasoning", task->streamed_reasoning.c_str());
	}
	acl::json_node &tool_calls = root.get_json().create_array();
	root.add_child("tool_calls", tool_calls);
	for (size_t i = 0; i < task->traces.size(); ++i) {
		acl::json_node &item = tool_calls.add_child(false, true);
		item.add_text("name", task->traces[i].name.c_str());
		item.add_text("path", task->traces[i].path.c_str());
		if (!task->traces[i].query.empty()) {
			item.add_text("query", task->traces[i].query.c_str());
		}
		item.add_bool("ok", task->traces[i].ok);
		item.add_bool("truncated", task->traces[i].truncated);
		item.add_bool("native", task->traces[i].native);
	}
}

// A normal runtime task owns its recovery metadata in memory.  After a service
// restart, however, an older browser can still be polling a run that no longer
// has a runtime task.  Associate that run with its durable conversation before
// replying, so the browser can offer the project-level "resume from progress"
// action instead of presenting a restart as an unrecoverable provider failure.
static bool add_hinted_recovery(acl::json_node &root,
    const std::string &user_root, const webcool::ai::agent_run_record_t &record,
    const std::string &session_hint,
    webcool::ai::agent_session_store_t &session_store, std::string &err)
{
	if (session_hint.empty())
		return false;

	webcool::ai::agent_session_record_t hinted;
	if (session_store.get(session_hint, hinted, err) &&
	    hinted.project_path == record.project_path) {
		bool found = false;
		webcool::ai::agent_progress_store_t progress_store(
		    user_root, hinted.project_path, hinted.id);
		if (progress_store.exists(found, err)) {
			root.add_bool("recovery_available", found);
			root.add_text("session_id", hinted.id.c_str());
			if (!found)
				return true;
			root.add_text("progress_file",
			    progress_store.relative_path().c_str());
			return true;
		}
		webcool::ai::ai_log_error(
		    "agent.run", "probe-hinted-progress", err);
	}
	return false;
}

void add_durable_recovery_json(acl::json_node &root,
    const std::string &user_root, const webcool::ai::agent_run_record_t &record,
    const std::string &session_hint)
{
	if (record.error != "agent run interrupted by service restart")
		return;
	webcool::ai::agent_session_store_t session_store(user_root);
	std::string err;
	if (add_hinted_recovery(
	        root, user_root, record, session_hint, session_store, err))
		return;
	std::vector<webcool::ai::agent_session_record_t> sessions;
	if (!session_store.list(50, sessions, err)) {
		webcool::ai::ai_log_error(
		    "agent.run", "list-restart-sessions", err);
		root.add_bool("recovery_available", false);
		return;
	}
	for (size_t i = 0; i < sessions.size(); ++i) {
		if (sessions[i].project_path != record.project_path)
			continue;
		bool owns_run = sessions[i].last_run_id == record.id;
		for (size_t j = 0; !owns_run && j < sessions[i].messages.size();
		     ++j) {
			owns_run = sessions[i].messages[j].run_id == record.id;
		}
		if (!owns_run)
			continue;
		bool found = false;
		webcool::ai::agent_progress_store_t progress_store(
		    user_root, sessions[i].project_path, sessions[i].id);
		if (!progress_store.exists(found, err)) {
			webcool::ai::ai_log_error(
			    "agent.run", "probe-restart-progress", err);
			root.add_bool("recovery_available", false);
			return;
		}
		root.add_bool("recovery_available", found);
		root.add_text("session_id", sessions[i].id.c_str());
		if (!found)
			return;
		root.add_text(
		    "progress_file", progress_store.relative_path().c_str());

		return;
	}
	root.add_bool("recovery_available", false);
}

void add_persisted_result_json(acl::json_node &root,
    const webcool::ai::agent_result_t &result, const std::string &relative_path)
{
	root.add_bool("result_available", true);
	root.add_bool("result_persisted", true);
	root.add_text("result_file", relative_path.c_str());
	root.add_text("result_decision", result.decision.c_str());
	root.add_bool("changes_applied", result.changes_applied);
	root.add_number("changes_applied_at", result.changes_applied_at);
	if (!result.changes_apply_error.empty()) {
		root.add_text(
		    "changes_apply_error", result.changes_apply_error.c_str());
	}
	root.add_text("task_contract_json", result.task_contract_json.c_str());
	root.add_text(
	    "task_acceptance_json", result.task_acceptance_json.c_str());
	root.add_text(
	    "task_acceptance_status", result.task_acceptance_status.c_str());
	root.add_text("text", result.text.c_str());
	if (!result.reasoning.empty()) {
		root.add_text("reasoning", result.reasoning.c_str());
	}
	if (!result.completion_summary.empty()) {
		root.add_text(
		    "completion_summary", result.completion_summary.c_str());
	}
	root.add_number("rejected_changes",
	    static_cast<long long>(result.rejected_changes));
	acl::json_node &changes = root.get_json().create_array();
	root.add_child("changes", changes);
	for (size_t i = 0; i < result.changes.size(); ++i) {
		acl::json_node &item = changes.add_child(false, true);
		item.add_text("operation", result.changes[i].operation.c_str());
		item.add_text("path", result.changes[i].path.c_str());
		if (!result.changes[i].target_path.empty()) {
			item.add_text("target_path",
			    result.changes[i].target_path.c_str());
		}
		item.add_text("content", result.changes[i].content.c_str());
		item.add_text("reason", result.changes[i].reason.c_str());
		item.add_bool("creates_file", result.changes[i].creates_file);
		item.add_bool(
		    "creates_directory", result.changes[i].creates_directory);
		item.add_number("added_lines", result.changes[i].added_lines);
		item.add_number(
		    "removed_lines", result.changes[i].removed_lines);
		if (!result.changes[i].diff.empty()) {
			item.add_text("diff", result.changes[i].diff.c_str());
		}
		item.add_text("original_content",
		    result.changes[i].original_content.c_str());
		item.add_bool("original_content_available",
		    result.changes[i].original_content_available);
		item.add_number("generation",
		    static_cast<long long>(result.changes[i].generation));
		item.add_text("base_hash", result.changes[i].base_hash.c_str());
		item.add_text(
		    "draft_hash", result.changes[i].draft_hash.c_str());
		item.add_text(
		    "review_status", result.changes[i].review_status.c_str());
	}
}

} // namespace agent_detail
} // namespace action
