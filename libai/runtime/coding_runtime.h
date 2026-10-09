#pragma once
#include "../stdafx.h"

// Transport-independent coding runtime contract shared by library workers
// and host adapters. Authentication and HTTP responses belong to the host.
#include "../workspace/agent_change_limits.h"
#include "../validation/draft_cycle_tracker.h"
#include "../prompt/prompt_templates.h"
#include "../validation/proposal_validation.h"
#include "../agent/agent_registry.h"
#include "../agent/agent_protocol.h"
#include "../common/ai_error_log.h"
#include "../storage/agent_run_store.h"
#include "../storage/agent_result_store.h"
#include "../storage/agent_session_store.h"
#include "../storage/assistant_session_store.h"
#include "../storage/agent_checkpoint_store.h"
#include "../storage/agent_draft_store.h"
#include "../agent/agent_execution_mode.h"
#include "../workspace/agent_review_state.h"
#include "../agent/agent_run_scheduler.h"
#include "../storage/agent_progress_store.h"
#include "../agent/agent_progress_supervisor.h"
#include "../validation/verification_task.h"
#include "../context/agent_read_coverage.h"
#include "../validation/validation_test_evidence.h"
#include "../context/agent_context_window.h"
#include "../context/agent_staged_context.h"
#include "../context/repair_context_evidence.h"
#include "../context/repair_review_context.h"
#include "../context/repair_edit_evidence.h"
#include "../validation/repair_failure_tracker.h"
#include "../common/utf8_text.h"
#include "../context/text_read_page.h"
#include "../project/preview_project_context.h"
#include "../provider/output_token_limit.h"
#include "../storage/agent_request_store.h"
#include "../project/agent_project_store.h"
#include "../project/agent_project_index.h"
#include "../workspace/agent_workspace.h"
#include "../storage/agent_workflow_store.h"
#include "../workspace/workspace_change_set.h"
#include "../project/project_toolchain.h"
#include "../project/project_diagnostics.h"
#include "../project/project_plan_template.h"
#include "../provider/ai_provider_client.h"
#include "../provider/ai_provider_store.h"
#include "../agent/ai_admin_policy.h"
#include "../project/supported_languages.h"
#include "../common/webcool_mutex.h"

