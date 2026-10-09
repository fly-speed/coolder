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

std::string runtime_task_key(const std::string &user_root,
			     const std::string &id)
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
			 std::shared_ptr<agent_runtime_task_t>>::iterator
			oldest = g_agent_runtime_tasks.end();
		for (std::map<std::string,
			      std::shared_ptr<agent_runtime_task_t>>::iterator
			     it = g_agent_runtime_tasks.begin();
		     it != g_agent_runtime_tasks.end(); ++it) {
			if (!it->second->done)
				continue;
			++completed;
			if (oldest == g_agent_runtime_tasks.end() ||
			    it->second->finished_at <
				    oldest->second->finished_at)
				oldest = it;
		}
		if (completed <= kMaxCompletedRuntimeResults ||
		    oldest == g_agent_runtime_tasks.end())
			break;
		g_agent_runtime_tasks.erase(oldest);
	}
}

bool register_runtime_task(const std::shared_ptr<agent_runtime_task_t> &task,
			   std::string &err)
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
			webcool::ai::normalize_agent_resource_path(
				actual_project);
	}
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	cleanup_runtime_tasks_locked(static_cast<long long>(time(NULL)));
	const std::string key = runtime_task_key(task->user_root, task->id);
	if (g_agent_runtime_tasks.find(key) != g_agent_runtime_tasks.end()) {
		err = "agent run is already active";
		webcool::ai::ai_log_error("agent.runtime", "register-duplicate",
					  err);
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
		const bool same_saved_session =
			!task->session_id.empty() &&
			it->second->session_id == task->session_id;
		if (webcool::ai::agent_run_scopes_conflict(task->scope,
							   it->second->scope)) {
			err = task->text_preview ?
				      (task->assistant_chat ?
					       "this assistant conversation already has an active run" :
					       "this document already has an active AI run") :
				      (same_saved_session ?
					       "this agent session already has an active run" :
					       "this project already has an active agent run");
			webcool::ai::ai_log_error("agent.runtime",
						  "register-scope-conflict",
						  err);
			return false;
		}
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
		webcool::ai::ai_log_error("agent.runtime", "register-capacity",
					  err);
		return false;
	}
	task->queue_sequence = g_next_agent_queue_sequence++;
	g_agent_runtime_tasks[key] = task;
	return true;
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
			if (!task->pause_requested) {
				std::vector<webcool::ai::agent_run_slot_t>
					slots;
				slots.reserve(g_agent_runtime_tasks.size());
				for (std::map<std::string,
					      std::shared_ptr<
						      agent_runtime_task_t>>::
					     const_iterator it =
						     g_agent_runtime_tasks
							     .begin();
				     it != g_agent_runtime_tasks.end(); ++it) {
					webcool::ai::agent_run_slot_t slot;
					slot.key = it->first;
					slot.user_scope = it->second->user_root;
					slot.sequence =
						it->second->queue_sequence;
					slot.admitted =
						it->second->execution_admitted;
					slot.done = it->second->done;
					slot.cancelled =
						it->second->cancel_requested;
					slot.paused =
						it->second->pause_requested;
					slots.push_back(slot);
				}
				const size_t user_limit = static_cast<size_t>(
					webcool::ai::ai_runtime_policy_get()
						.max_active_runs_per_user);
				const std::string selected =
					webcool::ai::select_next_agent_run(
						slots, kMaxActiveAgentRuns,
						user_limit);
				if (selected ==
				    runtime_task_key(task->user_root,
						     task->id)) {
					task->execution_admitted = true;
					task->phase = "preparing";
					++task->event_version;
					return true;
				}
			}
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

size_t
runtime_completed_tool_count(const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return 0;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->completed_tool_calls;
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
				if (entered_pause || task->phase == "paused" ||
				    task->phase == "pausing" ||
				    task->phase == "resuming") {
					task->phase =
						task->phase_before_pause
								.empty() ?
							"model_call" :
							task->phase_before_pause;
					task->phase_before_pause.clear();
					++task->event_version;
				}
				return true;
			}
			entered_pause = true;
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
		// Agent workers run as ACL fibers, so this yields to other users rather
		// than occupying an OS worker thread while the task is paused.
		acl::fiber::delay(100);
	}
}

bool request_runtime_pause(const std::string &user_root, const std::string &id,
			   bool paused, bool &already_done,
			   bool &pause_requested)
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

