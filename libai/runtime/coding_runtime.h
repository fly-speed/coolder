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

// Upper bound for initial prompt bytes.
const size_t kMaxInitialPromptBytes = 64 * 1024;
// Upper bound for tool transcript bytes.
const size_t kMaxToolTranscriptBytes = webcool::ai::kMaxAgentTranscriptBytes;
// Upper bound for tool result bytes.
const size_t kMaxToolResultBytes = 12 * 1024;
// Upper bound for protocol repair attempts.
const size_t kMaxProtocolRepairAttempts = 2;
// Operation logs are rewritten atomically after each significant event. Keep
// enough headroom below agent_workspace_t's 1 MiB generated-text limit so the
// final stop/error event can always be persisted.
const size_t kMaxOperationLogBytes = 768 * 1024;

// Scope-bound operation timing and metadata reporting for one agent run.
class operation_trace_t;
// Shared live state of an agent run, coordinated by the runtime mutex.
struct agent_runtime_task_t;
// Protects shared live agent tasks and their admission/status state.
extern webcool::mutex g_agent_runtime_mutex;
// Live agent tasks keyed by authenticated user and run identifier.
extern std::map<std::string, std::shared_ptr<agent_runtime_task_t>>
    g_agent_runtime_tasks;
// Bounded metadata describing one executed workspace tool call.
struct agent_tool_trace_t {
	// Metadata returned to the current browser session only. Tool result text is
	// deliberately absent so it cannot leak through run history or SSE events.
	std::string name;
	// Path of the file or resource associated with this record.
	std::string path;
	// Search expression or query supplied to the tool.
	std::string query;
	// Whether the operation completed successfully.
	bool ok;
	// Whether limits prevented returning the complete result.
	bool truncated;
	// Whether the call originated from the provider's native tool
	// protocol.
	bool native;
};

