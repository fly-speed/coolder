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
class runtime_stream_observer_t;

// Borrows caller-owned inputs and outputs for the synchronous loop invocation.
struct coding_loop_arguments_t {
	const webcool::ai::provider_config_t &provider;
	const std::string &api_key;
	webcool::ai::agent_workspace_t &workspace;
	const std::string &project_path;
	const std::string &original_prompt;
	const std::string &initial_prompt;
	const std::string &recovered_reasoning;
	const std::vector<webcool::ai::completion_image_t> &request_images;
	size_t initial_completed_tool_calls;
	size_t max_tool_calls;
	long long max_output_tokens;
	size_t max_no_progress_tool_calls;
	size_t tool_context_compaction_bytes;
	const webcool::ai::sandbox_limits_t &sandbox_limits;
	const std::string &thinking_mode;
	const std::string &reasoning_effort;
	webcool::ai::completion_result_t &final_output;
	std::vector<agent_tool_trace_t> &traces;
	std::vector<agent_change_proposal_t> &changes;
	std::string &memory_summary;
	std::string &completion_summary;
	std::string &session_title;
	size_t &rejected_changes;
	const std::shared_ptr<agent_runtime_task_t> &runtime_task;
	std::string &err;
};

class coding_tool_loop_t : private coding_loop_arguments_t {
public:
	explicit coding_tool_loop_t(const coding_loop_arguments_t &arguments);
	bool run();

private:
	unsigned long browser_debug_report_threshold;
	webcool::ai::browser_repair_evidence_t browser_evidence;
	void refresh_browser_evidence();
	std::string probe_browser_experiment();
	std::string browser_experiment_selector;
	std::vector<webcool::ai::completion_image_t> browser_experiment_images,
	    browser_verified_images;
	bool require_browser_connection();
	bool run_impl();
	bool prepare_task_contract();
	void record_acceptance_evidence(const std::string &report);
	void finish_task_acceptance(bool completed);
	std::string task_contract, acceptance_report, acceptance_draft,
	    acceptance_baseline;
	bool acceptance_review_requested = false;
	std::string requirement_progress_json;

	// proceed advances to the next phase; next_turn skips the rest of this turn.
	enum class step_t { proceed, next_turn, completed, failed };
	step_t reject_browser_edit();
	step_t run_turn(coding_turn_t &turn);

	// Startup and checkpoint recovery.
	void initialize_transcript();
	bool restore_checkpoint();
	void validate_recovered_draft();
	bool prepare_model_tools();
	void restore_context();
	void discover_validation_capabilities();

	// Provider requests, retries and response handling.
	void track_repair_edits(coding_turn_t &turn);
	void build_repair_focus(coding_turn_t &turn);
	void select_repair_sources(coding_turn_t &turn);
	void supply_repair_review_sources(coding_turn_t &turn);
	bool prepare_browser_experiment(coding_turn_t &turn);
	void merge_final_proposals(coding_turn_t &turn);
	bool restore_checkpoint_contents(
	    const webcool::ai::agent_progress_t &restored,
	    std::string &restore_err);
	bool retrieve_pending_model(coding_turn_t &turn,
	    runtime_stream_observer_t &observer, bool &model_call_completed);
	void invalidate_changed_read_context(coding_turn_t &turn);
	void build_model_request(coding_turn_t &turn);
	void build_request_instructions(coding_turn_t &turn);
	void build_request_contract(coding_turn_t &turn);
	void build_request_options(coding_turn_t &turn);
	void select_request_tools(coding_turn_t &turn);
	void restore_request_evidence(coding_turn_t &turn);

	void log_model_request(coding_turn_t &turn);
	bool request_or_retrieve_model(coding_turn_t &turn,
	    runtime_stream_observer_t &observer, bool &model_call_completed);
	bool retry_interrupted_stream(
	    coding_turn_t &turn, bool &model_call_completed);
	bool request_model(coding_turn_t &turn);
	step_t handle_preview_response(coding_turn_t &turn);
	void record_model_response(coding_turn_t &turn);
	step_t parse_model_response(coding_turn_t &turn);
	step_t finish_model_answer(coding_turn_t &turn);