bool request_runtime_cancel(const std::string &user_root, const std::string &id,
			    bool &already_done)
{
	std::shared_ptr<acl::fiber> worker;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		const std::map<std::string,
			       std::shared_ptr<agent_runtime_task_t>>::iterator
			it = g_agent_runtime_tasks.find(
				runtime_task_key(user_root, id));
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
	if (worker)
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

std::string
operation_log_path(const std::shared_ptr<agent_runtime_task_t> &task)
{
	const std::string filename = "ai-operations-" + task->id + ".jsonl";
	return task->project_path.empty() ?
		       ".webcool_agent/" + filename :
		       task->project_path + "/.webcool_agent/" + filename;
}

std::string sanitize_operation_detail(const std::string &input)
{
	// Provider diagnostics are valuable, but some gateways echo bearer tokens or
	// provider-specific sk-/ak- keys inside an error. Redact those token-shaped
	// substrings before a diagnostic leaves memory.
	std::string output;
	output.reserve(std::min<size_t>(input.size(), 2048));
	for (size_t i = 0; i < input.size() && output.size() < 2048;) {
		size_t prefix_length = 0;
		const bool bearer = input.compare(i, 7, "Bearer ") == 0;
		const bool key_prefix =
			(i == 0 || !std::isalnum(static_cast<unsigned char>(
					   input[i - 1]))) &&
			(input.compare(i, 3, "sk-") == 0 ||
			 input.compare(i, 3, "ak-") == 0);
		if (bearer)
			prefix_length = 7;
		else if (key_prefix)
			prefix_length = 3;
		size_t token_end = i + prefix_length;
		while (prefix_length != 0 && token_end < input.size()) {
			const unsigned char c =
				static_cast<unsigned char>(input[token_end]);
			if (std::isspace(c) || c == ']' || c == '>' ||
			    c == '"' || c == '\'')
				break;
			++token_end;
		}
		// Avoid mistaking ordinary words ending in "sk-" for a credential.
		if (key_prefix && token_end - (i + prefix_length) < 8) {
			prefix_length = 0;
		}
		if (prefix_length == 0) {
			const unsigned char c =
				static_cast<unsigned char>(input[i++]);
			output.push_back(c < 0x20 || c == 0x7f ?
						 ' ' :
						 static_cast<char>(c));
			continue;
		}
		output += "[redacted]";
		i = token_end;
	}
	return output;
}

void append_runtime_operation_events(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::vector<acl::json_node *> &events)
{
	if (!task)
		return;
	// The run ID and timestamp make lines independently searchable after several
	// users run agents concurrently. The filename is derived only from the
	// server-generated run ID and the already validated project path.
	const long long timestamp_ms = static_cast<long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
	std::string line;
	for (auto *event : events) {
		event->add_text("run_id", task->id.c_str());
		event->add_number("timestamp_ms", timestamp_ms);
		line += serialize_json(*event) + "\n";
	}

	std::lock_guard<webcool::mutex> guard(task->operation_log_mutex);
	if (!task->operation_log_initialized) {
		task->operation_log_initialized = true;
		if (task->resumed_after_restart) {
			webcool::ai::agent_workspace_t workspace(
				task->user_root);
			std::string existing;
			bool truncated = false;
			std::string load_err;
			webcool::ai::agent_request_store_t log_store(
				task->user_root, task->project_path, task->id);
			bool loaded = log_store.operation_log(existing, false,
							      load_err);
			if (!loaded) {
				const std::string legacy =
					(task->project_path.empty() ?
						 "" :
						 task->project_path + "/") +
					"ai-operations-" + task->id + ".jsonl";
				loaded = workspace.read(legacy, existing,
							truncated, load_err);
			}
			if (loaded && !truncated) {
				task->operation_log = existing;
			} else if (!load_err.empty() &&
				   load_err.find("does not exist") ==
					   std::string::npos) {
				webcool::ai::ai_log_error(
					"agent.operation-log",
					"load-before-recovery", load_err);
			}
		}
	}
	if (task->operation_log_truncated)
		return;
	const bool creates_log_file = task->operation_log.empty();
	if (task->operation_log.size() + line.size() > kMaxOperationLogBytes) {
		acl::json truncated_json;
		acl::json_node &truncated = truncated_json.create_node();
		truncated.add_text("event", "operation_log_truncated");
		truncated.add_text("run_id", task->id.c_str());
		truncated.add_number("timestamp_ms", timestamp_ms);
		truncated.add_number(
			"retained_bytes",
			static_cast<long long>(task->operation_log.size()));
		line = serialize_json(truncated) + "\n";
		task->operation_log_truncated = true;
	}
	task->operation_log += line;

	webcool::ai::agent_request_store_t log_store(
		task->user_root, task->project_path, task->id);
	std::string err;
	const std::string relative_path = operation_log_path(task);
	if (!log_store.operation_log(task->operation_log, true, err)) {
		// Do not recursively add a failed-log-write event to the same file.
		webcool::ai::ai_log_error("agent.operation-log", "persist",
					  err);
	} else if (creates_log_file) {
		// Publish only the creation as a tree mutation. Subsequent appends need not
		// force a full project-tree refresh for every model delta/tool boundary.
		std::lock_guard<webcool::mutex> runtime_guard(
			g_agent_runtime_mutex);
		++task->workspace_mutation_version;
		task->last_workspace_mutation_path = relative_path;
		++task->event_version;
	}
}

void append_runtime_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task,
	acl::json_node &event)
{
	append_runtime_operation_events(task, { &event });
}

