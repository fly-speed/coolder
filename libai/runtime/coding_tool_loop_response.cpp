#include "stdafx.h"
// Model response parsing, preview handling and final-answer validation.
#include "coding_tool_loop_internal.h"
#include "../sdk/json_schema.h"
#include "../sdk/vendor/rapidjson/writer.h"

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
		if (!(lower[i] >= 'A' && lower[i] <= 'Z'))
			continue;
		lower[i] += 'a' - 'A';
	}
	return lower.find("would you like me to") != std::string::npos ||
	    lower.find("shall i generate") != std::string::npos ||
	    lower.find("if you confirm") != std::string::npos;
}

} // namespace

namespace
{
std::string coding_tool_schema(const libai::ai::agent_tool_t &tool)
{
	if (!tool.parameters_json.empty())
		return tool.parameters_json;
	namespace json = libai::governance_detail::json;
	json::StringBuffer buffer;
	json::Writer<json::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("type"); writer.String("object");
	writer.Key("properties"); writer.StartObject();
	bool required = false;
	for (const auto &p : tool.parameters) {
		writer.Key(p.name.c_str()); writer.StartObject();
		writer.Key("type"); writer.String("string"); writer.EndObject();
		required = required || p.required;
	}
	writer.EndObject();
	if (required) {
		writer.Key("required"); writer.StartArray();
		for (const auto &p : tool.parameters)
			if (p.required) writer.String(p.name.c_str());
		writer.EndArray();
	}
	writer.EndObject();
	return std::string(buffer.GetString(), buffer.GetSize());
}
std::string check_coding_tool(const libai::ai::completion_request_t &input,
    const std::string &name, const std::string &arguments)
{
	for (const auto &tool : input.tools) {
		if (tool.name != name) continue;
		const auto result = libai::validate_json_schema(
		    coding_tool_schema(tool), arguments);
		if (result.valid) return "";
		return "tool arguments rejected: " + result.code + " " +
		    result.keyword + " " + result.instance_pointer;
	}
	return "tool not advertised for this turn";
}
std::string check_fallback_tool(const libai::ai::completion_request_t &input,
    const libai::ai::agent_protocol_message_t &message)
{
	try {
		namespace json = libai::governance_detail::json;
		json::Document wire;
		libai::governance_detail::parse_json(message.raw_tool_json, wire);
		if (!wire.IsObject() || !wire.HasMember("arguments"))
			throw std::invalid_argument("missing tool arguments");
		json::StringBuffer buffer;
		json::Writer<json::StringBuffer> writer(buffer);
		wire["arguments"].Accept(writer);
		return check_coding_tool(input, message.tool.name,
		    std::string(buffer.GetString(), buffer.GetSize()));
	} catch (const std::exception &) {
		return "invalid fallback tool JSON";
	}
}
}

coding_tool_loop_t::step_t coding_tool_loop_t::validate_tool_schema(
    coding_turn_t &turn)
{
	std::string rejection;
	if (turn.output.native_tool_call) {
		if (turn.output.tool_calls.empty())
			rejection = "native tool response lacks uncoerced arguments";
		else if (turn.output.tool_calls.size() > 16)
			rejection = "native tool batch exceeds admission limit";
		else {
			std::set<std::string> ids;
			for (const auto &call : turn.output.tool_calls) {
				if (call.id.empty() || !ids.insert(call.id).second) {
				rejection = "empty or duplicate tool call ID";
				break;
				}
				rejection = check_coding_tool(
				    turn.input, call.name, call.arguments_json);
				if (!rejection.empty()) break;
			}
		}
	} else {
		rejection = libai::ai::validate_agent_final_message(turn.output.text);
		libai::ai::agent_protocol_message_t message;
		if (rejection.empty() && libai::ai::parse_agent_protocol_message(turn.output.text, message) &&
		    !message.final_message) {
			rejection = check_fallback_tool(turn.input, message);
		}
	}
	if (rejection.empty()) return step_t::proceed;
	// Reject before preview reads, browser tools, merging or any workspace effect.
	record_model_response(turn);
	const std::string guidance = "\n<tool_schema_rejection>Entire tool batch was not executed. " +
	    rejection + ". Return corrected arguments matching the advertised schemas.</tool_schema_rejection>\n";
	transcript += guidance;
	persistent_transcript += guidance;
	next_tool_guidance = guidance;
	provider_tool_history.clear(); read_coverage.clear();
	provider_history_base = transcript;
	append_simple_operation_event(runtime_task, "tool_schema_rejected",
	    "batch_not_executed", rejection, executed_tool_calls);
	if (save_coding_progress(progress_store, provider, runtime_task,
	        project_path, original_prompt, persistent_transcript,
	        accumulated_reasoning, "", executed_tool_calls, err))
		return step_t::next_turn;
	return step_t::failed;
}