#include <openssl/rand.h>

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace action
{
namespace agent_detail
{

using webcool::ai::prompt_id;
using webcool::ai::path_is_in_project;
using webcool::ai::resolve_project_tool_path;
using webcool::ai::validate_change_proposals;
using webcool::ai::proposal_validation_error_t;
using webcool::ai::prompt_text;
using webcool::ai::prompt_with_value;

using webcool::ai::agent_change_proposal_t;
using webcool::ai::agent_tool_request_t;

const size_t kMaxInitialPromptBytes = 64 * 1024;
const size_t kMaxToolTranscriptBytes = webcool::ai::kMaxAgentTranscriptBytes;
const size_t kMaxToolResultBytes = 12 * 1024;
const size_t kMaxProtocolRepairAttempts = 2;
// Operation logs are rewritten atomically after each significant event. Keep
// enough headroom below agent_workspace_t's 1 MiB generated-text limit so the
// final stop/error event can always be persisted.
const size_t kMaxOperationLogBytes = 768 * 1024;

class operation_trace_t;
struct agent_runtime_task_t;
extern webcool::mutex g_agent_runtime_mutex;
extern std::map<std::string, std::shared_ptr<agent_runtime_task_t>>
	g_agent_runtime_tasks;
struct agent_tool_trace_t {
	// Metadata returned to the current browser session only. Tool result text is
	// deliberately absent so it cannot leak through run history or SSE events.
	std::string name;
	std::string path;
	std::string query;
	bool ok;
	bool truncated;
	bool native;
};

struct agent_runtime_task_t {
	std::string image_size;
	// In-memory live state keyed by user_root + run ID. Completed details expire
	// after one hour and are never serialized by agent_run_store_t.
	std::string id;
	std::string upload_root;
	std::string user_root;
	std::string username;
	// Admission control uses this scope to prevent two browser windows from
	// writing the same conversation or single-run recovery checkpoint at once.
	std::string project_path;
	// Terminal failures also persist their visible conversation turn. Keeping the
	// original request here lets finish_runtime_task save diagnostic reasoning.
	std::string original_prompt;
	std::string ui_language = "zh";
	std::string initial_session_title;
	bool cancel_requested = false;
	// Pause is cooperative: an in-flight provider response or atomic tool action
	// reaches the next safe boundary before the worker sleeps. Unlike cancellation,
	// no checkpoint, staged change or conversation state is discarded.
	bool pause_requested = false;
	std::string phase_before_pause;
	bool done = false;
	std::string status = "running";
	std::string phase = "queued";
	std::string current_tool;
	std::string streamed_text;
	webcool::ai::provider_stream_progress_t stream_progress;
	// Sensitive model reasoning follows the same transient lifecycle as the live
	// answer preview and is never written to the persistent run store.
	std::string streamed_reasoning;
	std::string completion_summary;
	std::string task_contract_json, task_acceptance_json,
		task_acceptance_status;
	size_t completed_tool_calls = 0;
	// Incremented only after a mutating tool has durably created a path. The
	// browser uses it to refresh the project tree while the run is still active.
	unsigned long long workspace_mutation_version = 0;
	// Monotonic version for review-only proposals. Unlike workspace mutation,
	// this never implies that a formal project path has changed.
	unsigned long long staged_change_version = 0;
	std::string last_workspace_mutation_path;
	size_t event_subscribers = 0;
	unsigned long long event_version = 1;
	std::string error;
	long long finished_at = 0;
	webcool::ai::completion_result_t output;
	std::vector<agent_tool_trace_t> traces;
	std::vector<agent_change_proposal_t> changes;
	size_t rejected_changes = 0;
	std::string result_file;
	std::string result_decision;
	bool changes_applied = false;
	std::string changes_apply_error;
	std::string session_id;
	bool remember_session = false;
	bool text_preview = false;
	std::string preview_context_directory;
	bool assistant_chat = false;
	std::string document_path;
	std::string conversation_id;
	webcool::ai::agent_run_scope_t scope;
	bool restart_recovery_enabled = false;
	bool resumed_after_restart = false;
	// Set when a failed run continues from the project-local progress JSON.
	bool resumed_from_progress = false;
	bool recovery_available = false;
	bool browser_debug_consent = false;
	bool browser_debug_confirmation_required = false;
	unsigned long browser_debug_retry_count = 0;
	std::string progress_file;
	// Opaque native Responses continuation state copied into each durable
	// checkpoint. It never crosses user/runtime scopes.
	std::string provider_response_id;
	bool provider_response_pending = false;
	std::vector<webcool::ai::completion_tool_output_t>
		provider_tool_outputs;
	// One JSONL file per run accompanies the optional reasoning text file. This
	// contains only operational metadata (never prompts, source, tool results or
	// credentials) and is updated while the model/tool loop is still running.
	webcool::mutex operation_log_mutex;
	std::string operation_log;
	bool operation_log_initialized = false;
	bool operation_log_truncated = false;
	// Capacity admission is separate from registration. Registered work can wait
	// fairly without consuming a provider request or being reported as failed.
	bool execution_admitted = false;
	unsigned long long queue_sequence = 0;
	// Handle of the ACL fiber performing provider I/O. Keeping it with the
	// per-user runtime task lets the cancel endpoint interrupt a fiber suspended
	// in DNS/connect/TLS/header/body I/O instead of waiting for the HTTP timeout.
	std::shared_ptr<acl::fiber> worker;
};

struct unavailable_validation_t {
	std::string fingerprint;
	std::string report;
	bool matches(const std::string &current) const
	{
		return !report.empty() && fingerprint == current;
	}
};
struct validation_cache_t {
	std::string draft_fingerprint;
	std::string fingerprint;
	std::string report;
};
struct batch_validation_evidence_t {
	std::string report, draft, baseline;
};

// Runtime helpers.
std::string runtime_task_key(const std::string &user_root,
			     const std::string &id);

bool register_runtime_task(const std::shared_ptr<agent_runtime_task_t> &task,
			   std::string &err);

bool wait_for_runtime_admission(
	const std::shared_ptr<agent_runtime_task_t> &task);

void remove_runtime_task(const std::string &user_root, const std::string &id);

bool runtime_cancel_requested(
	const std::shared_ptr<agent_runtime_task_t> &task);

size_t
runtime_completed_tool_count(const std::shared_ptr<agent_runtime_task_t> &task);

bool wait_while_runtime_paused(
	const std::shared_ptr<agent_runtime_task_t> &task);

bool request_runtime_pause(const std::string &user_root, const std::string &id,
			   bool paused, bool &already_done,
			   bool &pause_requested);

bool request_runtime_cancel(const std::string &user_root, const std::string &id,
			    bool &already_done);

void attach_runtime_worker(const std::shared_ptr<agent_runtime_task_t> &task,
			   const std::shared_ptr<acl::fiber> &worker);

void append_runtime_operation_events(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::vector<acl::json_node *> &events);

void append_runtime_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task,
	acl::json_node &event);

void append_simple_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
	const std::string &phase, const std::string &tool,
	size_t completed_tool_calls);

