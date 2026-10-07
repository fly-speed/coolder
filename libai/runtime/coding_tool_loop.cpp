#include "stdafx.h"
// Main loop and phase orchestration.
#include "coding_tool_loop_internal.h"

namespace action {
namespace agent_detail {

coding_tool_loop_t::coding_tool_loop_t(const coding_loop_arguments_t& arguments)
		: coding_loop_arguments_t(arguments), preparation("prepare_tool_loop"),
		  chinese(runtime_task->ui_language != "en"),
		  progress_store(runtime_task->user_root, project_path, runtime_task->session_id),
		  progress_supervisor(max_no_progress_tool_calls),
		  recent_exchanges(tool_context_compaction_bytes / 2), read_context(96 * 1024),
		  compaction_trigger_bytes(tool_context_compaction_bytes), recent_findings(8 * 1024),
		  executed_tool_calls(initial_completed_tool_calls),
		  effective_tool_call_limit(initial_completed_tool_calls + max_tool_calls) {
    browser_debug_report_threshold = webcool::ai::ai_runtime_policy_get().browser_debug_report_threshold;
    browser_evidence.enabled = !runtime_task->text_preview && !runtime_task->assistant_chat
        && webcool::ai::browser_repair_evidence_t::requests_debug(original_prompt)
        && !webcool::ai::read_only_analysis_task(original_prompt)
        && !webcool::ai::verification_only_task(original_prompt);
}

bool coding_tool_loop_t::run_impl()
{
	preparation.bind(runtime_task);
	preparation.phase("restore_checkpoint_and_draft");
	initialize_transcript();
	if (!restore_checkpoint()) return false;
    if (!require_browser_connection()) return false;
	preparation.phase("validate_recovered_draft");
	validate_recovered_draft();
	if (!prepare_model_tools()) return false;
	preparation.phase("restore_read_context");
	restore_context();
	preparation.phase("discover_toolchain");
	discover_validation_capabilities();
	has_task_revision = !changes.empty();
	preparation.complete();
	preparation.finish();
	while (executed_tool_calls <= effective_tool_call_limit) {
		if (!wait_while_runtime_paused(runtime_task)) {
			err = "agent run cancelled";
			return false;
		}
        if (!require_browser_connection()) return false;
		coding_turn_t turn;
		turn.call = executed_tool_calls;
		const step_t result = run_turn(turn);
		if (result == step_t::failed) return false;
		if (result == step_t::completed) return true;
        if (executed_tool_calls != turn.call) non_tool_turns = 0;
        else if (++non_tool_turns >= 6) {
            err = "模型连续 6 轮未执行工具，已停止协议/策略重试；实际工具额度未被扣除，断点已保存。";
            std::string save_error;
            if (coding_loop_detail::save_coding_progress(progress_store, provider, runtime_task,
                project_path, original_prompt, persistent_transcript, accumulated_reasoning,
                err, executed_tool_calls, save_error)) {
                mark_runtime_recovery_available(runtime_task, progress_store.relative_path());
            } else err += " " + save_error;
            return false;
        }
	}
	return finish_budget_boundary();
}

coding_tool_loop_t::step_t coding_tool_loop_t::run_turn(coding_turn_t& turn)
{
	build_model_request(turn);
	log_model_request(turn);
	if (!request_model(turn)) return step_t::failed;
	step_t result = handle_preview_response(turn);
	if (result != step_t::proceed) return result;
	record_model_response(turn);
	result = parse_model_response(turn);
	if (result != step_t::proceed) return result;
	result = finish_model_answer(turn);
	if (result != step_t::proceed) return result;
	result = check_tool_admission(turn);
	if (result != step_t::proceed) return result;
    if (!execute_tool(turn)) return step_t::failed;
	record_tool_outputs(turn);
	materialize_tool_changes(turn);
	observe_tool_progress(turn);
	track_repair_failures(turn);
	persist_staged_result(turn);
	result = finish_validated_result(turn);
	if (result != step_t::proceed) return result;
	build_tool_feedback(turn);
	result = check_progress_limit(turn);
	if (result != step_t::proceed) return result;
	if (!compact_and_checkpoint(turn)) return step_t::failed;
	return step_t::proceed;
}

} // namespace agent_detail
} // namespace action