// Shared live state of an agent run, coordinated by the runtime mutex.
struct agent_runtime_task_t {
	// Requested size of the generated image.
	std::string image_size;
	// In-memory live state keyed by user_root + run ID. Completed details expire
	// after one hour and are never serialized by agent_run_store_t.
	std::string id;
	// Root of the installation's uploaded and per-user data.
	std::string upload_root;
	// Filesystem root belonging to the authenticated user.
	std::string user_root;
	// Authenticated account name associated with this object.
	std::string username;
	// Admission control uses this scope to prevent two browser windows from
	// writing the same conversation or single-run recovery checkpoint at once.
	std::string project_path;
	// Terminal failures also persist their visible conversation turn. Keeping the
	// original request here lets finish_runtime_task save diagnostic reasoning.
	std::string original_prompt;
	// Interface language captured for this request or run.
	std::string ui_language = "zh";
	// Conversation title captured when the run starts.
	std::string initial_session_title;
	// Cancellation flag checked at safe execution boundaries.
	bool cancel_requested = false;
	// Pause is cooperative: an in-flight provider response or atomic tool action
	// reaches the next safe boundary before the worker sleeps. Unlike cancellation,
	// no checkpoint, staged change or conversation state is discarded.
	bool pause_requested = false;
	// Phase to restore when a cooperatively paused run resumes.
	std::string phase_before_pause;
	// Whether processing has reached a terminal state.
	bool done = false;
	// Current lifecycle or outcome status.
	std::string status = "running";
	// Current processing phase reported to progress observers.
	std::string phase = "queued";
	// Name of the tool currently reported as running.
	std::string current_tool;
	// Bounded answer text received before the model response completes.
	std::string streamed_text;
	// Latest provider stream counters and timing information.
	webcool::ai::provider_stream_progress_t stream_progress;
	// Sensitive model reasoning follows the same transient lifecycle as the live
	// answer preview and is never written to the persistent run store.
	std::string streamed_reasoning;
	// Concise user-facing summary of completed work.
	std::string completion_summary;
	// task_contract_json: Serialized task contract retained for later
	// validation.
	// task_acceptance_json: Serialized acceptance evidence retained with
	// the task result.
	// task_acceptance_status: Current outcome of the task acceptance
	// checks.
	std::string task_contract_json, task_acceptance_json,
	    task_acceptance_status;
	// Number of tool calls completed before this checkpoint.
	size_t completed_tool_calls = 0;
	// Incremented only after a mutating tool has durably created a path. The
	// browser uses it to refresh the project tree while the run is still active.
	unsigned long long workspace_mutation_version = 0;
	// Monotonic version for review-only proposals. Unlike workspace mutation,
	// this never implies that a formal project path has changed.
	unsigned long long staged_change_version = 0;
	// Path most recently changed by a workspace tool.
	std::string last_workspace_mutation_path;
	// Number of active subscribers to this run's progress events.
	size_t event_subscribers = 0;
	// Monotonic change counter used to notify progress subscribers.
	unsigned long long event_version = 1;
	// Diagnostic explaining why the operation failed.
	std::string error;
	// Finish time as seconds since the Unix epoch.
	long long finished_at = 0;
	// Result payload returned by the operation.
	webcool::ai::completion_result_t output;
	// Ordered metadata for tool calls executed during this run.
	std::vector<agent_tool_trace_t> traces;
	// File proposals associated with this result or running task.
	std::vector<agent_change_proposal_t> changes;
	// Number of proposals rejected by validation.
	size_t rejected_changes = 0;
	// Relative path of the persisted run result artifact.
	std::string result_file;
	// Review decision attached to the persisted result.
	std::string result_decision;
	// Whether the reviewed proposals were applied to the workspace.
	bool changes_applied = false;
	// Diagnostic from the last attempt to apply reviewed changes.
	std::string changes_apply_error;
	// Identifier of the owning conversation session.
	std::string session_id;
	// Whether conversation memory should be persisted.
	bool remember_session = false;
	// Whether the run is producing a text preview rather than staged
	// edits.
	bool text_preview = false;
	// Directory limiting source access for a preview run.
	std::string preview_context_directory;
	// Whether this run is an assistant conversation rather than a coding
	// task.
	bool assistant_chat = false;
	// Document associated with this run's resource scope.
	std::string document_path;
	// Conversation identifier used to isolate concurrent work.
	std::string conversation_id;
	// Resource scope used by admission and conflict detection.
	webcool::ai::agent_run_scope_t scope;
	// Whether this run persists checkpoints for service-restart recovery.
	bool restart_recovery_enabled = false;
	// Whether this invocation resumed a run interrupted by service
	// restart.
	bool resumed_after_restart = false;
	// Set when a failed run continues from the project-local progress JSON.
	bool resumed_from_progress = false;
	// Whether durable progress is available for an explicit resume.
	bool recovery_available = false;
	// Whether this request carries consent for browser debugging.
	bool browser_debug_consent = false;
	// Whether browser debugging still requires user confirmation.
	bool browser_debug_confirmation_required = false;
	// Number of earlier browser-repair reports associated with this run.
	unsigned long browser_debug_retry_count = 0;
	// Relative path of the resumable progress checkpoint.
	std::string progress_file;
	// Opaque native Responses continuation state copied into each durable
	// checkpoint. It never crosses user/runtime scopes.
	std::string provider_response_id;
	// Whether a background response still needs retrieval or consumption.
	bool provider_response_pending = false;
	// Native tool results awaiting delivery to the provider.
	std::vector<webcool::ai::completion_tool_output_t>
	    provider_tool_outputs;
	// One JSONL file per run accompanies the optional reasoning text file. This
	// contains only operational metadata (never prompts, source, tool results or
	// credentials) and is updated while the model/tool loop is still running.
	webcool::mutex operation_log_mutex;
	// Bounded serialized operation events for this run.
	std::string operation_log;
	// Whether the durable operation log has been initialized.
	bool operation_log_initialized = false;
	// Whether older events were dropped to satisfy the log size limit.
	bool operation_log_truncated = false;
	// Capacity admission is separate from registration. Registered work can wait
	// fairly without consuming a provider request or being reported as failed.
	bool execution_admitted = false;
	// Admission ordering number assigned to this runtime task.
	unsigned long long queue_sequence = 0;
	// Handle of the ACL fiber performing provider I/O. Keeping it with the
	// per-user runtime task lets the cancel endpoint interrupt a fiber suspended
	// in DNS/connect/TLS/header/body I/O instead of waiting for the HTTP timeout.
	std::shared_ptr<acl::fiber> worker;
};