	// Tool execution, durable revisions and validation evidence.
	step_t check_tool_admission(coding_turn_t &turn);
	bool execute_tool(coding_turn_t &turn);
	void record_tool_outputs(coding_turn_t &turn);
	void materialize_tool_changes(coding_turn_t &turn);
	void observe_tool_progress(coding_turn_t &turn);
	void track_repair_failures(coding_turn_t &turn);
	void persist_staged_result(coding_turn_t &turn);
	step_t finish_validated_result(coding_turn_t &turn);

	// Next-turn context, progress limits and final budget delivery.
	void build_tool_feedback(coding_turn_t &turn);
	step_t check_progress_limit(coding_turn_t &turn);
	bool compact_and_checkpoint(coding_turn_t &turn);
	bool finish_budget_boundary();

	operation_trace_t preparation;
	const bool chinese;
	std::string persistent_transcript, transcript;
	webcool::ai::agent_progress_store_t progress_store;
	long long total_input_tokens = 0;
	long long total_cached_input_tokens = 0;
	long long total_output_tokens = 0;
	long long total_reasoning_tokens = 0;
	long long total_latency_ms = 0;
	size_t protocol_repair_attempts = 0;
	size_t tool_argument_recovery_attempts = 0;
	webcool::ai::agent_progress_supervisor_t progress_supervisor;
	std::string last_validated_fingerprint;
	std::string last_validation_report;
	webcool::ai::repair_failure_tracker_t repair_failures;
	webcool::ai::draft_cycle_tracker_t draft_cycles;
	std::map<std::string, std::string> repair_required_reads;
	std::map<std::string, std::string> repair_supplied_versions;
	std::map<std::string, std::pair<std::string, std::string>>
	    pending_repair_edits;
	std::string unresolved_repair_contract, repair_contract_findings;
	std::set<std::string> repair_changed_paths;
	size_t repair_blocked_finals = 0;
	bool last_validation_passed = false;
	size_t final_validation_repair_attempts = 0;
	unavailable_validation_t unavailable_validation;
	validation_cache_t validation_cache;
	std::string
	    saved_proposal_batch; // One bounded payload, scoped to this running task.
	// Once a model has exhausted an entire output budget on reasoning, keep the
	// remainder of this run in direct protocol mode. A single successful recovery
	// must not be followed immediately by another high-effort reasoning loop.
	bool output_budget_recovery_mode = false;
	bool recovery_repair_only = false;
	std::string provider_response_id;
	std::vector<webcool::ai::completion_tool_output_t>
	    provider_tool_outputs;
	bool provider_response_pending = false;
	std::string next_tool_guidance;
	std::string provider_history_base;
	std::vector<webcool::ai::completion_tool_exchange_t>
	    provider_tool_history;

	std::string accumulated_reasoning;
	bool recovered_checkpoint_reconciled = false;
	std::vector<webcool::ai::agent_tool_t> model_tools, proposal_tools;
	bool proposal_only_turn = false;
	bool stateless_responses = false;
	webcool::ai::agent_context_window_t recent_exchanges;
	webcool::ai::agent_read_coverage_t read_coverage;
	webcool::ai::agent_read_context_t read_context;
	size_t compaction_trigger_bytes;
	webcool::ai::agent_context_window_t recent_findings;
	std::string validation_capabilities;
	std::shared_ptr<webcool::ai::provider_transport_session_t>
	    transport_session;
	size_t read_batch_argument_errors = 0;

	// A resumed run gets a fresh window while checkpoint counts stay cumulative.
	size_t executed_tool_calls;
	size_t non_tool_turns = 0;
	size_t supplementary_read_calls = 0;
	bool has_task_revision = false;
	const size_t effective_tool_call_limit;
};

} // namespace agent_detail
} // namespace action
