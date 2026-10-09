#pragma once

#include "coding_tool_loop.h"
#include "../validation/repair_scope.h"

namespace action
{
namespace agent_detail
{

// Shared only by the coding-loop implementation files.
struct coding_turn_t {
	size_t call{};
	size_t remaining_tool_calls{};
	bool budget_delivery_turn{};
	webcool::ai::completion_request_t input{};
	webcool::ai::completion_result_t output{};
	bool investigation_checkpoint{};
	webcool::ai::agent_protocol_message_t message{};
	size_t provider_tool_call_count{};
	agent_tool_trace_t trace{};
	std::vector<agent_change_proposal_t> review_generation_baseline{};
	size_t staged_before{};
	std::vector<agent_change_proposal_t> changes_before_tool{};
	std::string staged_fingerprint_before{};
	bool repair_blocked{};
	batch_validation_evidence_t batch_validation{};
	std::string tool_result{};
	std::vector<webcool::ai::completion_tool_output_t>
		current_tool_outputs{};
	bool have_native_outputs{};
	std::string staged_fingerprint_after{};
	bool staged_progress{};
	bool validation_unavailable{};
	bool validation_failed{};
	bool reads_retained{};
	bool draft_cycle{};
	webcool::ai::agent_progress_decision_t progress_decision{};
	std::vector<std::string> proposal_failures{};
	bool repeated_proposal_failure{};
	std::string repair_focus{};
	bool reconsider_repair{};
	std::string addition{};
	std::string automatic_build_feedback{};
};

namespace coding_loop_detail
{

std::string validation_repair_context(
	const std::string &user_root, const std::string &project_path,
	const std::string &run_id,
	const std::vector<agent_change_proposal_t> &changes,
	const std::string &validation_result, size_t maximum_bytes,
	const webcool::ai::completion_request_t *evidence = NULL);

bool save_coding_progress(
	webcool::ai::agent_progress_store_t &store,
	const webcool::ai::provider_config_t &provider,
	const std::shared_ptr<agent_runtime_task_t> &runtime_task,
	const std::string &project_path, const std::string &original_prompt,
	const std::string &transcript, const std::string &reasoning,
	const std::string &last_error, size_t completed_tool_calls,
	std::string &err);

std::string
compact_coding_transcript(const std::string &original_prompt,
			  const std::vector<agent_change_proposal_t> &changes,
			  const std::vector<agent_tool_trace_t> &traces,
			  const std::string &latest_exchange, bool chinese);

} // namespace coding_loop_detail

} // namespace agent_detail
} // namespace action
