#include "stdafx.h"
// Process-wide live tasks, admission, progress, operation logs and result serialization.
#include "coding_runtime.h"

namespace action
{
namespace agent_detail
{

webcool::mutex g_agent_runtime_mutex;
std::map<std::string, std::shared_ptr<agent_runtime_task_t>>
    g_agent_runtime_tasks;
const size_t kMaxActiveAgentRuns = 16;
const size_t kMaxQueuedAgentRuns = 64;
const size_t kMaxCompletedRuntimeResults = 20;
const size_t kMaxEventSubscribersPerRun = 4;
const long long kRuntimeResultLifetimeSeconds = 60 * 60;
unsigned long long g_next_agent_queue_sequence = 1;

std::string runtime_task_key(
    const std::string &user_root, const std::string &id)
{
	return user_root + "\n" + id;
}

void cleanup_runtime_tasks_locked(long long now)
{
	// Apply both a time bound and a count bound so transient model replies and
	// change proposals cannot accumulate for the lifetime of the service.
	for (std::map<std::string,
	         std::shared_ptr<agent_runtime_task_t>>::iterator it =
	         g_agent_runtime_tasks.begin();
	     it != g_agent_runtime_tasks.end();) {
		const std::shared_ptr<agent_runtime_task_t> &task = it->second;
		if (task->done && task->finished_at > 0 &&
		    now - task->finished_at > kRuntimeResultLifetimeSeconds) {
			it = g_agent_runtime_tasks.erase(it);
		} else {
			++it;
		}
	}
	for (;;) {
		size_t completed = 0;
		std::map<std::string,
		    std::shared_ptr<agent_runtime_task_t>>::iterator oldest =
		    g_agent_runtime_tasks.end();
		for (std::map<std::string,
		         std::shared_ptr<agent_runtime_task_t>>::iterator it =
		         g_agent_runtime_tasks.begin();
		     it != g_agent_runtime_tasks.end(); ++it) {
			if (!it->second->done)
				continue;
			++completed;
			if (!(oldest == g_agent_runtime_tasks.end() ||
			        it->second->finished_at <
			            oldest->second->finished_at))
				continue;
			oldest = it;
		}
		if (completed <= kMaxCompletedRuntimeResults ||
		    oldest == g_agent_runtime_tasks.end())
			break;
		g_agent_runtime_tasks.erase(oldest);
	}
}

bool register_runtime_task(
    const std::shared_ptr<agent_runtime_task_t> &task, std::string &err)
{
	// Resolve scope before taking the runtime lock: external/local project mappings
	// must compare against the actual file locations, not a shared empty UI path.
	task->scope.user_root = task->user_root;
	task->scope.session_id = task->session_id;
	task->scope.tool_free = task->text_preview;
	task->scope.conversation_id = task->conversation_id;
	task->scope.document_path =
	    webcool::ai::normalize_agent_resource_path(task->document_path);
	if (!task->text_preview) {
		std::string actual_project;
		if (!webcool::ai::agent_workspace_t::resolve_project_root(
		        task->user_root, task->project_path, actual_project,
		        err))
			return false;
		task->scope.project_path =
		    webcool::ai::normalize_agent_resource_path(actual_project);
	}
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	cleanup_runtime_tasks_locked(static_cast<long long>(time(NULL)));
	const std::string key = runtime_task_key(task->user_root, task->id);
	if (g_agent_runtime_tasks.find(key) != g_agent_runtime_tasks.end()) {
		err = "agent run is already active";
		webcool::ai::ai_log_error(
		    "agent.runtime", "register-duplicate", err);
		return false;
	}
	size_t registered = 0;
	size_t user_registered = 0;
	for (std::map<std::string,
	         std::shared_ptr<agent_runtime_task_t>>::iterator it =
	         g_agent_runtime_tasks.begin();
	     it != g_agent_runtime_tasks.end(); ++it) {
		if (it->second->done)
			continue;
		++registered;
		if (it->second->user_root != task->user_root)
			continue;
		++user_registered;
		const bool same_saved_session = !task->session_id.empty() &&
		    it->second->session_id == task->session_id;
		if (!webcool::ai::agent_run_scopes_conflict(
		        task->scope, it->second->scope))
			continue;
		err = task->text_preview ?
		    (task->assistant_chat ?
		            "this assistant conversation already has an active run" :
		            "this document already has an active AI run") :
		    (same_saved_session ?
		            "this agent session already has an active run" :
		            "this project already has an active agent run");
		webcool::ai::ai_log_error(
		    "agent.runtime", "register-scope-conflict", err);
		return false;
	}
	const size_t policy_user_limit = static_cast<size_t>(
	    webcool::ai::ai_runtime_policy_get().max_active_runs_per_user);
	const size_t max_user_registered =
	    std::max<size_t>(4, policy_user_limit * 4);
	if (registered >= kMaxQueuedAgentRuns ||
	    user_registered >= max_user_registered) {
		err = user_registered >= max_user_registered ?
		    "this user's agent run queue is full" :
		    "the server agent run queue is full";
		webcool::ai::ai_log_error(
		    "agent.runtime", "register-capacity", err);
		return false;
	}
	task->queue_sequence = g_next_agent_queue_sequence++;
	g_agent_runtime_tasks[key] = task;
	return true;
}

static bool admit_runtime_task_locked(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::vector<webcool::ai::agent_run_slot_t> slots;
	slots.reserve(g_agent_runtime_tasks.size());
	for (std::map<std::string,
	         std::shared_ptr<agent_runtime_task_t>>::const_iterator it =
	         g_agent_runtime_tasks.begin();
	     it != g_agent_runtime_tasks.end(); ++it) {
		webcool::ai::agent_run_slot_t slot;
		slot.key = it->first;
		slot.user_scope = it->second->user_root;
		slot.sequence = it->second->queue_sequence;
		slot.admitted = it->second->execution_admitted;
		slot.done = it->second->done;
		slot.cancelled = it->second->cancel_requested;
		slot.paused = it->second->pause_requested;
		slots.push_back(slot);
	}
	const size_t user_limit = static_cast<size_t>(
	    webcool::ai::ai_runtime_policy_get().max_active_runs_per_user);
	const std::string selected = webcool::ai::select_next_agent_run(
	    slots, kMaxActiveAgentRuns, user_limit);
	if (selected == runtime_task_key(task->user_root, task->id)) {
		task->execution_admitted = true;
		task->phase = "preparing";
		++task->event_version;
		return true;
	}
	return false;
}

bool wait_for_runtime_admission(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	for (;;) {
		{
			std::lock_guard<webcool::mutex> guard(
			    g_agent_runtime_mutex);
			if (task->done || task->cancel_requested)
				return false;
			if (task->execution_admitted)
				return true;

			if ((!task->pause_requested) &&
			    (admit_runtime_task_locked(task)))
				return true;
		}
		// Cooperative polling avoids blocking the ACL scheduler thread. Selecting
		// the earliest eligible sequence above provides FIFO fairness without a
		// head-of-line block when one user has exhausted only their own quota.
		acl::fiber::delay(25);
	}
}

void remove_runtime_task(const std::string &user_root, const std::string &id)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	g_agent_runtime_tasks.erase(runtime_task_key(user_root, id));
}

bool runtime_cancel_requested(const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->cancel_requested;
}

size_t runtime_completed_tool_count(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return 0;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->completed_tool_calls;
}

static bool resume_runtime_task_locked(
    const std::shared_ptr<agent_runtime_task_t> &task, bool entered_pause)
{
	if (!(entered_pause || task->phase == "paused" ||
	        task->phase == "pausing" || task->phase == "resuming"))
		return true;
	task->phase = task->phase_before_pause.empty() ?
	    "model_call" :
	    task->phase_before_pause;
	task->phase_before_pause.clear();
	++task->event_version;

	return true;
}

static void mark_runtime_paused_locked(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (task->phase != "paused") {
		// The control endpoint normally captures the original phase. Keep a
		// fallback for a pause racing with a progress update.
		if (task->phase_before_pause.empty() &&
		    task->phase != "pausing") {
			task->phase_before_pause = task->phase;
		}
		task->phase = "paused";
		++task->event_version;
	}
}

bool wait_while_runtime_paused(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	bool entered_pause = false;
	for (;;) {
		{
			std::lock_guard<webcool::mutex> guard(
			    g_agent_runtime_mutex);
			if (task->done || task->cancel_requested)
				return false;
			if (!task->pause_requested) {
				return resume_runtime_task_locked(
				    task, entered_pause);
			}
			entered_pause = true;
			mark_runtime_paused_locked(task);
		}
		// Agent workers run as ACL fibers, so this yields to other users rather
		// than occupying an OS worker thread while the task is paused.
		acl::fiber::delay(100);
	}
}

bool request_runtime_pause(const std::string &user_root, const std::string &id,
    bool paused, bool &already_done, bool &pause_requested)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	const std::map<std::string,
	    std::shared_ptr<agent_runtime_task_t>>::iterator it =
	    g_agent_runtime_tasks.find(runtime_task_key(user_root, id));
	if (it == g_agent_runtime_tasks.end())
		return false;
	agent_runtime_task_t &task = *it->second;
	already_done = task.done;
	if (already_done)
		return true;
	if (paused != task.pause_requested) {
		if (paused) {
			if (task.phase_before_pause.empty() &&
			    task.phase != "paused" && task.phase != "pausing") {
				task.phase_before_pause = task.phase;
			}
			task.pause_requested = true;
			task.phase = "pausing";
		} else {
			task.pause_requested = false;
			// The worker restores its precise pre-pause phase when it wakes.
			if (task.phase == "paused" || task.phase == "pausing") {
				task.phase = "resuming";
			}
		}
		++task.event_version;
	}
	pause_requested = task.pause_requested;
	return true;
}