void append_simple_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
	const std::string &phase, const std::string &tool,
	size_t completed_tool_calls)
{
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", name);
	if (!phase.empty())
		event.add_text("phase", phase.c_str());
	if (!tool.empty())
		event.add_text("tool", tool.c_str());
	event.add_number("completed_tool_calls",
			 static_cast<long long>(completed_tool_calls));
	append_runtime_operation_event(task, event);
}

void append_model_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
	const webcool::ai::completion_result_t &output,
	const std::string &error, long long effective_max_output_tokens)
{
	acl::json json;
	acl::json_node &event = json.create_node();
	event.add_text("event", name);
	event.add_number("latency_ms", output.latency_ms);
	event.add_number("input_tokens", output.input_tokens);
	event.add_number("cached_input_tokens", output.cached_input_tokens);
	event.add_number("output_tokens", output.output_tokens);
	event.add_number("reasoning_tokens", output.reasoning_tokens);
	if (effective_max_output_tokens > 0)
		event.add_number("effective_max_output_tokens",
				 effective_max_output_tokens);
	event.add_number("visible_text_bytes",
			 static_cast<long long>(output.text.size()));
	event.add_number("reasoning_bytes",
			 static_cast<long long>(output.reasoning.size()));
	size_t tool_content_bytes = 0;
	for (const auto &call : output.tool_calls)
		tool_content_bytes += call.content.size();
	event.add_number("tool_content_bytes",
			 static_cast<long long>(tool_content_bytes));
	event.add_bool("tool_arguments_recovery_attempted",
		       output.tool_arguments_recovery_attempted);
	event.add_bool("verbose_tool_preamble",
		       !output.tool_calls.empty() && output.text.size() > 4096);
	event.add_number("tool_call_count",
			 static_cast<long long>(output.tool_calls.size()));
	event.add_text("provider_error_category",
		       webcool::ai::provider_client_t::error_category_name(
			       output.error_category));
	event.add_bool("retryable", output.retryable_error);
	event.add_bool("transport_retry_attempted",
		       output.transport_retry_attempted);
	event.add_bool("connection_reused", output.connection_reused);
	event.add_bool("native_tool_call", output.native_tool_call);
	event.add_bool("reasoning_budget_recovered",
		       output.reasoning_budget_recovered);
	if (output.http_status > 0) {
		event.add_number("http_status", output.http_status);
	}
	if (!output.response_status.empty()) {
		event.add_text("response_status",
			       output.response_status.c_str());
	}
	if (!output.incomplete_reason.empty()) {
		event.add_text(
			"incomplete_reason",
			sanitize_operation_detail(output.incomplete_reason)
				.c_str());
	}
	if (!error.empty()) {
		event.add_text("error",
			       sanitize_operation_detail(error).c_str());
		if (!output.error_response_excerpt.empty()) {
			event.add_text("error_response_excerpt",
				       sanitize_operation_detail(
					       output.error_response_excerpt)
					       .c_str());
			event.add_bool("error_response_excerpt_truncated",
				       output.error_response_excerpt.size() >=
					       2048);
		}
		if (!output.error_response_content_type.empty())
			event.add_text(
				"error_response_content_type",
				sanitize_operation_detail(
					output.error_response_content_type)
					.c_str());
		if (!output.error_response_content_encoding.empty())
			event.add_text(
				"error_response_content_encoding",
				sanitize_operation_detail(
					output.error_response_content_encoding)
					.c_str());
		event.add_text("session_id", task->session_id.c_str());
		event.add_number("completed_tool_calls",
				 runtime_completed_tool_count(task));
	}
	append_runtime_operation_event(task, event);
}