coding_tool_loop_t::step_t coding_tool_loop_t::handle_preview_response(
    coding_turn_t &turn)
{
	// Preview tools only gather source evidence. They never enter the coding
	// draft/proposal loop; the browser still applies reviewed document edits.
	if (!runtime_task->text_preview)
		return step_t::proceed;
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
		err =
		    "preview tool access unavailable or read budget exhausted";
		return step_t::failed;
	}
	for (const auto &requested : turn.output.tool_calls) {
		if (!wait_while_runtime_paused(runtime_task)) {
			err = "agent run cancelled";
			return step_t::failed;
		}
		std::string result;
		const bool exhausted =
		    traces.size() >= std::min<size_t>(max_tool_calls, 16) ||
		    transcript.size() >= 256 * 1024;
		if (exhausted)
			result = tool_error_json(prompt_text(
			    prompt_id::read_budget_exhausted, chinese));
		else {
			++executed_tool_calls;
			result = libai::ai::preview_project_read(
			    runtime_task->preview_context_directory,
			    requested.name, requested.path, requested.query);
		}
		agent_tool_trace_t preview_trace;
		preview_trace.name = requested.name;
		preview_trace.path = requested.path;
		preview_trace.query = requested.query;
		acl::json parsed_result(result.c_str());
		preview_trace.ok = parsed_result.finish() &&
		    json_bool(parsed_result.get_root()["ok"], false);
		preview_trace.truncated = result.size() > 32 * 1024;
		preview_trace.native = turn.output.native_tool_call;
		traces.push_back(preview_trace);
		update_runtime_progress(runtime_task, "tool_call",
		    requested.name, executed_tool_calls);
		acl::json evidence;
		acl::json_node &item = evidence.create_node();
		item.add_text("tool", requested.name.c_str());
		item.add_text("path", requested.path.c_str());
		item.add_text("result",
		    libai::ai::utf8_prefix(result, 32 * 1024).c_str());
		transcript += prompt_text(prompt_id::source_evidence, chinese) +
		    serialize_json(item);
		append_runtime_operation_event(runtime_task, item);
		if (exhausted)
			break;
	}
	return step_t::next_turn;
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
	append_model_operation_event(runtime_task, "model_response_completed",
	    turn.output, std::string(),
	    turn.output.effective_max_output_tokens > 0 ?
	        turn.output.effective_max_output_tokens :
	        turn.input.max_output_tokens,
	    turn.has_retry_usage ? &turn.retry_usage : NULL);
	if (!turn.output.reasoning.empty() &&
	    accumulated_reasoning.size() < 1024 * 1024) {
		if (!accumulated_reasoning.empty() &&
		    accumulated_reasoning.size() + 2 < 1024 * 1024) {
			accumulated_reasoning += "\n\n";
		}
		const size_t keep = std::min(static_cast<size_t>(1024 * 1024) -
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
		std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
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

coding_tool_loop_t::step_t coding_tool_loop_t::parse_model_response(
    coding_turn_t &turn)
{
	turn.provider_tool_call_count = 0;
	bool parsed_protocol = true;
	if (turn.output.native_tool_call) {
		if (!turn.output.tool_calls.empty()) {
			turn.provider_tool_call_count =
			    turn.output.tool_calls.size();
			turn.message.tool =
			    merge_completion_tool_calls(turn.output.tool_calls);
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
		parsed_protocol = libai::ai::parse_agent_protocol_message(
		    turn.output.text, turn.message);
		turn.provider_tool_call_count =
		    turn.message.final_message ? 0 : 1;
	}
	const bool deferred_without_changes = parsed_protocol &&
	    turn.message.final_message && turn.message.changes.empty() &&
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
			    prompt_text(
			        prompt_id::final_protocol_repair, chinese) +
			    "\nFor tool operations prefer a native function call using the supplied schema. "
			    "Text fallback must be a complete JSON object with type=tool_call, "
			    "name=workspace.patch_set, arguments={path:...,content:...}. "
			    "content is a JSON-encoded array of old_text/new_text replacements. "
			    "Do not use type=patch_set or omit closing braces. "
			    "If reporting a concrete blocker without edits, use type=final with text and changes=[].";
			if (transcript.size() + guidance.size() >
			    kMaxToolTranscriptBytes) {
				guidance = prompt_text(
				    prompt_id::final_protocol_repair_compact,
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
				return save_coding_progress(progress_store,
				           provider, runtime_task, project_path,
				           original_prompt,
				           persistent_transcript,
				           accumulated_reasoning, "",
				           executed_tool_calls, err) ?
				    step_t::next_turn :
				    step_t::failed;
			}
		}
		err = deferred_without_changes ?
		    (chinese ?
		            "模型反复请求确认，未提交已授权的修改。可恢复本次运行继续处理。" :
		            "AI provider repeatedly deferred the authorized implementation") :
		    (chinese ?
		            "模型连续返回不符合工具调用或最终结果协议的内容，修复重试已用尽。可恢复本次运行继续处理。" :
		            "AI provider repeatedly returned invalid tool-call or final-result protocol");
		libai::ai::ai_log_error(
		    "agent.runtime", "protocol-repair", err);
		std::string save_err;
		if (save_coding_progress(progress_store, provider, runtime_task,
		        project_path, original_prompt, persistent_transcript,
		        accumulated_reasoning, err, executed_tool_calls,
		        save_err)) {
			mark_runtime_recovery_available(
			    runtime_task, progress_store.relative_path());
		} else {
			libai::ai::ai_log_error("agent.runtime",
			    "save-protocol-repair-progress", save_err);
		}
		return step_t::failed;
	}
	if (!(turn.message.final_message && !repair_required_reads.empty()))
		return step_t::proceed;
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

void coding_tool_loop_t::merge_final_proposals(coding_turn_t &turn)
{
	const std::vector<agent_change_proposal_t> changes_before_final =
	    changes;
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
	requirement_progress_json = turn.message.requirement_progress_json;
	session_title = turn.message.session_title;
	rejected_changes =
	    validate_change_proposals(workspace, project_path, changes);
	libai::ai::assign_agent_review_generations(
	    changes_before_final, changes);
}

bool coding_tool_loop_t::run_automatic_validation(const std::string &operation,
    agent_tool_trace_t &validation_trace,std::string &validation)
{
    if(!prepare_execution_revision())return false;
    std::string source;
    if(!workspace.tree_sha256(project_path,source,err))return false;
    const auto proposal=libai::ai::agent_workspace_t::content_sha256(staged_change_fingerprint(changes));
    libai::ai::agent_validation_receipt_t cached;
    {std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);cached=runtime_task->validation_receipt;}
    if(!cached.effect_key.empty() && cached.execution_revision==runtime_task->coding_execution_revision &&
        cached.source_sha256==source && cached.proposal_sha256==proposal) {
        {std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
        for(auto item=runtime_task->tool_effects.rbegin();item!=runtime_task->tool_effects.rend();++item)
            if((item->state=="done"||item->state=="confirmed_applied") && item->name.compare(0,7,"coding.")==0 &&
                item->name.find("_validation")!=std::string::npos) {
                if(item->key!=cached.effect_key){err="latest validation receipt is unavailable; host inspection required";return false;}break;
            }}
        if (!validate_cached_command_receipts(runtime_task->user_root, project_path, cached.report_json, err)) return false;
        validation=cached.report_json;validation_trace.ok=true;
        append_simple_operation_event(runtime_task,"automatic_validation_reused",operation,cached.effect_key,executed_tool_calls);
        return true;
    }
    if(executed_tool_calls>=effective_tool_call_limit || executed_tool_calls>=256) {
        validation=R"({"ok":true,"commands_executed":0,"validation_passed":false,"validation_available":false,"validation_blocked":true,"blocked_reason":"tool_budget_exhausted","message":"Automatic validation was not started because the remaining tool budget is exhausted."})";
        append_simple_operation_event(runtime_task,"automatic_validation_skipped","tool_budget_exhausted","",executed_tool_calls);
        return true;
    }
	if(!wait_while_runtime_paused(runtime_task)){err="agent run cancelled";return false;}
	coding_turn_t verification;
	verification.call=executed_tool_calls;verification.provider_tool_call_count=1;
	verification.message.tool.name=operation;
	verification.message.tool.path=project_path;
	verification.message.tool.query=source+"\n"+proposal;
	acl::json limits;auto &parameters=limits.create_node();
	parameters.add_number("timeout_ms",static_cast<long long>(sandbox_limits.timeout_ms))
	    .add_number("cpu_seconds",static_cast<long long>(sandbox_limits.cpu_seconds))
	    .add_number("memory_bytes",static_cast<long long>(sandbox_limits.memory_bytes))
	    .add_number("process_count",static_cast<long long>(sandbox_limits.process_count))
	    .add_number("open_files",static_cast<long long>(sandbox_limits.open_files))
	    .add_number("file_size_bytes",static_cast<long long>(sandbox_limits.file_size_bytes))
	    .add_number("output_bytes",static_cast<long long>(sandbox_limits.output_bytes));
	verification.message.tool.content=serialize_json(parameters);
	verification.message.tool.content_present=true;
	if(!begin_tool_effect(verification))return false;
	++executed_tool_calls;
	update_runtime_progress(runtime_task,"tool_call","workspace.validate",executed_tool_calls);
	validation=validate_staged_draft(runtime_task->user_root,project_path,
	    runtime_task->id,changes,sandbox_limits,validation_trace,NULL,false,verification.effect_key);
	if(!validation_trace.ok) {
		err="automatic validation outcome requires host reconciliation: "+verification.effect_key;
		return false;
	}
	verification.tool_result=validation;
    acl::json report(validation.c_str());
    if(!report.finish() || validation.size()>512*1024 || json_text(report["source_baseline_sha256"])!=source ||
        json_text(report["proposal_sha256"])!=proposal || json_text(report["tool_execution_key"])!=verification.effect_key) {
        err="automatic validation receipt does not match its input snapshot; host reconciliation required";return false;
    }
    libai::ai::agent_validation_receipt_t receipt;
    receipt.effect_key=verification.effect_key;receipt.execution_revision=runtime_task->coding_execution_revision;
    receipt.source_sha256=source;receipt.proposal_sha256=proposal;receipt.report_json=validation;
    {std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);runtime_task->validation_receipt=receipt;}
	if(settle_tool_effect(verification,step_t::proceed)==step_t::failed)return false;
	append_simple_operation_event(runtime_task,"automatic_validation_committed",
	    verification.message.tool.name,"",executed_tool_calls);
    return true;
}

coding_tool_loop_t::step_t coding_tool_loop_t::finish_model_answer(
    coding_turn_t &turn)
{
	if (!turn.message.final_message)
		return step_t::proceed;
	if(!prepare_execution_revision())return step_t::failed;
	refresh_browser_evidence();
	if (!turn.message.changes.empty() && !browser_evidence.permits_edits())
		return reject_browser_edit();
	if (turn.message.changes.empty()) {
		merge_final_proposals(turn);
	} else {
		// Final-message edits are a mutation batch, even without a native tool
		// call. Freeze its intent before touching the current proposal set.
		if(executed_tool_calls>=effective_tool_call_limit || executed_tool_calls>=256) {
			err="final proposal batch exceeds the remaining tool budget";
			return step_t::failed;
		}
		coding_turn_t mutation;
		mutation.call=executed_tool_calls;mutation.provider_tool_call_count=1;
		mutation.message.tool.name="coding.final_proposals";
		mutation.message.tool.content=turn.output.text;
		mutation.message.tool.content_present=true;
		if(!begin_tool_effect(mutation))return step_t::failed;
		++executed_tool_calls;
		merge_final_proposals(turn);
		{
			std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
			runtime_task->changes=changes;
			runtime_task->completed_tool_calls=executed_tool_calls;
			++runtime_task->staged_change_version;
		}
		acl::json result;auto &receipt=result.create_node();
		receipt.add_bool("ok",rejected_changes==0)
		    .add_number("staged_change_count",static_cast<long long>(changes.size()))
		    .add_number("rejected_changes",static_cast<long long>(rejected_changes))
		    .add_bool("formal_source_changed",false);
		mutation.tool_result=serialize_json(receipt);
		if(settle_tool_effect(mutation,step_t::proceed)==step_t::failed)return step_t::failed;
		append_simple_operation_event(runtime_task,"final_proposals_committed",
		    mutation.message.tool.name,"",executed_tool_calls);
	}
	// A provider's final prose is not evidence that generated code builds.
	// Validate each distinct revision in an isolated materialized draft. A
	// real compiler/test failure is returned to the model for one or more
	// repair turns without modifying the formal project.
	const std::string final_fingerprint =
	    staged_change_fingerprint(changes);
	const bool known_http_startup_failure =
	    final_fingerprint == last_validated_fingerprint &&
	    last_validation_report.find("http-smoke") != std::string::npos &&
	    (last_validation_report.find("sandbox executable is missing") !=
	            std::string::npos ||
	        last_validation_report.find("Address already in use") !=
	            std::string::npos);
	const bool inspection_without_edits = changes.empty() &&
	    !has_task_revision &&
	    !libai::ai::verification_only_task(original_prompt);
	if (inspection_without_edits &&
	    !libai::ai::read_only_analysis_task(original_prompt))
		final_output.text += chinese ?
		    "\n本轮未提交代码修改；未触发既有项目的自动修复，也未确认所需功能已完成。" :
		    "\nNo code changes were submitted in this run; existing project failures were not automatically repaired and requested functionality is not verified.";
	if (inspection_without_edits)
		append_simple_operation_event(runtime_task,
		    "automatic_validation_skipped", "no_task_revision", "",
		    executed_tool_calls);
	if (!inspection_without_edits &&
	    (last_validation_report.empty() ||
	        final_fingerprint != last_validated_fingerprint ||
	        (!last_validation_passed &&
	            last_validation_report.find("\"commands_executed\":0") ==
	                std::string::npos))) {
		agent_tool_trace_t validation_trace;
		validation_trace.name = "workspace.validate";
		validation_trace.path = project_path;
		std::string validation;
		if(final_fingerprint==last_validated_fingerprint && !last_validation_report.empty()) {
			validation=last_validation_report;
		} else {
			if(!run_automatic_validation("coding.final_validation",validation_trace,validation))return step_t::failed;
		}
		last_validation_report = validation;
		record_acceptance_evidence(validation);
		acl::json evidence_json;
		acl::json_node &evidence = evidence_json.create_node();
		evidence.add_text("event", "validation_completed");
		evidence.add_text("report", validation.c_str());
		evidence.add_text("draft_sha256",
		    libai::ai::agent_workspace_t::content_sha256(
		        final_fingerprint)
		        .c_str());
		append_runtime_operation_event(runtime_task, evidence);
		traces.push_back(validation_trace);
		last_validated_fingerprint = final_fingerprint;
		const bool compile_completed =
		    libai::ai::focused_compile_repair(original_prompt) &&
		    validation.find("\"compile_repair_completed\":true") !=
		        std::string::npos;
		const bool checks_failed = !compile_completed &&
		    validation.find("\"validation_passed\":false") !=
		        std::string::npos &&
		    validation.find("\"commands_executed\":0") ==
		        std::string::npos;
		last_validation_passed =
		    validation.find("\"validation_passed\":true") !=
		    std::string::npos;
		acl::json failed_validation(validation.c_str());
		const std::string failed_command = failed_validation.finish() ?
		    json_text(failed_validation["failed_command_id"]) :
		    "";
		const bool compiler_failure =
		    libai::ai::compile_command(failed_command) ||
		    validation.find("[build failed]") != std::string::npos;
		const std::string browser_status = failed_validation.finish() ?
		    json_text(failed_validation["browser_acceptance"]) :
		    "";
		const bool browser_blocked = browser_status == "unavailable";
		if (checks_failed && !browser_blocked &&
		    !known_http_startup_failure &&
		    turn.call < effective_tool_call_limit &&
		    final_validation_repair_attempts <
		        (compiler_failure ? 3U : 1U)) {
			++final_validation_repair_attempts;
			provider_response_id.clear();
			provider_tool_outputs.clear();
			const std::string guidance =
			    "\n\n<draft_validation>\n" + validation +
			    prompt_text(
			        prompt_id::draft_validation_repair, chinese) +
			    validation_repair_context(runtime_task->user_root,
			        project_path, runtime_task->id, changes,
			        validation, 48 * 1024) +
			    "\n<read_working_set>" + read_context.records() +
			    "</read_working_set>\n";
			supplementary_read_calls = 0;
			persistent_transcript = compact_coding_transcript(
			    initial_prompt, changes, traces, guidance, chinese);
			transcript = persistent_transcript;
			provider_tool_history.clear();
			read_coverage.clear();
			provider_history_base = transcript;
			proposal_only_turn = false;
			if (save_coding_progress(progress_store, provider,
			        runtime_task, project_path, original_prompt,
			        persistent_transcript, accumulated_reasoning,
			        "", executed_tool_calls, err))
				return step_t::next_turn;
			return step_t::failed;
		}
		if (compile_completed ||
		    validation.find("\"validation_passed\":true") !=
		        std::string::npos) {
			final_output.text += "\n\nWebCool：" +
			    successful_validation_summary(validation, chinese) +
			    "。";
		} else if (checks_failed) {
			final_output.text +=
			    "\n\nWebCool：构建、测试或功能验收失败，尚未完成验收。";
		} else if(validation.find("tool_budget_exhausted")!=std::string::npos) {
			final_output.text += chinese ? "\n\nWebCool：工具额度已用尽，未启动自动验证，尚未完成验收。" :
			    "\n\nWebCool: tool budget exhausted; automatic validation was not started and acceptance remains pending.";
		} else {
			final_output.text +=
			    "\n\nWebCool：当前环境没有可执行的固定验证命令，"
			    "本次修订尚未经过自动构建。";
		}
	}
	if (last_validation_report.empty())
		return step_t::completed;
	if (last_validation_report.find("\"commands_executed\":0") !=
	    std::string::npos)
		final_output.text +=
		    prompt_text(prompt_id::validation_not_executed, chinese);
	final_output.text +=
	    last_validation_report.find(
	        "\"functional_acceptance\":\"passed\"") != std::string::npos ?
	    "\n功能验收：配置的断言已通过。" :
	    "\n功能验收：未通过或未执行，不能据此确认业务功能可用。";

	return step_t::completed;
}

} // namespace agent_detail
} // namespace action
