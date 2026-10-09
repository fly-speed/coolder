#include "stdafx.h"
// Asynchronous task execution and recovery from durable checkpoints.
#include "coding_runtime.h"

namespace action
{
namespace agent_detail
{

class checkpoint_cleanup_guard_t {
public:
	explicit checkpoint_cleanup_guard_t(
		const std::shared_ptr<agent_runtime_task_t> &task)
		: task_(task)
	{
	}
	~checkpoint_cleanup_guard_t()
	{
		if (!task_->restart_recovery_enabled)
			return;
		webcool::ai::agent_checkpoint_store_t store(
			task_->upload_root, task_->user_root, task_->username);
		std::string err;
		if (!store.remove(task_->id, err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "remove-checkpoint", err);
		}
	}

private:
	std::shared_ptr<agent_runtime_task_t> task_;
};

static void finish_failed_coding_task(
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	const std::string &project_path,
	webcool::ai::agent_run_store_t &run_store,
	webcool::ai::completion_result_t &output, const std::string &err)
{
	const bool cancelled = runtime_cancel_requested(runtime_task) ||
			       err == "agent run cancelled";
	std::string audit_err;
	if (cancelled) {
		if (!run_store.cancel(runtime_task->id, audit_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "audit-cancel", audit_err);
		}
		finish_runtime_task(runtime_task, "cancelled", "", NULL, NULL,
				    NULL, 0);
		webcool::ai::agent_progress_store_t progress_store(
			runtime_task->user_root, project_path,
			runtime_task->session_id);
		std::string progress_err;
		if (!runtime_task->text_preview &&
		    !progress_store.remove(progress_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "remove-cancelled-progress",
						  progress_err);
		}
		std::string draft_err;
		if (!remove_persistent_worktree(runtime_task->user_root,
						project_path, runtime_task->id,
						draft_err)) {
			webcool::ai::ai_log_error("agent.draft",
						  "remove-cancelled-worktree",
						  draft_err);
		}
	} else {
		// Provider/workspace layers already log their failure location. This
		// entry connects that failure to background-task finalization without
		// recording the prompt, reply or tool transcript.
		webcool::ai::ai_log_error("agent.runtime", "finish-failed-run",
					  err);
		if (!run_store.fail(
			    runtime_task->id, err,
			    webcool::ai::provider_client_t::error_category_name(
				    output.error_category),
			    output.http_status, output.retryable_error,
			    audit_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "audit-failure", audit_err);
		}
		finish_runtime_task(runtime_task, "failed", err, &output, NULL,
				    NULL, 0);
	}
}

static void prepare_final_change_review(
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	webcool::ai::agent_result_store_t &result_store,
	webcool::ai::agent_result_t &durable_result,
	webcool::ai::completion_result_t &output,
	std::vector<agent_change_proposal_t> &changes)
{
	// Build and persist the review snapshot, but deliberately do not apply it.
	// The project remains unchanged until the authenticated user accepts the
	// revision through the separate preview/apply endpoint. Rejection therefore
	// records feedback only and can never require a compensating source rewrite.
	if (!changes.empty()) {
		std::vector<webcool::ai::workspace_change_input_t> inputs;
		inputs.reserve(changes.size());
		for (size_t i = 0; i < changes.size(); ++i) {
			if (changes[i].review_status != "pending")
				continue;
			webcool::ai::workspace_change_input_t input;
			input.operation = changes[i].operation;
			input.path = changes[i].path;
			input.target_path = changes[i].target_path;
			input.content = changes[i].content;
			input.reason = changes[i].reason;
			inputs.push_back(input);
		}
		webcool::ai::workspace_change_set_store_t change_store(
			runtime_task->user_root);
		webcool::ai::workspace_change_set_preview_t review_preview;
		std::string preview_err;
		const bool previewed =
			inputs.empty() ||
			change_store.create(inputs, review_preview,
					    preview_err);
		if (previewed)
			attach_change_preview_diffs(changes, review_preview);
		durable_result.changes = changes;
		set_runtime_changes_applied(
			runtime_task, durable_result.changes_applied,
			previewed ? "" : preview_err, "", 0);
		if (previewed && !inputs.empty()) {
			output.text +=
				"\n\nWebCool：上述文件变更已保存到修订区，"
				"接受后才会写入正式源码；拒绝不会修改项目文件。";
		} else if (inputs.empty()) {
			output.text +=
				"\n\nWebCool：本轮产生的文件修订已在推理过程中完成审查。";
		} else {
			durable_result.changes_apply_error = preview_err;
			output.text +=
				"\n\nWebCool：生成成果已保存，但准备修订预览失败：" +
				preview_err;
			webcool::ai::ai_log_error("agent.runtime",
						  "prepare-change-review",
						  preview_err);
		}
		durable_result.text = output.text;
		std::string update_err;
		if (!result_store.save(durable_result, update_err)) {
			// The source tree is still untouched; log the metadata refresh failure
			// while keeping the original durable proposal for recovery.
			webcool::ai::ai_log_error("agent.runtime",
						  "persist-review-state",
						  update_err);
		} else {
			webcool::ai::agent_result_t merged_result;
			bool merged_found = false;
			if (result_store.load(runtime_task->id, merged_result,
					      merged_found, update_err) &&
			    merged_found) {
				durable_result = merged_result;
				changes = merged_result.changes;
				publish_runtime_change_reviews(
					runtime_task,
					std::vector<
						webcool::ai::
							agent_change_review_t>(),
					merged_result);
			} else if (!update_err.empty()) {
				webcool::ai::ai_log_error(
					"agent.runtime",
					"reload-persisted-review-state",
					update_err);
			}
		}
	}
}

void run_async_coding_task(
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	webcool::ai::provider_config_t provider, std::string api_key,
	const std::string &project_path, const std::string &original_prompt,
	const std::string &initial_prompt,
	const std::string &recovered_reasoning,
	const std::vector<webcool::ai::completion_image_t> &request_images,
	size_t initial_completed_tool_calls, size_t max_tool_calls,
	long long max_output_tokens, size_t max_no_progress_tool_calls,
	size_t tool_context_compaction_bytes,
	const webcool::ai::sandbox_limits_t &sandbox_limits,
	const std::string &thinking_mode, const std::string &reasoning_effort)
{
	// Normal completion, failure and cancellation all retire the encrypted
	// checkpoint. A process crash skips this destructor, leaving it for recovery.
	checkpoint_cleanup_guard_t checkpoint_cleanup(runtime_task);
	webcool::ai::completion_result_t output;
	std::vector<agent_tool_trace_t> traces;
	std::vector<agent_change_proposal_t> changes;
	std::string memory_summary;
	std::string completion_summary;
	std::string session_title;
	size_t rejected_changes = 0;
	std::string err;
	bool completed = false;
	if (!wait_for_runtime_admission(runtime_task)) {
		err = "agent run cancelled";
	} else
		try {
			append_simple_operation_event(
				runtime_task, "execution_admitted", "preparing",
				"", initial_completed_tool_calls);
			webcool::ai::agent_workspace_t workspace(
				runtime_task->user_root);
			if (provider.protocol == "openai_images") {
				completed = run_assistant_image_task(
					runtime_task, provider, api_key, output,
					err);
			} else
				completed = run_coding_tool_loop(
					provider, api_key, workspace,
					project_path, original_prompt,
					initial_prompt, recovered_reasoning,
					request_images,
					initial_completed_tool_calls,
					max_tool_calls, max_output_tokens,
					max_no_progress_tool_calls,
					tool_context_compaction_bytes,
					sandbox_limits, thinking_mode,
					reasoning_effort, output, traces,
					changes, memory_summary,
					completion_summary, session_title,
					rejected_changes, runtime_task, err);
		} catch (const std::exception &exception) {
			err = exception.what();
			webcool::ai::ai_log_error("agent.runtime", "exception",
						  err);
		} catch (...) {
			err = "unexpected agent runtime exception";
			webcool::ai::ai_log_error("agent.runtime",
						  "unknown-exception", err);
		}
	std::fill(api_key.begin(), api_key.end(), '\0');
	provider.api_key_ciphertext.clear();
	webcool::ai::agent_run_store_t run_store(runtime_task->user_root);
	if (!completed) {
		finish_failed_coding_task(runtime_task, project_path, run_store,
					  output, err);
		return;
	}
	if (runtime_cancel_requested(runtime_task)) {
		std::string audit_err;
		if (!run_store.cancel(runtime_task->id, audit_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "audit-late-cancel",
						  audit_err);
		}
		finish_runtime_task(runtime_task, "cancelled", "", NULL, NULL,
				    NULL, 0);
		webcool::ai::agent_progress_store_t progress_store(
			runtime_task->user_root, project_path,
			runtime_task->session_id);
		std::string progress_err;
		if (!runtime_task->text_preview &&
		    !progress_store.remove(progress_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "remove-cancelled-progress",
						  progress_err);
		}
		std::string draft_err;
		if (!remove_persistent_worktree(runtime_task->user_root,
						project_path, runtime_task->id,
						draft_err)) {
			webcool::ai::ai_log_error(
				"agent.draft", "remove-late-cancelled-worktree",
				draft_err);
		}
		return;
	}
	completion_summary = completion_summary_for_run(
		completion_summary, memory_summary, output.text, changes,
		original_prompt);
	// Persist the complete final artifact before declaring the run complete or
	// deleting its recovery checkpoint. This is the durable boundary that keeps
	// generated files and reasoning available across service restarts.
	webcool::ai::agent_result_t durable_result;
	durable_result.task_contract_json = runtime_task->task_contract_json;
	durable_result.task_acceptance_json =
		runtime_task->task_acceptance_json;
	durable_result.task_acceptance_status =
		runtime_task->task_acceptance_status;
	durable_result.run_id = runtime_task->id;
	durable_result.project_path = project_path;
	durable_result.session_id = runtime_task->session_id;
	durable_result.text = output.text;
	// Some compatible providers expose reasoning only through streaming deltas.
	// Preserve that snapshot when the final response omits a reasoning field.
	durable_result.reasoning =
		output.reasoning.empty() ?
			runtime_reasoning_snapshot(runtime_task) :
			output.reasoning;
	durable_result.completion_summary = completion_summary;
	durable_result.changes = changes;
	durable_result.rejected_changes = rejected_changes;
	durable_result.decision = changes.empty() ? "accepted" : "pending";
	durable_result.saved_at = static_cast<long long>(time(NULL));
	webcool::ai::agent_result_store_t result_store(runtime_task->user_root,
						       project_path);
	std::string result_err;
	if (!result_store.save(durable_result, result_err)) {
		webcool::ai::ai_log_error("agent.runtime",
					  "persist-final-result", result_err);
		std::string audit_err;
		if (!run_store.fail(runtime_task->id, result_err, audit_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "audit-result-failure",
						  audit_err);
		}
		finish_runtime_task(runtime_task, "failed", result_err, &output,
				    &traces, NULL, rejected_changes);
		return;
	}
	{
		webcool::ai::agent_result_t merged_result;
		bool merged_found = false;
		std::string merged_err;
		if (result_store.load(runtime_task->id, merged_result,
				      merged_found, merged_err) &&
		    merged_found) {
			durable_result = merged_result;
			changes = merged_result.changes;
		} else if (!merged_err.empty()) {
			webcool::ai::ai_log_error("agent.runtime",
						  "reload-final-review-state",
						  merged_err);
		}
	}

	prepare_final_change_review(runtime_task, result_store, durable_result,
				    output, changes);

	set_runtime_result_artifact(
		runtime_task, result_store.relative_path(runtime_task->id),
		durable_result.decision);
	if (!run_store.complete(
		    runtime_task->id, output.input_tokens,
		    output.cached_input_tokens, output.output_tokens,
		    output.reasoning_tokens, output.latency_ms,
		    static_cast<long long>(
			    runtime_completed_tool_count(runtime_task)),
		    static_cast<long long>(changes.size()),
		    static_cast<long long>(rejected_changes), err)) {
		finish_runtime_task(runtime_task, "failed", err, &output, NULL,
				    NULL, 0);
		return;
	}
	if (runtime_task->remember_session &&
	    !runtime_task->session_id.empty()) {
		webcool::ai::agent_session_store_t session_store(
			runtime_task->user_root);
		std::string session_err;
		if (!session_store.update_after_run(
			    runtime_task->session_id,
			    webcool::ai::agent_session_task_title(
				    original_prompt),
			    memory_summary, runtime_task->id, original_prompt,
			    output.text.empty() ? memory_summary : output.text,
			    "completed", durable_result.reasoning,
			    completion_summary, output.latency_ms,
			    output.input_tokens, output.cached_input_tokens,
			    output.output_tokens, output.reasoning_tokens,
			    session_err)) {
			// The coding result remains valid; losing optional conversation memory
			// must be visible to operators without changing the completed audit.
			webcool::ai::ai_log_error(
				"agent.runtime", "update-session", session_err);
		}
	}
	// A completed answer supersedes the recovery state. Failure paths return
	// earlier and deliberately retain it for the user's next attempt.
	webcool::ai::agent_progress_store_t progress_store(
		runtime_task->user_root, project_path,
		runtime_task->session_id);
	std::string progress_err;
	if (!runtime_task->text_preview &&
	    !progress_store.remove(progress_err)) {
		webcool::ai::ai_log_error("agent.runtime",
					  "remove-completed-progress",
					  progress_err);
	}
	bool pending_review = false;
	for (size_t i = 0; i < changes.size(); ++i) {
		pending_review =
			pending_review || changes[i].review_status == "pending";
	}
	if (!pending_review) {
		std::string draft_err;
		if (!schedule_reviewed_worktree_cleanup(
			    runtime_task->user_root, project_path,
			    runtime_task->id, draft_err)) {
			webcool::ai::ai_log_error(
				"agent.draft",
				"queue-completed-worktree-cleanup", draft_err);
		}
	}
	set_runtime_completion_summary(runtime_task, completion_summary);
	finish_runtime_task(runtime_task, "completed", "", &output, &traces,
			    &changes, rejected_changes);
}

std::shared_ptr<agent_runtime_task_t>
recover_runtime_task(const std::string &upload_root,
		     const std::string &user_root, const std::string &username,
		     const webcool::ai::agent_run_record_t &record,
		     std::string &err)
{
	std::shared_ptr<agent_runtime_task_t> existing =
		find_runtime_task(user_root, record.id);
	if (existing)
		return existing;
	webcool::ai::agent_checkpoint_store_t checkpoint_store(
		upload_root, user_root, username);
	if (!checkpoint_store.exists(record.id))
		return existing;
	webcool::ai::agent_checkpoint_t checkpoint;
	if (!checkpoint_store.load(record.id, checkpoint, err))
		return existing;
	if (checkpoint.provider_id != record.provider_id ||
	    checkpoint.project_path != record.project_path) {
		err = "agent restart checkpoint does not match run metadata";
		webcool::ai::ai_log_error("agent.runtime", "validate-recovery",
					  err);
		return existing;
	}

	webcool::ai::provider_store_t provider_store(upload_root, user_root,
						     username);
	std::vector<webcool::ai::provider_config_t> providers;
	if (!provider_store.list(providers, err))
		return existing;
	const webcool::ai::provider_config_t *provider =
		select_provider(providers, checkpoint.provider_id);
	if (provider == NULL) {
		err = "saved AI provider is unavailable for agent recovery";
		webcool::ai::ai_log_error("agent.runtime",
					  "select-recovery-provider", err);
		return existing;
	}
	// Selecting a project for the coding agent is an explicit per-run grant to
	// send that project's non-sensitive text to the chosen model. Workspace path
	// checks still exclude credentials, VCS metadata and agent checkpoints.
	webcool::ai::provider_config_t effective_provider = *provider;
	effective_provider.allow_file_content = true;
	if (!webcool::ai::provider_client_t::configure_response_state(
		    effective_provider, checkpoint.response_state_mode, err))
		return existing;
	std::string previous_summary;
	if (!checkpoint.session_id.empty()) {
		webcool::ai::agent_session_store_t session_store(user_root);
		webcool::ai::agent_session_record_t session;
		if (!session_store.get(checkpoint.session_id, session, err))
			return existing;
		previous_summary = session.summary;
	}
	webcool::ai::agent_workspace_t workspace(user_root);
	std::string initial_prompt;
	std::string recovered_reasoning;
	size_t completed_tool_calls = 0;
	bool resumed_from_progress = false;
	webcool::ai::agent_progress_store_t progress_store(
		user_root, checkpoint.project_path, checkpoint.session_id);
	if (!load_matching_coding_progress(
		    progress_store, effective_provider, checkpoint.project_path,
		    checkpoint.prompt, initial_prompt, recovered_reasoning,
		    completed_tool_calls, resumed_from_progress, err)) {
		return existing;
	}
	if (!resumed_from_progress &&
	    !compose_initial_prompt(workspace, effective_provider, user_root,
				    checkpoint.project_path, checkpoint.prompt,
				    checkpoint.session_id,
				    checkpoint.execution_mode.empty() ?
					    "standard" :
					    checkpoint.execution_mode,
				    previous_summary, initial_prompt, err,
				    checkpoint.ui_language != "en"))
		return existing;
	std::string api_key;
	if (!provider_store.reveal_api_key(*provider, api_key, err))
		return existing;

	std::shared_ptr<agent_runtime_task_t> task(new agent_runtime_task_t());
	task->id = record.id;
	task->upload_root = upload_root;
	task->user_root = user_root;
	task->username = username;
	task->project_path = record.project_path;
	task->session_id = checkpoint.session_id;
	task->ui_language = checkpoint.ui_language;
	task->remember_session = checkpoint.remember_session;
	task->restart_recovery_enabled = true;
	task->resumed_after_restart = true;
	task->resumed_from_progress = resumed_from_progress;
	task->progress_file = progress_store.relative_path();
	task->phase = "recovering";
	if (!register_runtime_task(task, err)) {
		std::fill(api_key.begin(), api_key.end(), '\0');
		if (err == "agent run is already active") {
			return find_runtime_task(user_root, record.id);
		}
		return existing;
	}
	webcool::ai::provider_config_t provider_copy = effective_provider;
	provider_copy.api_key_ciphertext.clear();
	const std::string project_path = checkpoint.project_path;
	// Restart recovery uses the administrator's current tool budget while the
	// encrypted checkpoint preserves the original per-run thinking choice.
	const webcool::ai::ai_admin_policy_t recovery_policy =
		webcool::ai::ai_runtime_policy_get();
	const std::string recovery_mode = checkpoint.execution_mode.empty() ?
						  "standard" :
						  checkpoint.execution_mode;
	long long recovery_requested_tokens = std::min<long long>(
		checkpoint.max_output_tokens > 0 ?
			checkpoint.max_output_tokens :
			16384,
		static_cast<long long>(recovery_policy.max_output_tokens));
	if (provider_requires_extended_reasoning_budget(*provider) &&
	    recovery_requested_tokens < 16384) {
		recovery_requested_tokens = std::min<long long>(
			static_cast<long long>(
				recovery_policy.max_output_tokens),
			16384);
	}
	webcool::ai::agent_execution_profile_t execution_profile;
	if (!webcool::ai::build_agent_execution_profile(
		    recovery_mode,
		    static_cast<size_t>(
			    webcool::ai::ai_tool_call_limit_for_mode(
				    recovery_policy, recovery_mode)),
		    static_cast<size_t>(
			    recovery_policy.max_no_progress_tool_calls),
		    static_cast<size_t>(
			    recovery_policy.tool_context_compaction_kib) *
			    1024,
		    static_cast<long long>(recovery_policy.max_output_tokens),
		    static_cast<long long>(
			    recovery_policy.quick_mode_output_tokens),
		    recovery_requested_tokens, execution_profile, err)) {
		remove_runtime_task(user_root, record.id);
		std::fill(api_key.begin(), api_key.end(), '\0');
		webcool::ai::ai_log_error("agent.runtime",
					  "build-recovery-execution-profile",
					  err);
		return existing;
	}
	const size_t max_tool_calls = execution_profile.max_tool_calls;
	const size_t max_no_progress_tool_calls =
		execution_profile.max_no_progress_calls;
	const size_t tool_context_compaction_bytes =
		execution_profile.context_compaction_bytes;
	long long max_tokens = execution_profile.max_output_tokens;
	const long long profiled_max_tokens = max_tokens;
	if (provider_requires_extended_reasoning_budget(*provider) &&
	    max_tokens < 16384) {
		max_tokens = std::min<long long>(
			static_cast<long long>(
				recovery_policy.max_output_tokens),
			16384);
	}
	const webcool::ai::sandbox_limits_t sandbox_limits =
		recovery_policy.sandbox_limits;
	acl::json recovered_json;
	acl::json_node &recovered = recovered_json.create_node();
	recovered.add_text("event", "run_recovered_after_restart");
	recovered.add_text("provider_id", provider->id.c_str());
	recovered.add_text("provider_protocol", provider->protocol.c_str());
	recovered.add_text("model", provider->model.c_str());
	recovered.add_text("execution_mode", recovery_mode.c_str());
	recovered.add_number("max_tool_calls",
			     static_cast<long long>(max_tool_calls));
	recovered.add_number(
		"max_no_progress_tool_calls",
		static_cast<long long>(max_no_progress_tool_calls));
	recovered.add_number("max_output_tokens", max_tokens);
	recovered.add_number("profiled_max_output_tokens", profiled_max_tokens);
	recovered.add_bool("reasoning_budget_floor_applied",
			   max_tokens != profiled_max_tokens);
	recovered.add_number("completed_tool_calls_before_recovery",
			     static_cast<long long>(completed_tool_calls));
	recovered.add_bool("resumed_from_progress", resumed_from_progress);
	append_runtime_operation_event(task, recovered);
	const std::shared_ptr<acl::fiber> worker = acl::gofiber(
		[task, provider_copy, api_key, project_path, initial_prompt,
		 checkpoint, recovered_reasoning, completed_tool_calls,
		 max_tool_calls, max_no_progress_tool_calls,
		 tool_context_compaction_bytes, sandbox_limits,
		 max_tokens]() mutable {
			run_async_coding_task(
				task, provider_copy, api_key, project_path,
				checkpoint.prompt, initial_prompt,
				recovered_reasoning,
				std::vector<webcool::ai::completion_image_t>(),
				completed_tool_calls, max_tool_calls,
				max_tokens, max_no_progress_tool_calls,
				tool_context_compaction_bytes, sandbox_limits,
				checkpoint.thinking_mode,
				checkpoint.reasoning_effort);
		});
	attach_runtime_worker(task, worker);
	std::fill(api_key.begin(), api_key.end(), '\0');
	return task;
}

} // namespace agent_detail
} // namespace action
