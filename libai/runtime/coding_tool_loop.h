#pragma once

#include "coding_runtime.h"
#include "browser_repair_evidence.h"

namespace action
{
namespace agent_detail
{

// Per-turn state is in coding_tool_loop_internal.h; the stream observer stays
// private to coding_tool_loop_transport.cpp.
struct coding_turn_t;
// Receives provider stream events and updates the owning run's progress.
class runtime_stream_observer_t;

// Borrows caller-owned inputs and outputs for the synchronous loop invocation.
struct coding_loop_arguments_t {
	// Provider configuration selected for this task.
	const webcool::ai::provider_config_t &provider;
	// Provider credential; do not include it in logs or UI responses.
	const std::string &api_key;
	// Authorized workspace used by this operation.
	webcool::ai::agent_workspace_t &workspace;
	// Logical path identifying the selected project.
	const std::string &project_path;
	// Original user request, retained across recovery and compaction.
	const std::string &original_prompt;
	// Prepared prompt containing the task and its initial context.
	const std::string &initial_prompt;
	// Reasoning restored from the interrupted run's checkpoint.
	const std::string &recovered_reasoning;
	// Images retained for the current task's provider requests.
	const std::vector<webcool::ai::completion_image_t> &request_images;
	// Cumulative tool count restored before this invocation starts.
	size_t initial_completed_tool_calls;
	// Maximum number of tool calls allowed for this run.
	size_t max_tool_calls;
	// Upper bound on generated tokens for one provider request.
	long long max_output_tokens;
	// Limit on consecutive tool calls without useful progress.
	size_t max_no_progress_tool_calls;
	// Prompt size in bytes that triggers context compaction.
	size_t tool_context_compaction_bytes;
	// Resource limits applied to external command execution.
	const webcool::ai::sandbox_limits_t &sandbox_limits;
	// Vendor thinking toggle selected for the provider request.
	const std::string &thinking_mode;
	// Provider reasoning-effort setting selected for the request.
	const std::string &reasoning_effort;
	// Caller-owned result populated when the coding loop finishes.
	webcool::ai::completion_result_t &final_output;
	// Ordered metadata for tool calls executed during this run.
	std::vector<agent_tool_trace_t> &traces;
	// File proposals associated with this result or running task.
	std::vector<agent_change_proposal_t> &changes;
	// Compact conversation memory returned to the session store.
	std::string &memory_summary;
	// Concise user-facing summary of completed work.
	std::string &completion_summary;
	// Conversation title produced or updated by this run.
	std::string &session_title;
	// Number of proposals rejected by validation.
	size_t &rejected_changes;
	// Shared runtime state belonging to this coding invocation.
	const std::shared_ptr<agent_runtime_task_t> &runtime_task;
	// Caller-owned diagnostic populated when the operation fails.
	std::string &err;
};

// Coordinates model turns, staged edits, validation and durable recovery.
class coding_tool_loop_t : private coding_loop_arguments_t {
public:
	// Initialize coding tool loop state from the supplied arguments.
	explicit coding_tool_loop_t(const coding_loop_arguments_t &arguments);
	// Execute the coding loop and return whether the invocation
	// succeeded.
	bool run();

private:
	// Number of relevant reports that triggers a browser-debug prompt.
	unsigned long browser_debug_report_threshold;
	// Tracks browser observations required before and after UI changes.
	webcool::ai::browser_repair_evidence_t browser_evidence;
	// Update browser-repair requirements from the current conversation
	// evidence.
	void refresh_browser_evidence();
	// Run the requested browser observation and return bounded evidence.
	std::string probe_browser_experiment();
	// Element selector used by the current browser experiment.
	std::string browser_experiment_selector;
	// browser_experiment_images: Screenshots captured for the current
	// browser experiment.
	// browser_verified_images: Screenshots retained after browser
	// verification.
	std::vector<webcool::ai::completion_image_t> browser_experiment_images,
	    browser_verified_images;
	// Check browser connectivity before attempting an evidence-dependent
	// edit.
	bool require_browser_connection();
	// Execute the initialized coding loop until completion, failure or a
	// budget boundary.
	bool run_impl();
	// Load or initialize the task requirements before requesting model
	// output.
	bool prepare_task_contract();
	// Associate validation evidence with its exact source and draft
	// revisions.
	void record_acceptance_evidence(const std::string &report);
	// Persist the final task-acceptance outcome for the completed
	// invocation.
	void finish_task_acceptance(bool completed);
	// task_contract: Task requirements and acceptance conditions supplied
	// to the model.
	// acceptance_report: Validation report retained as task acceptance
	// evidence.
	// acceptance_draft: Draft fingerprint associated with the acceptance
	// evidence.
	// acceptance_baseline: Source fingerprint associated with the
	// acceptance evidence.
	std::string task_contract, acceptance_report, acceptance_draft,
	    acceptance_baseline;
	// Whether the model has been asked to review acceptance evidence.
	bool acceptance_review_requested = false;
	// Serialized progress evidence for the task requirements.
	std::string requirement_progress_json;