void update_runtime_progress(const std::shared_ptr<agent_runtime_task_t> &task,
			     const std::string &phase,
			     const std::string &current_tool,
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
	append_simple_operation_event(task, "phase_changed", phase,
				      current_tool, completed_tool_calls);
}

void complete_runtime_tool(const std::shared_ptr<agent_runtime_task_t> &task,
			   const agent_tool_trace_t &trace,
			   size_t completed_tool_calls)
{
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (task->done || task->cancel_requested)
			return;
		task->phase = "model_call";
		task->current_tool.clear();
		task->completed_tool_calls = completed_tool_calls;
		if (trace.ok && (trace.name == "workspace.create" ||
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

std::string
runtime_reasoning_snapshot(const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->streamed_reasoning;
}

std::string
runtime_reasoning_for_save(const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	// Non-streaming compatible providers may expose reasoning only in the final
	// completion object. Prefer it after completion; otherwise save the live
	// accumulated stream visible in the reasoning window.
	if (task->done && !task->output.reasoning.empty()) {
		return task->output.reasoning;
	}
	return task->streamed_reasoning;
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
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &delta)
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
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &delta)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	if (task->done || task->cancel_requested)
		return false;
	// The transient preview has the same one-hour lifecycle as the completed
	// reply and a stricter 1 MiB memory bound. It is never persisted or logged.
	const size_t limit = 1024 * 1024;
	if (task->streamed_text.size() < limit) {
		const size_t keep = std::min(limit - task->streamed_text.size(),
					     delta.size());
		task->streamed_text.append(delta, 0, keep);
	}
	++task->event_version;
	return true;
}

unsigned long long
runtime_event_version(const std::shared_ptr<agent_runtime_task_t> &task)
{
	if (!task)
		return 0;
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	return task->event_version;
}

unsigned long long
runtime_staged_change_version(const std::shared_ptr<agent_runtime_task_t> &task)
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

std::shared_ptr<agent_runtime_task_t>
find_runtime_task(const std::string &user_root, const std::string &id)
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
			message.text =
				status == "completed" ?
					result.text :
					(!error.empty() ? error :
							  "生成已停止。");
			message.input = result.input_tokens;
			message.output = result.output_tokens;
			message.duration = result.latency_ms;
		}
		std::string save_err;
		if (!webcool::ai::assistant_session_store_t(task->user_root)
			     .append(task->conversation_id, message,
				     save_err)) {
			webcool::ai::ai_log_error("assistant", "persist-reply",
						  save_err);
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
	finished.add_number("proposed_changes",
			    static_cast<long long>(task->changes.size()));
	finished.add_number("rejected_changes",
			    static_cast<long long>(rejected_changes));
	finished.add_number("latency_ms", task->output.latency_ms);
	finished.add_number("input_tokens", task->output.input_tokens);
	finished.add_number("cached_input_tokens",
			    task->output.cached_input_tokens);
	finished.add_number("output_tokens", task->output.output_tokens);
	finished.add_number("reasoning_tokens", task->output.reasoning_tokens);
	if (!error.empty()) {
		finished.add_text("error",
				  sanitize_operation_detail(error).c_str());
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
		if (!session_store.update_after_run(
			    task->session_id, title, "", task->id,
			    task->original_prompt, persisted_reply,
			    status == "cancelled" ? "cancelled" : "failed",
			    persisted_reasoning, "", persisted_duration_ms,
			    task->output.input_tokens,
			    task->output.cached_input_tokens,
			    task->output.output_tokens,
			    task->output.reasoning_tokens, session_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "persist-terminal-session",
						  session_err);
		}
	}
}

void set_runtime_result_artifact(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &path, const std::string &decision)
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