void append_model_operation_event(
	const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
	const webcool::ai::completion_result_t &output,
	const std::string &error = std::string(),
	long long effective_max_output_tokens = 0);

void update_runtime_progress(const std::shared_ptr<agent_runtime_task_t> &task,
			     const std::string &phase,
			     const std::string &current_tool,
			     size_t completed_tool_calls);

void complete_runtime_tool(const std::shared_ptr<agent_runtime_task_t> &task,
			   const agent_tool_trace_t &trace,
			   size_t completed_tool_calls);

void begin_runtime_model_stream(
	const std::shared_ptr<agent_runtime_task_t> &task);

std::string
runtime_reasoning_snapshot(const std::shared_ptr<agent_runtime_task_t> &task);

std::string
runtime_reasoning_for_save(const std::shared_ptr<agent_runtime_task_t> &task);

void mark_runtime_recovery_available(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &progress_file);

bool append_runtime_reasoning_delta(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &delta);

bool append_runtime_model_delta(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &delta);

unsigned long long
runtime_event_version(const std::shared_ptr<agent_runtime_task_t> &task);

unsigned long long runtime_staged_change_version(
	const std::shared_ptr<agent_runtime_task_t> &task);

bool begin_runtime_subscription(
	const std::shared_ptr<agent_runtime_task_t> &task);

std::shared_ptr<agent_runtime_task_t>
find_runtime_task(const std::string &user_root, const std::string &id);

void finish_runtime_task(const std::shared_ptr<agent_runtime_task_t> &task,
			 const std::string &status, const std::string &error,
			 const webcool::ai::completion_result_t *output,
			 const std::vector<agent_tool_trace_t> *traces,
			 const std::vector<agent_change_proposal_t> *changes,
			 size_t rejected_changes);

void set_runtime_result_artifact(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &path, const std::string &decision);

void set_runtime_completion_summary(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::string &completion_summary);

void set_runtime_changes_applied(
	const std::shared_ptr<agent_runtime_task_t> &task, bool applied,
	const std::string &apply_error, const std::string &last_path,
	size_t mutation_count);

void publish_runtime_staged_changes(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::vector<agent_change_proposal_t> &changes);

void publish_runtime_change_reviews(
	const std::shared_ptr<agent_runtime_task_t> &task,
	const std::vector<webcool::ai::agent_change_review_t> & /* reviews */,
	const webcool::ai::agent_result_t &persisted);

void attach_change_preview_diffs(
	std::vector<agent_change_proposal_t> &changes,
	const webcool::ai::workspace_change_set_preview_t &preview);

void add_run_record_json(acl::json_node &item,
			 const webcool::ai::agent_run_record_t &record);

void add_runtime_result_json(
	acl::json_node &root, const std::shared_ptr<agent_runtime_task_t> &task,
	const webcool::ai::agent_result_t *persisted = NULL,
	bool include_changes = true);

void add_durable_recovery_json(acl::json_node &root,
			       const std::string &user_root,
			       const webcool::ai::agent_run_record_t &record,
			       const std::string &session_hint);

void add_persisted_result_json(acl::json_node &root,
			       const webcool::ai::agent_result_t &result,
			       const std::string &relative_path);

