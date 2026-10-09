#include "stdafx.h"
// Model response parsing, preview handling and final-answer validation.
#include "coding_tool_loop_internal.h"

namespace action
{
namespace agent_detail
{

using coding_loop_detail::validation_repair_context;
using coding_loop_detail::save_coding_progress;
using coding_loop_detail::compact_coding_transcript;

namespace
{

bool final_text_defers_implementation(const std::string &text)
{
	if (text.find("是否需要我") != std::string::npos ||
	    text.find("是否要我") != std::string::npos ||
	    text.find("确认后我会") != std::string::npos ||
	    text.find("确认后将") != std::string::npos)
		return true;
	std::string lower = text;
	for (size_t i = 0; i < lower.size(); ++i) {
		if (lower[i] >= 'A' && lower[i] <= 'Z')
			lower[i] += 'a' - 'A';
	}
	return lower.find("would you like me to") != std::string::npos ||
	       lower.find("shall i generate") != std::string::npos ||
	       lower.find("if you confirm") != std::string::npos;
}

} // namespace

coding_tool_loop_t::step_t
coding_tool_loop_t::handle_preview_response(coding_turn_t &turn)
{
	// Preview tools only gather source evidence. They never enter the coding
	// draft/proposal loop; the browser still applies reviewed document edits.
	if (runtime_task->text_preview) {
		total_input_tokens += turn.output.input_tokens;
		total_output_tokens += turn.output.output_tokens;
		total_cached_input_tokens += turn.output.cached_input_tokens;
		total_reasoning_tokens += turn.output.reasoning_tokens;
		total_latency_ms += turn.output.latency_ms;
		final_output = turn.output;
		final_output.input_tokens = total_input_tokens;
		final_output.output_tokens = total_output_tokens;
		final_output.cached_input_tokens = total_cached_input_tokens;
		final_output.reasoning_tokens = total_reasoning_tokens;
		final_output.latency_ms = total_latency_ms;
		if (turn.output.tool_calls.empty())
			return step_t::completed;
		if (runtime_task->preview_context_directory.empty() ||
		    turn.input.tools.empty()) {
			err = "preview tool access unavailable or read budget exhausted";
			return step_t::failed;
		}
		for (const auto &requested : turn.output.tool_calls) {
			if (!wait_while_runtime_paused(runtime_task)) {
				err = "agent run cancelled";
				return step_t::failed;
			}
			std::string result;
			const bool exhausted =
				traces.size() >=
					std::min<size_t>(max_tool_calls, 16) ||
				transcript.size() >= 256 * 1024;
			if (exhausted)
				result = tool_error_json(prompt_text(
					prompt_id::read_budget_exhausted,
					chinese));
			else {
				++executed_tool_calls;
				result = webcool::ai::preview_project_read(
					runtime_task->preview_context_directory,
					requested.name, requested.path,
					requested.query);
			}
			agent_tool_trace_t preview_trace;
			preview_trace.name = requested.name;
			preview_trace.path = requested.path;
			preview_trace.query = requested.query;
			acl::json parsed_result(result.c_str());
			preview_trace.ok =
				parsed_result.finish() &&
				json_bool(parsed_result.get_root()["ok"],
					  false);
			preview_trace.truncated = result.size() > 32 * 1024;
			preview_trace.native = turn.output.native_tool_call;
			traces.push_back(preview_trace);
			update_runtime_progress(runtime_task, "tool_call",
						requested.name,
						executed_tool_calls);
			acl::json evidence;
			acl::json_node &item = evidence.create_node();
			item.add_text("tool", requested.name.c_str());
			item.add_text("path", requested.path.c_str());
			item.add_text("result", webcool::ai::utf8_prefix(
							result, 32 * 1024)
							.c_str());
			transcript += prompt_text(prompt_id::source_evidence,
						  chinese) +
				      serialize_json(item);
			append_runtime_operation_event(runtime_task, item);
			if (exhausted)
				break;
		}
		return step_t::next_turn;
	}
	return step_t::proceed;
}

void coding_tool_loop_t::record_model_response(coding_turn_t &turn)
{
	output_budget_recovery_mode = output_budget_recovery_mode ||
				      turn.output.reasoning_budget_recovered;
	total_input_tokens += turn.output.input_tokens;
	total_cached_input_tokens += turn.output.cached_input_tokens;
	total_output_tokens += turn.output.output_tokens;
	total_reasoning_tokens += turn.output.reasoning_tokens;
	total_latency_ms += turn.output.latency_ms;
	append_model_operation_event(
		runtime_task, "model_response_completed", turn.output,
		std::string(),
		turn.output.effective_max_output_tokens > 0 ?
			turn.output.effective_max_output_tokens :
			turn.input.max_output_tokens);
	if (!turn.output.reasoning.empty() &&
	    accumulated_reasoning.size() < 1024 * 1024) {
		if (!accumulated_reasoning.empty() &&
		    accumulated_reasoning.size() + 2 < 1024 * 1024) {
			accumulated_reasoning += "\n\n";
		}
		const size_t keep =
			std::min(static_cast<size_t>(1024 * 1024) -
					 accumulated_reasoning.size(),
				 turn.output.reasoning.size());
		accumulated_reasoning.append(turn.output.reasoning, 0, keep);
	}
	// A continuation input is single-use. Clearing it here prevents a protocol
	// repair or validation-repair turn from resubmitting the same function
	// outputs. A newly returned tool call installs the next pair below.
	provider_response_id.clear();
	provider_tool_outputs.clear();
	provider_response_pending = false;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		runtime_task->provider_response_id.clear();
		runtime_task->provider_response_pending = false;
		runtime_task->provider_tool_outputs.clear();
	}
	if (!turn.output.native_tool_call) {
		provider_tool_history.clear();
		read_coverage.clear();
		provider_history_base = transcript;
	}
}

coding_tool_loop_t::step_t
coding_tool_loop_t::parse_model_response(coding_turn_t &turn)
{
	turn.provider_tool_call_count = 0;
	bool parsed_protocol = true;
	if (turn.output.native_tool_call) {
		if (!turn.output.tool_calls.empty()) {
			turn.provider_tool_call_count =
				turn.output.tool_calls.size();
			turn.message.tool = merge_completion_tool_calls(
				turn.output.tool_calls);
		} else {
			// Compatibility for adapters compiled against the original scalar
			// result fields. New adapters always populate tool_calls.
			turn.provider_tool_call_count = 1;
			turn.message.tool.name = turn.output.tool_name;
			turn.message.tool.path = turn.output.tool_path;
			turn.message.tool.query = turn.output.tool_query;
			turn.message.tool.old_text = turn.output.tool_old_text;
			turn.message.tool.target_path =
				turn.output.tool_target_path;
			turn.message.tool.content = turn.output.tool_content;
		}
	} else {
		parsed_protocol = webcool::ai::parse_agent_protocol_message(
			turn.output.text, turn.message);
		turn.provider_tool_call_count =
			turn.message.final_message ? 0 : 1;
	}
	const bool deferred_without_changes =
		parsed_protocol && turn.message.final_message &&
		turn.message.changes.empty() &&
		final_text_defers_implementation(turn.message.final_text);
	if (!turn.output.native_tool_call &&
	    (!parsed_protocol || deferred_without_changes)) {
		if (protocol_repair_attempts < kMaxProtocolRepairAttempts &&
		    turn.call < effective_tool_call_limit) {
			// Do not accept a prose plan as a successful coding result. Feed one
			// bounded copy back to the same model and require the already-defined
			// machine protocol without another long reasoning pass.
			std::string excerpt = turn.output.text;
			if (excerpt.size() > 32 * 1024)
				excerpt.resize(32 * 1024);
			std::string guidance =
				"\n\n<invalid_assistant_output>\n" + excerpt +
				"\n</invalid_assistant_output>\n" +
				prompt_text(prompt_id::final_protocol_repair,
					    chinese) +
				"\nFor tool operations prefer a native function call using the supplied schema. "
				"Text fallback must be a complete JSON object with type=tool_call, "
				"name=workspace.patch_set, arguments={path:...,content:...}. "
				"content is a JSON-encoded array of old_text/new_text replacements. "
				"Do not use type=patch_set or omit closing braces. "
				"If reporting a concrete blocker without edits, use type=final with text and changes=[].";
			if (transcript.size() + guidance.size() >
			    kMaxToolTranscriptBytes) {
				guidance = prompt_text(
					prompt_id::
						final_protocol_repair_compact,
					chinese);
			}
			if (transcript.size() + guidance.size() <=
			    kMaxToolTranscriptBytes) {
				transcript += guidance;
				persistent_transcript += guidance;
				provider_tool_history.clear();
				read_coverage.clear();
				provider_history_base = transcript;
				++protocol_repair_attempts;
				if (!save_coding_progress(
					    progress_store, provider,
					    runtime_task, project_path,
					    original_prompt,
					    persistent_transcript,
					    accumulated_reasoning, "",
					    executed_tool_calls, err))
					return step_t::failed;
				return step_t::next_turn;
			}
		}
		err = deferred_without_changes ?
			      (chinese ?
				       "模型反复请求确认，未提交已授权的修改。可恢复本次运行继续处理。" :
				       "AI provider repeatedly deferred the authorized implementation") :
			      (chinese ?
				       "模型连续返回不符合工具调用或最终结果协议的内容，修复重试已用尽。可恢复本次运行继续处理。" :
				       "AI provider repeatedly returned invalid tool-call or final-result protocol");
		webcool::ai::ai_log_error("agent.runtime", "protocol-repair",
					  err);
		std::string save_err;
		if (save_coding_progress(progress_store, provider, runtime_task,
					 project_path, original_prompt,
					 persistent_transcript,
					 accumulated_reasoning, err,
					 executed_tool_calls, save_err)) {
			mark_runtime_recovery_available(
				runtime_task, progress_store.relative_path());
		} else {
			webcool::ai::ai_log_error(
				"agent.runtime",
				"save-protocol-repair-progress", save_err);
		}
		return step_t::failed;
	}
	if (turn.message.final_message && !repair_required_reads.empty()) {
		const std::string review =
			repair_review_required(repair_required_reads, chinese);
		if (++repair_blocked_finals >= 3) {
			err = review;
			return step_t::failed;
		}
		next_tool_guidance = review;
		transcript += "\n" + review;
		persistent_transcript = transcript;
		provider_response_id.clear();
		provider_tool_outputs.clear();
		proposal_only_turn = false;
		return step_t::next_turn;
	}
	return step_t::proceed;
}

coding_tool_loop_t::step_t
coding_tool_loop_t::finish_model_answer(coding_turn_t &turn)
{
	if (turn.message.final_message) {
		refresh_browser_evidence();
		if (!turn.message.changes.empty() &&
		    !browser_evidence.permits_edits())
			return reject_browser_edit();
		const std::vector<agent_change_proposal_t>
			changes_before_final = changes;
		final_output = turn.output;
		final_output.text = turn.message.final_text;
		final_output.reasoning = accumulated_reasoning;
		final_output.input_tokens = total_input_tokens;
		final_output.cached_input_tokens = total_cached_input_tokens;
		final_output.output_tokens = total_output_tokens;
		final_output.reasoning_tokens = total_reasoning_tokens;
		final_output.latency_ms = total_latency_ms;
		// Incrementally staged files remain valid even when the final answer only
		// summarizes the work. Final entries replace same-path staged snapshots.
		for (size_t i = 0; i < turn.message.changes.size(); ++i) {
			for (std::vector<agent_change_proposal_t>::iterator it =
				     changes.begin();
			     it != changes.end();) {
				if (it->path == turn.message.changes[i].path)
					it = changes.erase(it);
				else
					++it;
			}
			changes.push_back(turn.message.changes[i]);
		}
		memory_summary = turn.message.memory_summary;
		completion_summary = turn.message.completion_summary;
		requirement_progress_json =
			turn.message.requirement_progress_json;
		session_title = turn.message.session_title;
		rejected_changes = validate_change_proposals(
			workspace, project_path, changes);
		webcool::ai::assign_agent_review_generations(
			changes_before_final, changes);
		// A provider's final prose is not evidence that generated code builds.
		// Validate each distinct revision in an isolated materialized draft. A
		// real compiler/test failure is returned to the model for one or more
		// repair turns without modifying the formal project.
		const std::string final_fingerprint =
			staged_change_fingerprint(changes);
		const bool known_http_startup_failure =
			final_fingerprint == last_validated_fingerprint &&
			last_validation_report.find("http-smoke") !=
				std::string::npos &&
			(last_validation_report.find(
				 "sandbox executable is missing") !=
				 std::string::npos ||
			 last_validation_report.find(
				 "Address already in use") !=
				 std::string::npos);
		const bool inspection_without_edits =
			changes.empty() && !has_task_revision &&
			!webcool::ai::verification_only_task(original_prompt);
		if (inspection_without_edits &&
		    !webcool::ai::read_only_analysis_task(original_prompt))
			final_output.text +=
				chinese ?
					"\n本轮未提交代码修改；未触发既有项目的自动修复，也未确认所需功能已完成。" :
					"\nNo code changes were submitted in this run; existing project failures were not automatically repaired and requested functionality is not verified.";
		if (inspection_without_edits)
			append_simple_operation_event(
				runtime_task, "automatic_validation_skipped",
				"no_task_revision", "", executed_tool_calls);
		if (!inspection_without_edits &&
		    (last_validation_report.empty() ||
		     final_fingerprint != last_validated_fingerprint ||
		     (!last_validation_passed &&
		      last_validation_report.find("\"commands_executed\":0") ==
			      std::string::npos))) {
			agent_tool_trace_t validation_trace;
			validation_trace.name = "workspace.validate";
			validation_trace.path = project_path;
			const std::string validation =
				final_fingerprint == last_validated_fingerprint &&
						!last_validation_report
							 .empty() ?
					last_validation_report :
					validate_staged_draft(
						runtime_task->user_root,
						project_path, runtime_task->id,
						changes, sandbox_limits,
						validation_trace);
			last_validation_report = validation;
			record_acceptance_evidence(validation);
			acl::json evidence_json;
			acl::json_node &evidence = evidence_json.create_node();
			evidence.add_text("event", "validation_completed");
			evidence.add_text("report", validation.c_str());
			evidence.add_text(
				"draft_sha256",
				webcool::ai::agent_workspace_t::content_sha256(
					final_fingerprint)
					.c_str());
			append_runtime_operation_event(runtime_task, evidence);
			traces.push_back(validation_trace);
			last_validated_fingerprint = final_fingerprint;
			const bool compile_completed =
				webcool::ai::focused_compile_repair(
					original_prompt) &&
				validation.find(
					"\"compile_repair_completed\":true") !=
					std::string::npos;
			const bool checks_failed =
				!compile_completed &&
				validation.find(
					"\"validation_passed\":false") !=
					std::string::npos &&
				validation.find("\"commands_executed\":0") ==
					std::string::npos;
			last_validation_passed =
				validation.find("\"validation_passed\":true") !=
				std::string::npos;
			acl::json failed_validation(validation.c_str());
			const std::string failed_command =
				failed_validation.finish() ?
					json_text(
						failed_validation
							["failed_command_id"]) :
					"";
			const bool compiler_failure =
				webcool::ai::compile_command(failed_command) ||
				validation.find("[build failed]") !=
					std::string::npos;
			const std::string browser_status =
				failed_validation.finish() ?
					json_text(
						failed_validation
							["browser_acceptance"]) :
					"";
			const bool browser_blocked =
				browser_status == "unavailable";
			if (checks_failed && !browser_blocked &&
			    !known_http_startup_failure &&
			    turn.call < effective_tool_call_limit &&
			    final_validation_repair_attempts <
				    (compiler_failure ? 3U : 1U)) {
				++final_validation_repair_attempts;
				provider_response_id.clear();
				provider_tool_outputs.clear();
				const std::string guidance =
					"\n\n<draft_validation>\n" +
					validation +
					prompt_text(
						prompt_id::
							draft_validation_repair,
						chinese) +
					validation_repair_context(
						runtime_task->user_root,
						project_path, runtime_task->id,
						changes, validation,
						48 * 1024) +
					"\n<read_working_set>" +
					read_context.records() +
					"</read_working_set>\n";
				supplementary_read_calls = 0;
				persistent_transcript =
					compact_coding_transcript(
						initial_prompt, changes, traces,
						guidance, chinese);
				transcript = persistent_transcript;
				provider_tool_history.clear();
				read_coverage.clear();
				provider_history_base = transcript;
				proposal_only_turn = false;
				if (!save_coding_progress(
					    progress_store, provider,
					    runtime_task, project_path,
					    original_prompt,
					    persistent_transcript,
					    accumulated_reasoning, "",
					    executed_tool_calls, err))
					return step_t::failed;
				return step_t::next_turn;
			}
			if (compile_completed ||
			    validation.find("\"validation_passed\":true") !=
				    std::string::npos) {
				final_output.text +=
					"\n\nWebCool：" +
					successful_validation_summary(
						validation, chinese) +
					"。";
			} else if (checks_failed) {
				final_output.text +=
					"\n\nWebCool：构建、测试或功能验收失败，尚未完成验收。";
			} else {
				final_output.text +=
					"\n\nWebCool：当前环境没有可执行的固定验证命令，"
					"本次修订尚未经过自动构建。";
			}
		}
		if (!last_validation_report.empty()) {
			if (last_validation_report.find(
				    "\"commands_executed\":0") !=
			    std::string::npos)
				final_output.text += prompt_text(
					prompt_id::validation_not_executed,
					chinese);
			final_output.text +=
				last_validation_report.find(
					"\"functional_acceptance\":\"passed\"") !=
						std::string::npos ?
					"\n功能验收：配置的断言已通过。" :
					"\n功能验收：未通过或未执行，不能据此确认业务功能可用。";
		}
		return step_t::completed;
	}
	return step_t::proceed;
}

} // namespace agent_detail
} // namespace action