// Tracks validation commands unavailable in the current environment.
struct unavailable_validation_t {
	// Fingerprint used to identify a particular content version.
	std::string fingerprint;
	// Serialized evidence or diagnostic report for this operation.
	std::string report;
	// Check whether the current draft matches the cached
	// unavailable-validation state.
	bool matches(const std::string &current) const
	{
		return !report.empty() && fingerprint == current;
	}
};
// Cached validation report bound to its staged content fingerprint.
struct validation_cache_t {
	// Fingerprint identifying the current staged source tree.
	std::string draft_fingerprint;
	// Fingerprint used to identify a particular content version.
	std::string fingerprint;
	// Serialized evidence or diagnostic report for this operation.
	std::string report;
};
// Validation evidence tied to the exact draft and source baseline.
struct batch_validation_evidence_t {
	// report: Serialized evidence or diagnostic report for this
	// operation.
	// draft: Fingerprint or identity of the draft under observation.
	// baseline: Fingerprint of the source state used for validation.
	std::string report, draft, baseline;
};

// Runtime helpers.
// Combine user identity and run ID for isolated live task lookup.
std::string runtime_task_key(
    const std::string &user_root, const std::string &id);

// Register live run state under the runtime's admission constraints.
bool register_runtime_task(
    const std::shared_ptr<agent_runtime_task_t> &task, std::string &err);