	// proceed advances to the next phase; next_turn skips the rest of this turn.
	enum class step_t { proceed, next_turn, completed, failed };
	// Reject an edit that lacks the required browser observation
	// evidence.
	step_t reject_browser_edit();
	// Advance one model/tool turn and report the next loop transition.
	step_t run_turn(coding_turn_t &turn);

	// Startup and checkpoint recovery.
	// Build the initial durable transcript and bounded recovered
	// reasoning context.
	void initialize_transcript();
	// Restore resumable progress and reconcile already reviewed
	// proposals.
	bool restore_checkpoint();
	// Recheck recovered proposals before making them available to the
	// model.
	void validate_recovered_draft();
	// Select the authorized tool catalog for this invocation.
	bool prepare_model_tools();
	// Restore compact conversation and source context for the resumed
	// task.
	void restore_context();
	// Describe fixed build and test commands available to this project.
	void discover_validation_capabilities();

	// Provider requests, retries and response handling.
	// Record before/after source evidence for edits made during repair.
	void track_repair_edits(coding_turn_t &turn);
	// Assemble bounded diagnostics identifying the current repair target.
	void build_repair_focus(coding_turn_t &turn);
	// Choose source versions needed to investigate the current failures.
	void select_repair_sources(coding_turn_t &turn);
	// Supply the source evidence required by a pending repair review.
	void supply_repair_review_sources(coding_turn_t &turn);
	// Prepare the observation needed to verify a browser-related change.
	bool prepare_browser_experiment(coding_turn_t &turn);
	// Merge final model proposals into the staged reviewable revision.
	void merge_final_proposals(coding_turn_t &turn);
	// Populate loop state from a loaded checkpoint and verify its
	// baseline.
	bool restore_checkpoint_contents(
	    const webcool::ai::agent_progress_t &restored,
	    std::string &restore_err);
	// Poll an existing background response without creating a duplicate
	// request.
	bool retrieve_pending_model(coding_turn_t &turn,
	    runtime_stream_observer_t &observer, bool &model_call_completed);
	// Discard cached pages whose source versions changed during tool
	// execution.
	void invalidate_changed_read_context(coding_turn_t &turn);
	// Assemble one provider-neutral request from current loop state.
	void build_model_request(coding_turn_t &turn);
	// Compose system and turn instructions for the selected execution
	// mode.
	void build_request_instructions(coding_turn_t &turn);
	// Add the task contract and its acceptance requirements to the
	// request.
	void build_request_contract(coding_turn_t &turn);
	// Apply provider controls, token limits and continuation options.
	void build_request_options(coding_turn_t &turn);
	// Select the tool schemas allowed for the current model turn.
	void select_request_tools(coding_turn_t &turn);
	// Attach retained source, repair and validation evidence to the
	// request.
	void restore_request_evidence(coding_turn_t &turn);