// Context helpers.

bool local_project_location(const std::string &input, std::string &logical,
			    std::string &physical, std::string &err);

bool safe_git_project_directory(const std::string &physical);

bool load_temporary_attachments(
	acl::json_node *attachment_node, const std::string &draft,
	const std::string &user_root,
	std::vector<webcool::ai::completion_image_t> &images,
	std::string &text_context, std::string &err);

void remove_temporary_attachment_draft(const std::string &draft,
				       const std::string &user_root);

std::string new_run_id();

const webcool::ai::provider_config_t *
select_provider(const std::vector<webcool::ai::provider_config_t> &providers,
		const std::string &requested);

bool provider_is_deepseek(const webcool::ai::provider_config_t &provider);

bool provider_supports_thinking_disable(
	const webcool::ai::provider_config_t &provider);

bool provider_requires_extended_reasoning_budget(
	const webcool::ai::provider_config_t &provider);

bool compose_initial_prompt(
	webcool::ai::agent_workspace_t &workspace,
	const webcool::ai::provider_config_t &provider,
	const std::string &user_root, const std::string &project_path,
	const std::string &prompt, const std::string &current_session_id,
	const std::string &execution_mode, const std::string &previous_summary,
	std::string &initial_prompt, std::string &err, bool chinese,
	const webcool::ai::agent_project_record_t *selected_project = NULL,
	operation_trace_t *trace = NULL);

std::string deterministic_delivery_summary(
	const std::string &user_request,
	const std::vector<agent_change_proposal_t> &changes,
	const std::string &validation_state);

std::string
completion_summary_for_run(const std::string &provided,
			   const std::string &memory_summary,
			   const std::string &final_text,
			   const std::vector<agent_change_proposal_t> &changes,
			   const std::string &original_prompt);

// Tools helpers.
std::string tool_error_json(const std::string &error);

void collect_proposal_failure_causes(const std::string &result,
				     std::vector<std::string> &causes,
				     size_t depth = 0);

bool is_incremental_proposal_tool(const std::string &name);

bool request_contains_only_proposals(const agent_tool_request_t &request);

agent_tool_request_t merge_completion_tool_calls(
	const std::vector<webcool::ai::completion_tool_call_t> &calls);

bool build_native_tool_outputs(
	const std::vector<webcool::ai::completion_tool_call_t> &calls,
	const std::string &combined_result,
	std::vector<webcool::ai::completion_tool_output_t> &outputs);

bool completion_calls_have_ids(
	const std::vector<webcool::ai::completion_tool_call_t> &calls);

std::string annotate_read_coverage(const std::string &name,
				   const std::string &result,
				   webcool::ai::agent_read_coverage_t &coverage,
				   bool chinese);

void collect_observation_signatures(const std::string &name,
				    const std::string &result,
				    std::vector<std::string> &signatures);

bool repeated_repair_failure(const std::string &report,
			     const std::string &draft,
			     webcool::ai::repair_failure_tracker_t &tracker);

bool repair_review_read_only(const agent_tool_request_t &request);

void consume_repair_reads(const std::string &name, const std::string &result,
			  std::map<std::string, std::string> &required);

std::string
repair_review_required(const std::map<std::string, std::string> &required,
		       bool chinese);

std::string batch_read_continuation(const std::string &name,
				    const std::string &result);
bool remember_read_result(const std::string &name, const std::string &result,
			  webcool::ai::agent_read_context_t &context);

bool read_result_is_retained(const std::string &name, const std::string &result,
			     const webcool::ai::agent_read_context_t &context);

void restore_read_context(const std::string &transcript,
			  webcool::ai::agent_read_context_t &context);

bool project_path_to_draft_path(const std::string &project_path,
				const std::string &path,
				std::string &draft_path);

std::string
validation_baseline_fingerprint(webcool::ai::agent_workspace_t &workspace,
				const std::string &project_path);

std::string successful_validation_summary(const std::string &report,
					  bool chinese);