// Yield until the run is admitted or cancelled.
bool wait_for_runtime_admission(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Remove the user's run from the live task registry.
void remove_runtime_task(const std::string &user_root, const std::string &id);

// Read the run's cancellation state under runtime synchronization.
bool runtime_cancel_requested(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Return the synchronized count of completed tool calls.
size_t runtime_completed_tool_count(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Yield during a cooperative pause and stop if cancellation is requested.
bool wait_while_runtime_paused(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Request or clear a cooperative pause for the selected run.
bool request_runtime_pause(const std::string &user_root, const std::string &id,
    bool paused, bool &already_done, bool &pause_requested);

// Mark the selected run for cancellation at its next safe boundary.
bool request_runtime_cancel(
    const std::string &user_root, const std::string &id, bool &already_done);

// Associate the scheduled worker with its live run record.
void attach_runtime_worker(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::shared_ptr<acl::fiber> &worker);

// Inspect whether a workspace entry exists and whether it is a directory.
bool workspace_entry_state(webcool::ai::agent_workspace_t &workspace,
    const std::string &raw_path, bool &exists, bool &directory,
    std::string &err);
// Return the project-relative path of a run's operation log.
std::string operation_log_path(
    const std::shared_ptr<agent_runtime_task_t> &task);
// Bound and sanitize detail text before recording an operation event.
std::string sanitize_operation_detail(const std::string &input);
// Append a batch of operation events to the owning run's log.
void append_runtime_operation_events(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<acl::json_node *> &events);

// Append a bounded structured event to the owning run's operation log.
void append_runtime_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, acl::json_node &event);

// Record an operation event with a bounded textual detail.
void append_simple_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
    const std::string &phase, const std::string &tool,
    size_t completed_tool_calls);

// Record model outcome and usage metadata without exposing credentials.
void append_model_operation_event(
    const std::shared_ptr<agent_runtime_task_t> &task, const char *name,
    const webcool::ai::completion_result_t &output,
    const std::string &error = std::string(),
    long long effective_max_output_tokens = 0);

// Update the live phase and progress counters visible to subscribers.
void update_runtime_progress(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &phase, const std::string &current_tool,
    size_t completed_tool_calls);

// Publish completion metadata for the latest tool operation.
void complete_runtime_tool(const std::shared_ptr<agent_runtime_task_t> &task,
    const agent_tool_trace_t &trace, size_t completed_tool_calls);

// Reset per-request streaming state before reading provider output.
void begin_runtime_model_stream(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Return a synchronized copy of the currently streamed reasoning.
std::string runtime_reasoning_snapshot(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Select the reasoning text suitable for the requested save operation.
std::string runtime_reasoning_for_save(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Publish the checkpoint path and notify recovery-state subscribers.
void mark_runtime_recovery_available(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &progress_file);

// Append bounded reasoning text to the live runtime snapshot.
bool append_runtime_reasoning_delta(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &delta);

// Append bounded answer text to the live runtime snapshot.
bool append_runtime_model_delta(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &delta);

// Return the change counter used by event subscribers.
unsigned long long runtime_event_version(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Return the version of the run's staged-change snapshot.
unsigned long long runtime_staged_change_version(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Reserve a bounded subscription slot for this run's events.
bool begin_runtime_subscription(
    const std::shared_ptr<agent_runtime_task_t> &task);

// Look up a run within the authenticated user's live runtime state.
std::shared_ptr<agent_runtime_task_t> find_runtime_task(
    const std::string &user_root, const std::string &id);

// Publish the terminal outcome and persist the run's completion metadata.
void finish_runtime_task(const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &status, const std::string &error,
    const webcool::ai::completion_result_t *output,
    const std::vector<agent_tool_trace_t> *traces,
    const std::vector<agent_change_proposal_t> *changes,
    size_t rejected_changes);

// Associate the persisted result artifact with the live task.
void set_runtime_result_artifact(
    const std::shared_ptr<agent_runtime_task_t> &task, const std::string &path,
    const std::string &decision);

// Publish the user-facing summary of the completed run.
void set_runtime_completion_summary(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::string &completion_summary);

// Publish whether reviewed proposals were applied to the workspace.
void set_runtime_changes_applied(
    const std::shared_ptr<agent_runtime_task_t> &task, bool applied,
    const std::string &apply_error, const std::string &last_path,
    size_t mutation_count);

// Publish the current staged proposal generations for browser review.
void publish_runtime_staged_changes(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<agent_change_proposal_t> &changes);

// Merge review decisions into the synchronized live run snapshot.
void publish_runtime_change_reviews(
    const std::shared_ptr<agent_runtime_task_t> &task,
    const std::vector<webcool::ai::agent_change_review_t> & /* reviews */,
    const webcool::ai::agent_result_t &persisted);

// Attach bounded review diffs to validated change proposals.
void attach_change_preview_diffs(std::vector<agent_change_proposal_t> &changes,
    const webcool::ai::workspace_change_set_preview_t &preview);

// Serialize one metadata-only run audit record.
void add_run_record_json(
    acl::json_node &item, const webcool::ai::agent_run_record_t &record);

// Serialize the live run's displayable result and tool metadata.
void add_runtime_result_json(acl::json_node &root,
    const std::shared_ptr<agent_runtime_task_t> &task,
    const webcool::ai::agent_result_t *persisted = NULL,
    bool include_changes = true);

// Find persisted progress for a run interrupted by service restart.
void add_durable_recovery_json(acl::json_node &root,
    const std::string &user_root, const webcool::ai::agent_run_record_t &record,
    const std::string &session_hint);

// Serialize saved result metadata for the authenticated browser request.
void add_persisted_result_json(acl::json_node &root,
    const webcool::ai::agent_result_t &result,
    const std::string &relative_path);

// Context helpers.

// Resolve a local project selection into logical and physical paths.
bool local_project_location(const std::string &input, std::string &logical,
    std::string &physical, std::string &err);

// Validate a physical directory before using it for Git project operations.
bool safe_git_project_directory(const std::string &physical);

// Resolve authorized temporary images into bounded in-memory attachments.
bool load_temporary_attachments(acl::json_node *attachment_node,
    const std::string &draft, const std::string &user_root,
    std::vector<webcool::ai::completion_image_t> &images,
    std::string &text_context, std::string &err);

// Remove the task's temporary attachment staging files.
void remove_temporary_attachment_draft(
    const std::string &draft, const std::string &user_root);

// Allocate a fresh opaque identifier for an agent run.
std::string new_run_id();

// Select the requested provider or the configured default from the available
// list.
const webcool::ai::provider_config_t *select_provider(
    const std::vector<webcool::ai::provider_config_t> &providers,
    const std::string &requested);

// Recognize the DeepSeek provider configuration.
bool provider_is_deepseek(const webcool::ai::provider_config_t &provider);

// Check whether the model permits disabling its thinking mode.
bool provider_supports_thinking_disable(
    const webcool::ai::provider_config_t &provider);

// Check whether the selected model needs the extended reasoning allowance.
bool provider_requires_extended_reasoning_budget(
    const webcool::ai::provider_config_t &provider);

// Combine the user's request with the selected project and session context.
bool compose_initial_prompt(webcool::ai::agent_workspace_t &workspace,
    const webcool::ai::provider_config_t &provider,
    const std::string &user_root, const std::string &project_path,
    const std::string &prompt, const std::string &current_session_id,
    const std::string &execution_mode, const std::string &previous_summary,
    std::string &initial_prompt, std::string &err, bool chinese,
    const webcool::ai::agent_project_record_t *selected_project = NULL,
    operation_trace_t *trace = NULL);

// Summarize completed edits and observed validation without model inference.
std::string deterministic_delivery_summary(const std::string &user_request,
    const std::vector<agent_change_proposal_t> &changes,
    const std::string &validation_state);

// Build the user-facing completion summary from the run outcome.
std::string completion_summary_for_run(const std::string &provided,
    const std::string &memory_summary, const std::string &final_text,
    const std::vector<agent_change_proposal_t> &changes,
    const std::string &original_prompt);

// Tools helpers.
// Serialize a failed tool result with its diagnostic message.
std::string tool_error_json(const std::string &error);

// Extract bounded rejection causes from tool feedback, including nested
// batches.
void collect_proposal_failure_causes(const std::string &result,
    std::vector<std::string> &causes, size_t depth = 0);

// Identify tools that incrementally update the staged proposal set.
bool is_incremental_proposal_tool(const std::string &name);

// Check whether every request in the batch is a proposal operation.
bool request_contains_only_proposals(const agent_tool_request_t &request);

// Merge streamed call updates without losing their provider identifiers.
agent_tool_request_t merge_completion_tool_calls(
    const std::vector<webcool::ai::completion_tool_call_t> &calls);

// Pair batch results with the original native tool-call identifiers.
bool build_native_tool_outputs(
    const std::vector<webcool::ai::completion_tool_call_t> &calls,
    const std::string &combined_result,
    std::vector<webcool::ai::completion_tool_output_t> &outputs);

// Check that native calls can be matched to their tool outputs.
bool completion_calls_have_ids(
    const std::vector<webcool::ai::completion_tool_call_t> &calls);

// Mark which portions of a read result were already supplied to the model.
std::string annotate_read_coverage(const std::string &name,
    const std::string &result, webcool::ai::agent_read_coverage_t &coverage,
    bool chinese);

// Collect stable signatures used to recognize repeated tool observations.
void collect_observation_signatures(const std::string &name,
    const std::string &result, std::vector<std::string> &signatures);

// Compare this validation report with earlier draft failure evidence.
bool repeated_repair_failure(const std::string &report,
    const std::string &draft, webcool::ai::repair_failure_tracker_t &tracker);

// Check whether the requested action only inspects repair-review evidence.
bool repair_review_read_only(const agent_tool_request_t &request);

// Mark required source versions satisfied by the latest read result.
void consume_repair_reads(const std::string &name, const std::string &result,
    std::map<std::string, std::string> &required);

// Describe source evidence that must be read before repair can proceed.
std::string repair_review_required(
    const std::map<std::string, std::string> &required, bool chinese);

// Build continuation guidance for a truncated batch of source reads.
std::string batch_read_continuation(
    const std::string &name, const std::string &result);
// Retain valid source pages extracted from a workspace tool result.
bool remember_read_result(const std::string &name, const std::string &result,
    webcool::ai::agent_read_context_t &context);

// Check whether a read result is still present in bounded context.
bool read_result_is_retained(const std::string &name, const std::string &result,
    const webcool::ai::agent_read_context_t &context);

// Restore version-bound source pages from saved context records.
void restore_read_context(
    const std::string &transcript, webcool::ai::agent_read_context_t &context);

// Translate a logical project path into the draft workspace.
bool project_path_to_draft_path(const std::string &project_path,
    const std::string &path, std::string &draft_path);

// Fingerprint the source baseline against which validation ran.
std::string validation_baseline_fingerprint(
    webcool::ai::agent_workspace_t &workspace, const std::string &project_path);

// Describe the successful checks without claiming unexecuted validation.
std::string successful_validation_summary(
    const std::string &report, bool chinese);

// Execute the workspace tool tool against its authorized context.
std::string execute_workspace_tool(webcool::ai::agent_workspace_t &workspace,
    const std::string &user_root, const std::string &project_path,
    bool allow_file_content, const agent_tool_request_t &request,
    agent_tool_trace_t &trace,
    std::vector<agent_change_proposal_t> *staged_changes,
    const std::string &run_id,
    const webcool::ai::sandbox_limits_t &sandbox_limits, bool chinese,
    const unavailable_validation_t *unavailable_validation = NULL,
    validation_cache_t *validation_cache = NULL,
    std::string *saved_proposal_batch = NULL,
    batch_validation_evidence_t *batch_validation = NULL);

// Drafts helpers.
// Verify that the workspace already contains every reviewed result change.
bool verify_result_changes_applied(const std::string &user_root,
    const webcool::ai::agent_result_t &result, std::string &err);

// Check whether the live workspace already reflects the proposed operation.
bool workspace_change_matches(const std::string &user_root,
    const webcool::ai::workspace_change_input_t &change, bool &matches,
    std::string &err);

// Check whether a workspace path is below the supplied directory.
bool workspace_path_is_below(
    const std::string &path, const std::string &directory);

// Resolve the private draft root for this project and run.
std::string persistent_draft_root(const std::string &user_root,
    const std::string &project_path, const std::string &run_id);

// Create or refresh the private source tree containing the staged edits.
bool materialize_staged_worktree(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes, size_t &skipped_files,
    std::string &err);

// Reconcile live proposal status with persisted review decisions.
bool synchronize_live_review_state(
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    const std::string &project_path,
    std::vector<agent_change_proposal_t> &active_changes,
    size_t &resolved_count, std::string &err);

// Remove the private draft tree belonging to the specified run.
bool remove_persistent_worktree(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    std::string &err);

// Schedule cleanup after review no longer needs the private draft.
bool schedule_reviewed_worktree_cleanup(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    std::string &err);

// Run the permitted fixed validation commands against the private draft.
std::string validate_staged_draft(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes,
    const webcool::ai::sandbox_limits_t &limits, agent_tool_trace_t &trace,
    std::string *source_fingerprint = NULL, bool build_only = false);

// Compute a stable identity for the current ordered change proposals.
std::string staged_change_fingerprint(
    const std::vector<agent_change_proposal_t> &changes);

// Compute the draft identity used to detect repeated repair cycles.
std::string staged_cycle_fingerprint(
    const std::vector<agent_change_proposal_t> &changes);

// Loop helpers.
// Restore progress only when its provider, project and task identity match.
bool load_matching_coding_progress(webcool::ai::agent_progress_store_t &store,
    const webcool::ai::provider_config_t &provider,
    const std::string &project_path, const std::string &original_prompt,
    std::string &initial_prompt, std::string &recovered_reasoning,
    size_t &completed_tool_calls, bool &resumed, std::string &err);

// Run the synchronous coding loop using caller-owned state and outputs.
bool run_coding_tool_loop(const webcool::ai::provider_config_t &provider,
    const std::string &api_key, webcool::ai::agent_workspace_t &workspace,
    const std::string &project_path, const std::string &original_prompt,
    const std::string &initial_prompt, const std::string &recovered_reasoning,
    const std::vector<webcool::ai::completion_image_t> &request_images,
    size_t initial_completed_tool_calls, size_t max_tool_calls,
    long long max_output_tokens, size_t max_no_progress_tool_calls,
    size_t tool_context_compaction_bytes,
    const webcool::ai::sandbox_limits_t &sandbox_limits,
    const std::string &thinking_mode, const std::string &reasoning_effort,
    webcool::ai::completion_result_t &final_output,
    std::vector<agent_tool_trace_t> &traces,
    std::vector<agent_change_proposal_t> &changes, std::string &memory_summary,
    std::string &completion_summary, std::string &session_title,
    size_t &rejected_changes,
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    std::string &err);

// Worker helpers.
// Generate the requested image and retain its authorized attachment result.
bool run_assistant_image_task(const std::shared_ptr<agent_runtime_task_t> &task,
    const webcool::ai::provider_config_t &provider, const std::string &api_key,
    webcool::ai::completion_result_t &output, std::string &err);

// Execute a registered coding task and publish its terminal runtime state.
void run_async_coding_task(
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    webcool::ai::provider_config_t provider, std::string api_key,
    const std::string &project_path, const std::string &original_prompt,
    const std::string &initial_prompt, const std::string &recovered_reasoning,
    const std::vector<webcool::ai::completion_image_t> &request_images,
    size_t initial_completed_tool_calls, size_t max_tool_calls,
    long long max_output_tokens, size_t max_no_progress_tool_calls,
    size_t tool_context_compaction_bytes,
    const webcool::ai::sandbox_limits_t &sandbox_limits,
    const std::string &thinking_mode, const std::string &reasoning_effort);

// Reconstruct live task state from persisted recovery information.
std::shared_ptr<agent_runtime_task_t> recover_runtime_task(
    const std::string &upload_root, const std::string &user_root,
    const std::string &username, const webcool::ai::agent_run_record_t &record,
    std::string &err);

// Common helpers.

// Resolve a run record for an authorized review request and session.
bool load_review_run_record(const std::string &user_root,
    const std::string &run_id, const std::string &session_id,
    webcool::ai::agent_run_record_t &record, std::string &err);

// Check whether the run status permits reviewing its saved proposals.
bool reviewable_agent_run_status(const std::string &status);

// Read a JSON scalar as text, using the supplied fallback when applicable.
std::string json_text(acl::json_node *node);

// Read a JSON number with the caller's fallback for missing values.
long long json_number(acl::json_node *node, long long fallback);

// Read a JSON boolean with the caller's fallback for missing values.
bool json_bool(acl::json_node *node, bool fallback);

// Return the array represented by this JSON node, or null.
acl::json_node *json_array_node(acl::json_node *node);

// Parse a bounded array of JSON strings into the output collection.
bool parse_string_array(
    acl::json_node *node, size_t limit, std::vector<std::string> &values);

// Serialize a JSON node to its wire representation.
std::string serialize_json(acl::json_node &root);

// CPU percentages are interval averages, not sampled peaks. Thread time includes
// other fibers on the same OS thread; process time includes concurrent requests.
struct operation_cpu_snapshot_t {
	// Wall-clock timestamp or elapsed time used for accounting.
	std::chrono::steady_clock::time_point wall =
	    std::chrono::steady_clock::now();
	// thread_ms: Thread CPU time in milliseconds.
	// process_ms: Process CPU time in milliseconds.
	double thread_ms = -1, process_ms = -1;
	// Initialize operation cpu snapshot state from the supplied
	// arguments.
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
	// Append elapsed CPU counters relative to this snapshot to the JSON
	// event.
	void add_delta(
	    acl::json_node &node, const operation_cpu_snapshot_t &end) const
	{
		const double elapsed =
		    std::chrono::duration<double, std::milli>(end.wall - wall)
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
	// Work and CPU timing accumulated for one named processing phase.
	struct stage_t {
		// Name used to identify this item in its containing
		// collection.
		std::string name;
		// begin: Wall and CPU counters captured when this phase
		// began.
		// end: Wall and CPU counters captured when this phase ended.
		operation_cpu_snapshot_t begin, end;
	};
	// operation_: Operation name attached to timing and diagnostic
	// records.
	// project_id_: Identifier of the registered project record.
	// phase_: Name of the phase currently being measured.
	std::string operation_, project_id_, phase_ = "authenticate_and_parse";
	// started_: Wall and CPU counters captured at operation start.
	// phase_started_: Wall and CPU counters captured at the current phase
	// start.
	operation_cpu_snapshot_t started_, phase_started_;
	// Per-phase timing records accumulated by the operation trace.
	std::vector<stage_t> stages_;
	// Runtime task associated with this diagnostic trace.
	std::shared_ptr<agent_runtime_task_t> task_;
	// completed_: Whether the tracked operation has completed.
	// finished_: Whether final timing or completion bookkeeping has been
	// recorded.
	bool completed_ = false, finished_ = false;

public:
	// Initialize operation trace state from the supplied arguments.
	explicit operation_trace_t(const char *operation)
	        : operation_(operation)
	{
	}
	// Associate the timing trace with the run that will receive its
	// events.
	void bind(const std::shared_ptr<agent_runtime_task_t> &task)
	{
		task_ = task;
	}
	// Finish timing the previous phase and begin the named phase.
	void phase(const char *name)
	{
		operation_cpu_snapshot_t now;
		stages_.push_back({ phase_, phase_started_, now });
		phase_ = name;
		phase_started_ = now;
	}
	// Attach the project identifier used in subsequent operation
	// diagnostics.
	void project_id(const std::string &id)
	{
		project_id_ = id;
	}
	// Mark the operation successful before final timing is recorded.
	void complete()
	{
		completed_ = true;
	}
	// Finalize the operation trace if it has not already been emitted.
	~operation_trace_t()
	{
		finish();
	}
	// Emit final operation timing once, including the last active phase.
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
				event.add_text(
				    "project_id", project_id_.c_str());
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
				event.add_text(
				    "trace_run_id", task_->id.c_str());
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
