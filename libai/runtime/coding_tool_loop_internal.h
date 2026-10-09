#pragma once

#include "coding_tool_loop.h"
#include "../validation/repair_scope.h"

namespace action
{
namespace agent_detail
{

// Shared only by the coding-loop implementation files.
struct coding_turn_t {
	// Index of the current tool-loop iteration.
	size_t call{};
	// Number of tool calls left in the current invocation's budget.
	size_t remaining_tool_calls{};
	// Whether this turn should deliver existing work near the call limit.
	bool budget_delivery_turn{};
	// Provider-neutral request prepared for the current model turn.
	webcool::ai::completion_request_t input{};
	// Normalized provider completion for the current model turn.
	webcool::ai::completion_result_t output{};
	// Whether investigation requires a summary before more reads.
	bool investigation_checkpoint{};
	// Parsed model protocol message selecting a tool or final answer.
	webcool::ai::agent_protocol_message_t message{};
	// Number of native calls emitted by the latest model response.
	size_t provider_tool_call_count{};
	// Metadata describing the current tool execution.
	agent_tool_trace_t trace{};
	// Proposal generations captured before execution for later review
	// reconciliation.
	std::vector<agent_change_proposal_t> review_generation_baseline{};
	// Number of staged proposals before this turn executes a tool.
	size_t staged_before{};
	// Snapshot used to compare or roll back the current tool's edits.
	std::vector<agent_change_proposal_t> changes_before_tool{};
	// Draft fingerprint before executing the tool.
	std::string staged_fingerprint_before{};
	// Whether required source or validation evidence currently blocks
	// repair.
	bool repair_blocked{};
	// Validation evidence produced by the current mutation batch.
	batch_validation_evidence_t batch_validation{};
	// Serialized result returned to the model after tool execution.
	std::string tool_result{};
	// Native outputs produced by this turn's tool calls.
	std::vector<webcool::ai::completion_tool_output_t>
	    current_tool_outputs{};
	// Whether native call results are ready for the next model request.
	bool have_native_outputs{};
	// Draft fingerprint after executing the tool.
	std::string staged_fingerprint_after{};
	// Whether this tool call changed the staged draft.
	bool staged_progress{};
	// Whether required validation could not be executed.
	bool validation_unavailable{};
	// Whether the latest executed validation checks failed.
	bool validation_failed{};
	// Whether read results remain available after context compaction.
	bool reads_retained{};
	// Whether the current edit revisits a previously observed draft.
	bool draft_cycle{};
	// Supervisor decision derived from the current tool outcome.
	webcool::ai::agent_progress_decision_t progress_decision{};
	// Proposal rejection evidence retained for progress supervision.
	std::vector<std::string> proposal_failures{};
	// Whether the current proposal repeats a known rejection cause.
	bool repeated_proposal_failure{};
	// Bounded diagnostic context identifying the current repair target.
	std::string repair_focus{};
	// Whether current evidence requires revisiting the repair strategy.
	bool reconsider_repair{};
	// Text to append to the transcript after this tool turn.
	std::string addition{};
	// Build-validation feedback appended after the tool result.
	std::string automatic_build_feedback{};
};

namespace coding_loop_detail
{

// Build bounded repair guidance from validation diagnostics and evidence.
std::string validation_repair_context(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes,
    const std::string &validation_result, size_t maximum_bytes,
    const webcool::ai::completion_request_t *evidence = NULL);

// Persist resumable transcript, reasoning and staged-change state.
bool save_coding_progress(webcool::ai::agent_progress_store_t &store,
    const webcool::ai::provider_config_t &provider,
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    const std::string &project_path, const std::string &original_prompt,
    const std::string &transcript, const std::string &reasoning,
    const std::string &last_error, size_t completed_tool_calls,
    std::string &err);

// Build a bounded checkpoint transcript retaining task and tool evidence.
std::string compact_coding_transcript(const std::string &original_prompt,
    const std::vector<agent_change_proposal_t> &changes,
    const std::vector<agent_tool_trace_t> &traces,
    const std::string &latest_exchange, bool chinese);

} // namespace coding_loop_detail

} // namespace agent_detail
} // namespace action