bool request_runtime_cancel(
    const std::string &user_root, const std::string &id, bool &already_done)
{
	std::shared_ptr<acl::fiber> worker;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		const std::map<std::string,
		    std::shared_ptr<agent_runtime_task_t>>::iterator it =
		    g_agent_runtime_tasks.find(runtime_task_key(user_root, id));
		if (it == g_agent_runtime_tasks.end())
			return false;
		already_done = it->second->done;
		if (!already_done) {
			it->second->cancel_requested = true;
			it->second->pause_requested = false;
			it->second->phase = "cancelling";
			++it->second->event_version;
			worker = it->second->worker;
		}
	}
	// Never call into the scheduler while holding the runtime mutex: the woken
	// provider fiber immediately checks the observer and takes the same mutex.
	if (!worker)
		return true;
	(void)worker->kill(false);
	return true;
}

void attach_runtime_worker(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::shared_ptr<acl::fiber> &worker)
{
	bool cancel_now = false;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (!task->done) {
			task->worker = worker;
			cancel_now = task->cancel_requested;
		}
	}
	// Cancellation may arrive between gofiber() and handle publication.
	if (cancel_now && worker)
		(void)worker->kill(false);
}

void update_runtime_progress(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &phase, const std::string &current_tool,
    size_t completed_tool_calls)
{
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (task->done || task->cancel_requested)
			return;
		task->phase = phase;
		task->current_tool = current_tool;
		task->completed_tool_calls = completed_tool_calls;
		++task->event_version;
	}
	append_simple_operation_event(
	    task, "phase_changed", phase, current_tool, completed_tool_calls);
}