std::string execute_workspace_tool(
	webcool::ai::agent_workspace_t &workspace, const std::string &user_root,
	const std::string &project_path, bool allow_file_content,
	const agent_tool_request_t &request, agent_tool_trace_t &trace,
	std::vector<agent_change_proposal_t> *staged_changes,
	const std::string &run_id,
	const webcool::ai::sandbox_limits_t &sandbox_limits, bool chinese,
	const unavailable_validation_t *unavailable_validation = NULL,
	validation_cache_t *validation_cache = NULL,
	std::string *saved_proposal_batch = NULL,
	batch_validation_evidence_t *batch_validation = NULL);

// Drafts helpers.
bool verify_result_changes_applied(const std::string &user_root,
				   const webcool::ai::agent_result_t &result,
				   std::string &err);

bool workspace_change_matches(
	const std::string &user_root,
	const webcool::ai::workspace_change_input_t &change, bool &matches,
	std::string &err);

bool workspace_path_is_below(const std::string &path,
			     const std::string &directory);

std::string persistent_draft_root(const std::string &user_root,
				  const std::string &project_path,
				  const std::string &run_id);

bool materialize_staged_worktree(
	const std::string &user_root, const std::string &project_path,
	const std::string &run_id,
	const std::vector<agent_change_proposal_t> &changes,
	size_t &skipped_files, std::string &err);

bool synchronize_live_review_state(
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	const std::string &project_path,
	std::vector<agent_change_proposal_t> &active_changes,
	size_t &resolved_count, std::string &err);

bool remove_persistent_worktree(const std::string &user_root,
				const std::string &project_path,
				const std::string &run_id, std::string &err);

bool schedule_reviewed_worktree_cleanup(const std::string &user_root,
					const std::string &project_path,
					const std::string &run_id,
					std::string &err);

std::string validate_staged_draft(
	const std::string &user_root, const std::string &project_path,
	const std::string &run_id,
	const std::vector<agent_change_proposal_t> &changes,
	const webcool::ai::sandbox_limits_t &limits, agent_tool_trace_t &trace,
	std::string *source_fingerprint = NULL, bool build_only = false);

std::string
staged_change_fingerprint(const std::vector<agent_change_proposal_t> &changes);

std::string
staged_cycle_fingerprint(const std::vector<agent_change_proposal_t> &changes);

// Loop helpers.
bool load_matching_coding_progress(
	webcool::ai::agent_progress_store_t &store,
	const webcool::ai::provider_config_t &provider,
	const std::string &project_path, const std::string &original_prompt,
	std::string &initial_prompt, std::string &recovered_reasoning,
	size_t &completed_tool_calls, bool &resumed, std::string &err);

bool run_coding_tool_loop(
	const webcool::ai::provider_config_t &provider,
	const std::string &api_key, webcool::ai::agent_workspace_t &workspace,
	const std::string &project_path, const std::string &original_prompt,
	const std::string &initial_prompt,
	const std::string &recovered_reasoning,
	const std::vector<webcool::ai::completion_image_t> &request_images,
	size_t initial_completed_tool_calls, size_t max_tool_calls,
	long long max_output_tokens, size_t max_no_progress_tool_calls,
	size_t tool_context_compaction_bytes,
	const webcool::ai::sandbox_limits_t &sandbox_limits,
	const std::string &thinking_mode, const std::string &reasoning_effort,
	webcool::ai::completion_result_t &final_output,
	std::vector<agent_tool_trace_t> &traces,
	std::vector<agent_change_proposal_t> &changes,
	std::string &memory_summary, std::string &completion_summary,
	std::string &session_title, size_t &rejected_changes,
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	std::string &err);

// Worker helpers.
bool run_assistant_image_task(const std::shared_ptr<agent_runtime_task_t> &task,
			      const webcool::ai::provider_config_t &provider,
			      const std::string &api_key,
			      webcool::ai::completion_result_t &output,
			      std::string &err);

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
	const std::string &thinking_mode, const std::string &reasoning_effort);

std::shared_ptr<agent_runtime_task_t>
recover_runtime_task(const std::string &upload_root,
		     const std::string &user_root, const std::string &username,
		     const webcool::ai::agent_run_record_t &record,
		     std::string &err);

// Common helpers.

bool load_review_run_record(const std::string &user_root,
			    const std::string &run_id,
			    const std::string &session_id,
			    webcool::ai::agent_run_record_t &record,
			    std::string &err);