	// Record bounded request telemetry before contacting the provider.
	void log_model_request(coding_turn_t &turn);
	// Choose between a new completion and retrieval of an existing
	// response.
	bool request_or_retrieve_model(coding_turn_t &turn,
	    runtime_stream_observer_t &observer, bool &model_call_completed);
	// Checkpoint an interrupted stream before its permitted recovery
	// attempt.
	bool retry_interrupted_stream(
	    coding_turn_t &turn, bool &model_call_completed);
	// Run the provider request and apply the loop's transport recovery
	// policy.
	bool request_model(coding_turn_t &turn);
	// Handle a text-preview response without entering staged-edit
	// delivery.
	step_t handle_preview_response(coding_turn_t &turn);
	// Accumulate usage and record the completed model response metadata.
	void record_model_response(coding_turn_t &turn);
	// Decode model output into a tool action or final protocol message.
	step_t parse_model_response(coding_turn_t &turn);
	// Validate final delivery and choose whether the loop can finish.
	step_t finish_model_answer(coding_turn_t &turn);

	// Tool execution, durable revisions and validation evidence.
	// Check budgets and evidence requirements before executing the
	// requested tool.
	step_t check_tool_admission(coding_turn_t &turn);
	// Execute the admitted workspace action and retain its trace and
	// result.
	bool execute_tool(coding_turn_t &turn);
	// Pair executed native calls with outputs for provider continuation.
	void record_tool_outputs(coding_turn_t &turn);
	// Publish staged edits into the private draft or restore the previous
	// revision.
	void materialize_tool_changes(coding_turn_t &turn);
	// Update progress tracking from tool success and staged-content
	// changes.
	void observe_tool_progress(coding_turn_t &turn);
	// Compare the latest validation failures with earlier repair
	// observations.
	void track_repair_failures(coding_turn_t &turn);
	// Save the current immutable proposal generations for review and
	// recovery.
	void persist_staged_result(coding_turn_t &turn);
	// Deliver the result once required validation evidence is satisfied.
	step_t finish_validated_result(coding_turn_t &turn);

	// Next-turn context, progress limits and final budget delivery.
	// Assemble the tool outcome and required next-step guidance for the
	// model.
	void build_tool_feedback(coding_turn_t &turn);
	// Choose the next loop transition when progress thresholds are
	// reached.
	step_t check_progress_limit(coding_turn_t &turn);
	// Reduce retained context and persist the next resumable boundary.
	bool compact_and_checkpoint(coding_turn_t &turn);
	// Deliver saved work and recovery information when the call budget is
	// exhausted.
	bool finish_budget_boundary();

