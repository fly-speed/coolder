#include "stdafx.h"
// Coding loop entry point and checkpoint lookup used by HTTP actions.
#include "coding_tool_loop.h"

namespace action
{
namespace agent_detail
{

bool load_matching_coding_progress(
	webcool::ai::agent_progress_store_t &store,
	const webcool::ai::provider_config_t &provider,
	const std::string &project_path, const std::string &original_prompt,
	std::string &initial_prompt, std::string &recovered_reasoning,
	size_t &completed_tool_calls, bool &resumed, std::string &err)
{
	webcool::ai::agent_progress_t progress;
	bool found = false;
	if (!store.load(progress, found, err))
		return false;
	resumed = found && progress.provider_id == provider.id &&
		  (progress.provider_model.empty() ||
		   progress.provider_model == provider.model) &&
		  progress.project_path == project_path &&
		  progress.original_prompt == original_prompt &&
		  !progress.transcript.empty();
	if (!resumed)
		return true;
	initial_prompt = progress.transcript;
	recovered_reasoning = progress.reasoning;
	completed_tool_calls = progress.completed_tool_calls;
	return true;
}

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
	std::string &err)
{
	const coding_loop_arguments_t arguments = {
		provider,
		api_key,
		workspace,
		project_path,
		original_prompt,
		initial_prompt,
		recovered_reasoning,
		request_images,
		initial_completed_tool_calls,
		max_tool_calls,
		max_output_tokens,
		max_no_progress_tool_calls,
		tool_context_compaction_bytes,
		sandbox_limits,
		thinking_mode,
		reasoning_effort,
		final_output,
		traces,
		changes,
		memory_summary,
		completion_summary,
		session_title,
		rejected_changes,
		runtime_task,
		err
	};
	return coding_tool_loop_t(arguments).run();
}

} // namespace agent_detail
} // namespace action