void complete_runtime_tool(const std::shared_ptr<agent_runtime_task_t> &task,
    const agent_tool_trace_t &trace, size_t completed_tool_calls)
{
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (task->done || task->cancel_requested)
			return;
		task->phase = "model_call";
		task->current_tool.clear();
		task->completed_tool_calls = completed_tool_calls;
		if (trace.ok &&
		    (trace.name == "workspace.create" ||
		        trace.name == "workspace.mkdir")) {
			++task->workspace_mutation_version;
			task->last_workspace_mutation_path = trace.path;
		}
		++task->event_version;
	}
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", "tool_completed");
	event.add_text("tool", trace.name.c_str());
	if (!trace.path.empty())
		event.add_text("path", trace.path.c_str());
	event.add_bool("ok", trace.ok);
	event.add_bool("truncated", trace.truncated);
	event.add_bool("native", trace.native);
	event.add_number("completed_tool_calls",
	    static_cast<long long>(completed_tool_calls));
	append_runtime_operation_event(task, event);
}

void begin_runtime_model_stream(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->done || task->cancel_requested)
		return;
	task->streamed_text.clear();
	// Separate reasoning emitted by consecutive model calls in the tool loop.
	if (!task->streamed_reasoning.empty() &&
	    task->streamed_reasoning.size() + 2 <= 1024 * 1024) {
		task->streamed_reasoning += "\n\n";
	}
	task->phase = "model_stream";
	++task->event_version;
}

std::string runtime_reasoning_snapshot(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->streamed_reasoning;
}