void attach_change_preview_diffs(
	std::vector<agent_change_proposal_t> &changes,
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

void add_run_record_json(acl::json_node &item,
			 const webcool::ai::agent_run_record_t &record)
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
		item.add_number("provider_http_status",
				record.provider_http_status);
	}
	item.add_bool("provider_error_retryable",
		      record.provider_error_retryable);
	if (!record.error.empty())
		item.add_text("error", record.error.c_str());
}

void add_runtime_result_json(acl::json_node &root,
			     const std::shared_ptr<agent_runtime_task_t> &task,
			     const webcool::ai::agent_result_t *persisted,
			     bool include_changes)
{
	if (!task) {
		root.add_bool("result_available", false);
		return;
	}
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	root.add_bool("cancel_requested", task->cancel_requested);
	root.add_bool("pause_requested", task->pause_requested);
	root.add_bool("paused", task->phase == "paused");
	root.add_bool("result_available",
		      task->done && task->status == "completed");
	root.add_text("phase", task->phase.c_str());
	root.add_bool("provider_response_pending",
		      task->provider_response_pending);
	if (!task->provider_response_id.empty()) {
		root.add_text("provider_response_id",
			      task->provider_response_id.c_str());
	}
	root.add_number("event_version",
			static_cast<long long>(task->event_version));
	root.add_number("completed_tool_calls",
			static_cast<long long>(task->completed_tool_calls));
	root.add_number(
		"workspace_mutation_version",
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
	root.add_bool("restart_recovery_enabled",
		      task->restart_recovery_enabled);
	root.add_bool("resumed_after_restart", task->resumed_after_restart);
	root.add_bool("resumed_from_progress", task->resumed_from_progress);
	root.add_bool("recovery_available", task->recovery_available);
	root.add_bool("browser_debug_confirmation_required",
		      task->browser_debug_confirmation_required);
	root.add_number(
		"browser_debug_retry_count",
		static_cast<long long>(task->browser_debug_retry_count));
	root.add_bool("result_persisted", !task->result_file.empty());
	root.add_bool("changes_applied", persisted ?
						 persisted->changes_applied :
						 task->changes_applied);
	if (!task->changes_apply_error.empty()) {
		root.add_text("changes_apply_error",
			      task->changes_apply_error.c_str());
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
	root.add_number("cached_input_tokens",
			task->output.cached_input_tokens);
	root.add_number("output_tokens", task->output.output_tokens);
	root.add_number("reasoning_tokens", task->output.reasoning_tokens);
	root.add_number("latency_ms", task->output.latency_ms);
	root.add_text("provider_error_category",
		      webcool::ai::provider_client_t::error_category_name(
			      task->output.error_category));
	root.add_bool("provider_error_retryable", task->output.retryable_error);
	if (task->output.http_status > 0) {
		root.add_number("provider_http_status",
				task->output.http_status);
	}
	if (!task->current_tool.empty()) {
		root.add_text("current_tool", task->current_tool.c_str());
	}
	acl::json_node &stream_progress =
		root.add_child("stream_progress", true);
	stream_progress.add_text("phase", task->stream_progress.phase.c_str());
	stream_progress.add_number("elapsed_ms",
				   task->stream_progress.elapsed_ms);
	stream_progress.add_number("first_byte_ms",
				   task->stream_progress.first_byte_ms);
	stream_progress.add_number("last_effective_ms",
				   task->stream_progress.last_effective_ms);
	stream_progress.add_number(
		"received_bytes",
		static_cast<long long>(task->stream_progress.received_bytes));
	stream_progress.add_number(
		"tool_argument_bytes",
		static_cast<long long>(
			task->stream_progress.tool_argument_bytes));
	if (!task->streamed_text.empty()) {
		root.add_text("streamed_text", task->streamed_text.c_str());
	}
	if (!task->streamed_reasoning.empty() &&
	    (!task->done || task->status != "completed" ||
	     task->output.reasoning.empty())) {
		root.add_text("streamed_reasoning",
			      task->streamed_reasoning.c_str());
	}
	if (!task->completion_summary.empty()) {
		root.add_text("completion_summary",
			      task->completion_summary.c_str());
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
			item.add_text("operation",
				      visible_changes[i].operation.c_str());
			item.add_text("path", visible_changes[i].path.c_str());
			if (!visible_changes[i].target_path.empty()) {
				item.add_text(
					"target_path",
					visible_changes[i].target_path.c_str());
			}
			item.add_text("content",
				      visible_changes[i].content.c_str());
			item.add_text("reason",
				      visible_changes[i].reason.c_str());
			item.add_bool("creates_file",
				      visible_changes[i].creates_file);
			item.add_bool("creates_directory",
				      visible_changes[i].creates_directory);
			item.add_number("added_lines",
					visible_changes[i].added_lines);
			item.add_number("removed_lines",
					visible_changes[i].removed_lines);
			if (!visible_changes[i].diff.empty()) {
				item.add_text("diff",
					      visible_changes[i].diff.c_str());
			}
			item.add_text(
				"original_content",
				visible_changes[i].original_content.c_str());
			item.add_bool(
				"original_content_available",
				visible_changes[i].original_content_available);
			item.add_number("generation",
					static_cast<long long>(
						visible_changes[i].generation));
			item.add_text("base_hash",
				      visible_changes[i].base_hash.c_str());
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
void add_durable_recovery_json(acl::json_node &root,
			       const std::string &user_root,
			       const webcool::ai::agent_run_record_t &record,
			       const std::string &session_hint)
{
	if (record.error != "agent run interrupted by service restart")
		return;
	webcool::ai::agent_session_store_t session_store(user_root);
	std::string err;
	if (!session_hint.empty()) {
		webcool::ai::agent_session_record_t hinted;
		if (session_store.get(session_hint, hinted, err) &&
		    hinted.project_path == record.project_path) {
			bool found = false;
			webcool::ai::agent_progress_store_t progress_store(
				user_root, hinted.project_path, hinted.id);
			if (progress_store.exists(found, err)) {
				root.add_bool("recovery_available", found);
				root.add_text("session_id", hinted.id.c_str());
				if (found)
					root.add_text(
						"progress_file",
						progress_store.relative_path()
							.c_str());
				return;
			}
			webcool::ai::ai_log_error("agent.run",
						  "probe-hinted-progress", err);
		}
	}
	std::vector<webcool::ai::agent_session_record_t> sessions;
	if (!session_store.list(50, sessions, err)) {
		webcool::ai::ai_log_error("agent.run", "list-restart-sessions",
					  err);
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
		if (found) {
			root.add_text("progress_file",
				      progress_store.relative_path().c_str());
		}
		return;
	}
	root.add_bool("recovery_available", false);
}

void add_persisted_result_json(acl::json_node &root,
			       const webcool::ai::agent_result_t &result,
			       const std::string &relative_path)
{
	root.add_bool("result_available", true);
	root.add_bool("result_persisted", true);
	root.add_text("result_file", relative_path.c_str());
	root.add_text("result_decision", result.decision.c_str());
	root.add_bool("changes_applied", result.changes_applied);
	root.add_number("changes_applied_at", result.changes_applied_at);
	if (!result.changes_apply_error.empty()) {
		root.add_text("changes_apply_error",
			      result.changes_apply_error.c_str());
	}
	root.add_text("task_contract_json", result.task_contract_json.c_str());
	root.add_text("task_acceptance_json",
		      result.task_acceptance_json.c_str());
	root.add_text("task_acceptance_status",
		      result.task_acceptance_status.c_str());
	root.add_text("text", result.text.c_str());
	if (!result.reasoning.empty()) {
		root.add_text("reasoning", result.reasoning.c_str());
	}
	if (!result.completion_summary.empty()) {
		root.add_text("completion_summary",
			      result.completion_summary.c_str());
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
		item.add_bool("creates_directory",
			      result.changes[i].creates_directory);
		item.add_number("added_lines", result.changes[i].added_lines);
		item.add_number("removed_lines",
				result.changes[i].removed_lines);
		if (!result.changes[i].diff.empty()) {
			item.add_text("diff", result.changes[i].diff.c_str());
		}
		item.add_text("original_content",
			      result.changes[i].original_content.c_str());
		item.add_bool("original_content_available",
			      result.changes[i].original_content_available);
		item.add_number(
			"generation",
			static_cast<long long>(result.changes[i].generation));
		item.add_text("base_hash", result.changes[i].base_hash.c_str());
		item.add_text("draft_hash",
			      result.changes[i].draft_hash.c_str());
		item.add_text("review_status",
			      result.changes[i].review_status.c_str());
	}
}

} // namespace agent_detail
} // namespace action