bool reviewable_agent_run_status(const std::string &status);

std::string json_text(acl::json_node *node);

long long json_number(acl::json_node *node, long long fallback);

bool json_bool(acl::json_node *node, bool fallback);

acl::json_node *json_array_node(acl::json_node *node);

bool parse_string_array(acl::json_node *node, size_t limit,
			std::vector<std::string> &values);

std::string serialize_json(acl::json_node &root);

// CPU percentages are interval averages, not sampled peaks. Thread time includes
// other fibers on the same OS thread; process time includes concurrent requests.
struct operation_cpu_snapshot_t {
	std::chrono::steady_clock::time_point wall =
		std::chrono::steady_clock::now();
	double thread_ms = -1, process_ms = -1;
	operation_cpu_snapshot_t()
	{
#if defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_PROCESS_CPUTIME_ID)
		struct timespec ts;
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
			thread_ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
		if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
			process_ms =
				ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
	}
	void add_delta(acl::json_node &node,
		       const operation_cpu_snapshot_t &end) const
	{
		const double elapsed =
			std::chrono::duration<double, std::milli>(end.wall -
								  wall)
				.count();
		const double thread = thread_ms < 0 || end.thread_ms < 0 ?
					      -1 :
					      end.thread_ms - thread_ms;
		const double process = process_ms < 0 || end.process_ms < 0 ?
					       -1 :
					       end.process_ms - process_ms;
		node.add_number("elapsed_ms", (long long)elapsed);
		node.add_number("thread_cpu_ms", (long long)thread);
		node.add_number("process_cpu_ms", (long long)process);
		node.add_number("thread_cpu_average_percent",
				thread < 0 || elapsed <= 0 ?
					-1 :
					(long long)(thread * 100 / elapsed));
		node.add_number("process_cpu_average_percent",
				process < 0 || elapsed <= 0 ?
					-1 :
					(long long)(process * 100 / elapsed));
	}
};

// Buffer stages and persist once; never add per-file timing writes to review.
// An early error is logged too, with the last active phase and completed=false.
class operation_trace_t {
	struct stage_t {
		std::string name;
		operation_cpu_snapshot_t begin, end;
	};
	std::string operation_, project_id_, phase_ = "authenticate_and_parse";
	operation_cpu_snapshot_t started_, phase_started_;
	std::vector<stage_t> stages_;
	std::shared_ptr<agent_runtime_task_t> task_;
	bool completed_ = false, finished_ = false;

public:
	explicit operation_trace_t(const char *operation)
		: operation_(operation)
	{
	}
	void bind(const std::shared_ptr<agent_runtime_task_t> &task)
	{
		task_ = task;
	}
	void phase(const char *name)
	{
		operation_cpu_snapshot_t now;
		stages_.push_back({ phase_, phase_started_, now });
		phase_ = name;
		phase_started_ = now;
	}
	void project_id(const std::string &id)
	{
		project_id_ = id;
	}
	void complete()
	{
		completed_ = true;
	}
	~operation_trace_t()
	{
		finish();
	}
	void finish()
	{
		if (finished_)
			return;
		finished_ = true;
		try {
			phase("finished");
			acl::json json;
			auto &event = json.create_node();
			event.add_text("event", "operation_performance");
			event.add_text("operation", operation_.c_str());
			if (!project_id_.empty())
				event.add_text("project_id",
					       project_id_.c_str());
			event.add_bool("completed", completed_);
			started_.add_delta(event, phase_started_);
			auto &stages = json.create_array();
			event.add_child("stages", stages);
			for (const auto &stage : stages_) {
				auto &item = stages.add_child(false, true);
				item.add_text("phase", stage.name.c_str());
				stage.begin.add_delta(item, stage.end);
			}
			// Server log also covers requests rejected before a run is created,
			// and archived reviews without a live runtime task.
			if (task_)
				event.add_text("trace_run_id",
					       task_->id.c_str());
			logger("agent.performance %s",
			       serialize_json(event).c_str());
			append_runtime_operation_event(task_, event);
		} catch (...) {
			logger_error("agent.performance trace failed");
		}
	}
};

} // namespace agent_detail
} // namespace action