std::string runtime_reasoning_for_save(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	// Non-streaming compatible providers may expose reasoning only in the final
	// completion object. Prefer it after completion; otherwise save the live
	// accumulated stream visible in the reasoning window.
	if (!(task->done && !task->output.reasoning.empty()))
		return task->streamed_reasoning;
	return task->output.reasoning;
}

void mark_runtime_recovery_available(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &progress_file)
{
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		task->recovery_available = true;
		task->progress_file = progress_file;
		++task->event_version;
	}
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", "recovery_checkpoint_available");
	event.add_text("progress_file", progress_file.c_str());
	append_runtime_operation_event(task, event);
}

bool append_runtime_reasoning_delta(
    const std::shared_ptr<agent_runtime_task_t> &task, const std::string &delta)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->done || task->cancel_requested)
		return false;
	// Keep the expandable preview bounded independently from the final answer.
	const size_t limit = 1024 * 1024;
	if (task->streamed_reasoning.size() < limit) {
		const size_t keep = std::min(
		    limit - task->streamed_reasoning.size(), delta.size());
		task->streamed_reasoning.append(delta, 0, keep);
	}
	++task->event_version;
	return true;
}

bool append_runtime_model_delta(
    const std::shared_ptr<agent_runtime_task_t> &task, const std::string &delta)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->done || task->cancel_requested)
		return false;
	// The transient preview has the same one-hour lifecycle as the completed
	// reply and a stricter 1 MiB memory bound. It is never persisted or logged.
	const size_t limit = 1024 * 1024;
	if (task->streamed_text.size() < limit) {
		const size_t keep =
		    std::min(limit - task->streamed_text.size(), delta.size());
		task->streamed_text.append(delta, 0, keep);
	}
	++task->event_version;
	return true;
}

unsigned long long runtime_event_version(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return 0;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->event_version;
}

unsigned long long runtime_staged_change_version(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return 0;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->staged_change_version;
}

bool begin_runtime_subscription(
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return true;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->event_subscribers >= kMaxEventSubscribersPerRun)
		return false;
	++task->event_subscribers;
	return true;
}

std::shared_ptr<agent_runtime_task_t> find_runtime_task(
    const std::string &user_root, const std::string &id)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	cleanup_runtime_tasks_locked(static_cast<long long>(time(NULL)));
	const std::map<std::string,
	    std::shared_ptr<agent_runtime_task_t>>::iterator it =
	    g_agent_runtime_tasks.find(runtime_task_key(user_root, id));
	return it == g_agent_runtime_tasks.end() ?
	    std::shared_ptr<agent_runtime_task_t>() :
	    it->second;
}