	// Timing trace for preparing the coding loop.
	operation_trace_t preparation;
	// Whether user-facing output selects the Chinese prompt variant.
	const bool chinese;
	// persistent_transcript: Compact transcript written to the resumable
	// checkpoint.
	// transcript: Working prompt transcript for the current invocation.
	std::string persistent_transcript, transcript;
	// Store used to persist and restore this run's checkpoint.
	webcool::ai::agent_progress_store_t progress_store;
	// Input tokens accumulated across model requests.
	long long total_input_tokens = 0;
	// Cached input tokens accumulated across model requests.
	long long total_cached_input_tokens = 0;
	// Generated tokens accumulated across model requests.
	long long total_output_tokens = 0;
	// Reasoning tokens accumulated across model requests.
	long long total_reasoning_tokens = 0;
	// Provider request time accumulated in milliseconds.
	long long total_latency_ms = 0;
	// Attempts used to recover malformed model protocol output.
	size_t protocol_repair_attempts = 0;
	// Attempts used to recover incomplete native tool arguments.
	size_t tool_argument_recovery_attempts = 0;
	// Detects repeated or unproductive tool activity.
	webcool::ai::agent_progress_supervisor_t progress_supervisor;
	// Draft identity associated with the last validation report.
	std::string last_validated_fingerprint;
	// Most recent serialized build and test evidence.
	std::string last_validation_report;
	// Tracks repeated and alternating failures across repair attempts.
	webcool::ai::repair_failure_tracker_t repair_failures;
	// Tracks repeated draft fingerprints to prevent repair oscillation.
	webcool::ai::draft_cycle_tracker_t draft_cycles;
	// Source versions that must be read before attempting the repair.
	std::map<std::string, std::string> repair_required_reads;
	// Source versions already supplied as repair context.
	std::map<std::string, std::string> repair_supplied_versions;
	// Before/after excerpts awaiting verification by the next repair
	// review.
	std::map<std::string, std::pair<std::string, std::string>>
	    pending_repair_edits;
	// unresolved_repair_contract: Repair requirements still awaiting
	// supporting evidence.
	// repair_contract_findings: Evidence retained for checking the
	// repair's stated constraints.
	std::string unresolved_repair_contract, repair_contract_findings;
	// Source paths changed during the current repair sequence.
	std::set<std::string> repair_changed_paths;
	// Final responses blocked because required repair evidence is
	// missing.
	size_t repair_blocked_finals = 0;
	// Whether the last validated draft passed its required checks.
	bool last_validation_passed = false;
	// Repair attempts made after final validation failed.
	size_t final_validation_repair_attempts = 0;
	// Tracks checks that cannot run in the current environment.
	unavailable_validation_t unavailable_validation;
	// Cached build/test evidence tied to the exact staged revision.
	validation_cache_t validation_cache;
	// Bounded proposal payload retained for follow-up batch operations.
	std::string
	    saved_proposal_batch; // One bounded payload, scoped to this running task.
	// Once a model has exhausted an entire output budget on reasoning, keep the
	// remainder of this run in direct protocol mode. A single successful recovery
	// must not be followed immediately by another high-effort reasoning loop.
	bool output_budget_recovery_mode = false;
	// Whether recovery is restricted to repairing the current staged
	// work.
	bool recovery_repair_only = false;
	// Opaque identifier used to retrieve or continue the provider
	// response.
	std::string provider_response_id;
	// Native tool results awaiting delivery to the provider.
	std::vector<webcool::ai::completion_tool_output_t>
	    provider_tool_outputs;
	// Whether a background response still needs retrieval or consumption.
	bool provider_response_pending = false;
	// Instructions appended before the next tool-selection turn.
	std::string next_tool_guidance;
	// Stable prompt prefix preceding replayed native tool history.
	std::string provider_history_base;
	// Bounded native assistant/tool exchanges retained for provider
	// replay.
	std::vector<webcool::ai::completion_tool_exchange_t>
	    provider_tool_history;

	// Reasoning collected across model turns in this run.
	std::string accumulated_reasoning;
	// Whether saved proposals were reconciled with durable review
	// decisions.
	bool recovered_checkpoint_reconciled = false;
	// model_tools: Tools exposed to the provider for the current coding
	// mode.
	// proposal_tools: Restricted tool catalog used for proposal-delivery
	// turns.
	std::vector<webcool::ai::agent_tool_t> model_tools, proposal_tools;
	// Whether the model must deliver proposals instead of more
	// investigation.
	bool proposal_only_turn = false;
	// Whether Responses history must be replayed without server-side
	// state.
	bool stateless_responses = false;
	// Bounded complete tool exchanges retained for short-term context.
	webcool::ai::agent_context_window_t recent_exchanges;
	// Tracks which source ranges the model has already received.
	webcool::ai::agent_read_coverage_t read_coverage;
	// Versioned source pages retained for future model context.
	webcool::ai::agent_read_context_t read_context;
	// Prompt byte count at which compaction should be considered.
	size_t compaction_trigger_bytes;
	// Bounded observations retained when older exchanges are compacted.
	webcool::ai::agent_context_window_t recent_findings;
	// Serialized summary of available validation commands.
	std::string validation_capabilities;
	// Task-scoped reusable provider HTTP connection state.
	std::shared_ptr<webcool::ai::provider_transport_session_t>
	    transport_session;
	// Count of malformed read-batch requests during this invocation.
	size_t read_batch_argument_errors = 0;

	// A resumed run gets a fresh window while checkpoint counts stay cumulative.
	size_t executed_tool_calls;
	// Consecutive model turns that did not execute a workspace tool.
	size_t non_tool_turns = 0;
	// Additional source reads used to supply missing repair evidence.
	size_t supplementary_read_calls = 0;
	// Whether this run has produced a staged revision for the task.
	bool has_task_revision = false;
	// Resolved tool-call ceiling after applying run policy.
	const size_t effective_tool_call_limit;
};

} // namespace agent_detail
} // namespace action