void finish_runtime_task(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &status, const std::string &error,
    const webcool::ai::completion_result_t *output,
    const std::vector<agent_tool_trace_t> *traces,
    const std::vector<agent_change_proposal_t> *changes,
    size_t rejected_changes)
{
	std::string final_status = status;
	std::string final_error = error;
	if (task->assistant_chat) {
		webcool::ai::assistant_message_t message;
		message.role = "assistant";
		message.run_id = task->id;
		message.failed = status != "completed";
		{
			std::lock_guard<webcool::mutex> guard(
			    g_agent_runtime_mutex);
			const webcool::ai::completion_result_t &result =
			    output ? *output : task->output;
			message.text = status == "completed" ?
			    result.text :
			    (!error.empty() ? error : "生成已停止。");
			message.input = result.input_tokens;
			message.output = result.output_tokens;
			message.duration = result.latency_ms;
		}
		std::string save_err;
		if (!webcool::ai::assistant_session_store_t(task->user_root)
		         .append(task->conversation_id, message, save_err)) {
			webcool::ai::ai_log_error(
			    "assistant", "persist-reply", save_err);
			final_status = "failed";
			final_error =
			    "AI reply could not be saved: " + save_err;
		}
	}

	std::string persisted_reasoning;
	std::string persisted_reply;
	long long persisted_duration_ms = 0;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		task->status = final_status;
		task->error = final_error;
		task->done = true;
		task->phase = status;
		task->current_tool.clear();
		task->finished_at = static_cast<long long>(time(NULL));
		if (output != NULL)
			task->output = *output;
		if (traces != NULL)
			task->traces = *traces;
		if (changes != NULL)
			task->changes = *changes;
		task->rejected_changes = rejected_changes;
		persisted_reasoning = task->output.reasoning.empty() ?
		    task->streamed_reasoning :
		    task->output.reasoning;
		persisted_reply = !error.empty() ? error : task->streamed_text;
		persisted_duration_ms = task->output.latency_ms;
		++task->event_version;
		cleanup_runtime_tasks_locked(task->finished_at);
	}
	acl::json finished_json;
	acl::json_node &finished = finished_json.create_node();
	finished.add_text("event", "run_finished");
	finished.add_text("status", status.c_str());
	finished.add_number("completed_tool_calls",
	    static_cast<long long>(task->completed_tool_calls));
	finished.add_number(
	    "proposed_changes", static_cast<long long>(task->changes.size()));
	finished.add_number(
	    "rejected_changes", static_cast<long long>(rejected_changes));
	finished.add_number("latency_ms", task->output.latency_ms);
	finished.add_number("input_tokens", task->output.input_tokens);
	finished.add_number(
	    "cached_input_tokens", task->output.cached_input_tokens);
	finished.add_number("output_tokens", task->output.output_tokens);
	finished.add_number("reasoning_tokens", task->output.reasoning_tokens);
	if (!error.empty()) {
		finished.add_text(
		    "error", sanitize_operation_detail(error).c_str());
	}
	append_runtime_operation_event(task, finished);
	// Successful runs are saved after their final summary/result is durable. Save
	// only failure/cancellation here so their already-visible reasoning survives a
	// page reload without duplicating the normal completion path.
	if (status != "completed" && task->remember_session &&
	    !task->session_id.empty() && !task->original_prompt.empty()) {
		if (persisted_reply.empty()) {
			persisted_reply = status == "cancelled" ?
			    "智能体运行已取消。" :
			    "智能体运行失败。";
		}
		webcool::ai::agent_session_store_t session_store(
		    task->user_root);
		std::string session_err;
		const std::string title = task->initial_session_title.empty() ?
		    "编程会话" :
		    task->initial_session_title;
		if (!session_store.update_after_run(task->session_id, title, "",
		        task->id, task->original_prompt, persisted_reply,
		        status == "cancelled" ? "cancelled" : "failed",
		        persisted_reasoning, "", persisted_duration_ms,
		        task->output.input_tokens,
		        task->output.cached_input_tokens,
		        task->output.output_tokens,
		        task->output.reasoning_tokens, session_err)) {
			webcool::ai::ai_log_error("agent.runtime",
			    "persist-terminal-session", session_err);
		}
	}
}

void set_runtime_result_artifact(
    const std::shared_ptr<agent_runtime_task_t> &task, const std::string &path,
    const std::string &decision)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	task->result_file = path;
	task->result_decision = decision;
}

void set_runtime_completion_summary(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &completion_summary)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	task->completion_summary = completion_summary;
}

void set_runtime_changes_applied(
    const std::shared_ptr<agent_runtime_task_t> &task, bool applied,
    const std::string &apply_error, const std::string &last_path,
    size_t mutation_count)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	const bool state_changed = task->changes_applied != applied ||
	    task->changes_apply_error != apply_error;
	task->changes_applied = applied;
	task->changes_apply_error = apply_error;
	if (applied && mutation_count > 0) {
		task->workspace_mutation_version += mutation_count;
		task->last_workspace_mutation_path = last_path;
	}
	if (state_changed) {
		// Status/SSE consumers must see the same decision as the persisted result;
		// otherwise another browser tab can recreate an already-consumed overlay.
		++task->staged_change_version;
		++task->event_version;
	}
}

void publish_runtime_staged_changes(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<agent_change_proposal_t> &changes)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->done || task->cancel_requested)
		return;
	// These are review-only snapshots. Publishing them increments the event
	// version so SSE clients can add virtual project-tree nodes immediately.
	task->changes = changes;
	++task->staged_change_version;
	++task->event_version;
}

void publish_runtime_change_reviews(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<webcool::ai::agent_change_review_t> & /* reviews */,
    const webcool::ai::agent_result_t &persisted)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	// Mirror the durable result wholesale. Besides avoiding divergent badges in
	// another browser, this also removes legacy intermediate generations that a
	// single latest-file decision superseded in the result store.
	task->changes = persisted.changes;
	task->result_decision = persisted.decision;
	task->changes_applied = persisted.changes_applied;
	++task->staged_change_version;
	++task->event_version;
}

} // namespace agent_detail
} // namespace action
