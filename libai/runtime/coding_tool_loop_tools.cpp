#include "stdafx.h"
// Tool admission, execution, materialization and progress observation.
#include "coding_tool_loop_internal.h"
#include "browser_debug.h"

namespace action
{
namespace agent_detail
{

using coding_loop_detail::save_coding_progress;

namespace
{
acl::json_node *browser_object(acl::json_node *n)
{
	return n && n->is_object() ? n : (n ? n->get_obj() : NULL);
}
// Inspect results include changing ancestor geometry. For an occluding frame,
// only the frame itself is evidence; ancestor changes are not causal evidence.
std::string browser_observation(acl::json_node *array, bool frame_only)
{
	if (!array || !array->first_child())
		return "";
	if (!frame_only)
		return serialize_json(*array);
	auto *item = browser_object(array->first_child());
	auto *element = item ? browser_object((*item)["element"]) : NULL;
	return element ? serialize_json(*element) : "";
}
std::string suspected_frame(acl::json_node *array)
{
	for (auto *n = array ? array->first_child() : NULL; n;
	     n = array->next_child()) {
		auto *item = browser_object(n);
		auto *style = item ? browser_object((*item)["style"]) : NULL;
		if (!style || json_text((*item)["tag"]) != "IFRAME" ||
		    json_text((*style)["position"]) != "fixed" ||
		    json_text((*style)["display"]) == "none" ||
		    json_text((*style)["visibility"]) != "visible" ||
		    json_text((*style)["opacity"]) == "0" ||
		    json_text((*style)["color-scheme"]).find("dark") ==
			    std::string::npos)
			continue;
		const auto bg = json_text((*style)["background-color"]);
		if (bg != "rgba(0, 0, 0, 0)" && bg != "transparent")
			continue;
		const auto id = json_text((*item)["id"]);
		if (id.empty())
			continue; // Never select an arbitrary different iframe.
		std::string selector = "iframe[id=\"";
		for (unsigned char c : id) {
			// CSS hex escapes safely encode quotes, backslashes and controls.
			if (c < 32 || c == 127 || c == '"' || c == '\\') {
				const char *hex = "0123456789abcdef";
				selector += '\\';
				selector += hex[c >> 4];
				selector += hex[c & 15];
				selector += ' ';
			} else
				selector += c;
		}
		selector += "\"]";
		if (selector.size() <= 300)
			return selector;
	}
	return "";
}
}

void coding_tool_loop_t::refresh_browser_evidence()
{
	if (!browser_evidence.enabled)
		return;
	const auto id = webcool::ai::browser_debug_evidence_id(
		runtime_task->user_root, project_path, runtime_task->id);
	if (id != browser_evidence.session_id()) {
		browser_experiment_images.clear();
		browser_verified_images.clear();
		browser_experiment_selector.clear();
	}
	browser_evidence.bind(id);
	acl::json status(webcool::ai::browser_debug_status(
				 runtime_task->user_root, project_path)
				 .c_str());
	if (!status.finish() || !json_bool(status["images_allowed"], false)) {
		browser_experiment_images.clear();
		browser_verified_images.clear();
	}
}

std::string coding_tool_loop_t::probe_browser_experiment()
{
	if (browser_experiment_selector.empty() || !provider.allow_file_content)
		return "";
	const auto observed = webcool::ai::browser_debug_tool(
		runtime_task->user_root, project_path, runtime_task->id,
		"browser.inspect", browser_experiment_selector, "");
	refresh_browser_evidence();
	acl::json result(observed.c_str());
	auto *data = result.finish() ? json_array_node(result["data"]) : NULL;
	browser_evidence.observe(
		"browser.inspect", browser_experiment_selector,
		browser_observation(
			data, !browser_evidence.experiment_target().empty()),
		"", "", result.finish() && json_bool(result["ok"], false));
	append_simple_operation_event(
		runtime_task, "browser_experiment_probe", "same_selector",
		browser_experiment_selector, executed_tool_calls);
	acl::json status(webcool::ai::browser_debug_status(
				 runtime_task->user_root, project_path)
				 .c_str());
	if (status.finish() && json_bool(status["images_allowed"], false)) {
		const auto snapshot = webcool::ai::browser_debug_tool(
			runtime_task->user_root, project_path, runtime_task->id,
			"browser.snapshot", "", "{\"image\":true}");
		const auto bytes = webcool::ai::browser_debug_take_image(
			runtime_task->user_root, project_path,
			runtime_task->id);
		if (!bytes.empty()) {
			webcool::ai::completion_image_t image;
			image.name = browser_evidence.active_patch().empty() ?
					     "baseline-or-restored.jpg" :
					     "temporary-style.jpg";
			image.mime_type = "image/jpeg";
			image.data = bytes;
			if (browser_experiment_images.size() == 2)
				browser_experiment_images.erase(
					browser_experiment_images.begin());
			browser_experiment_images.push_back(image);
			if (!browser_evidence.active_patch().empty() &&
			    browser_experiment_images.size() == 2)
				browser_verified_images =
					browser_experiment_images;
		}
		append_simple_operation_event(
			runtime_task, "browser_experiment_screenshot",
			bytes.empty() ? "unavailable" : "captured", "",
			executed_tool_calls);
	}
	return observed;
}

bool coding_tool_loop_t::require_browser_connection()
{
	if (!browser_evidence.enabled)
		return true;
	bool confirmation;
	unsigned long retries;
	{
		std::lock_guard<webcool::mutex> lock(g_agent_runtime_mutex);
		confirmation =
			runtime_task->browser_debug_confirmation_required;
		retries = runtime_task->browser_debug_retry_count;
	}
	const auto id = webcool::ai::browser_debug_evidence_id(
		runtime_task->user_root, project_path, runtime_task->id);
	if (!confirmation)
		refresh_browser_evidence();
	if (!confirmation && !id.empty())
		return true;
	err = chinese ?
		      "浏览器调试未连接到本次任务，已停止后续修改，已有修订和进度保留。请在项目的“浏览器调试”中生成配对码，打开出现问题的 Firefox 标签页，在 WebCool 扩展中粘贴并允许调试；连接成功后恢复本次任务。安装扩展不等于已连接；仅点击继续不会自动建立连接。" :
		      "Affected browser tab is not connected to this run. Further edits stopped; existing revisions and progress are retained. Generate a pairing code in Browser Debug, pair the affected Firefox tab in the WebCool extension, then resume this task. Installing the extension or retrying alone does not establish a connection.";
	if (confirmation)
		err = chinese ?
			      "已尝试修复 " + std::to_string(retries) +
				      " 次，问题仍未解决。是否使用浏览器调试？选择“是”后请生成配对码；选择“否”将结束本轮会话。" :
			      "Already attempted " + std::to_string(retries) +
				      " repairs. Use browser debugging? Choose Yes to set up pairing, or No to end this attempt.";
	final_output.reasoning = accumulated_reasoning;
	final_output.input_tokens = total_input_tokens;
	final_output.cached_input_tokens = total_cached_input_tokens;
	final_output.output_tokens = total_output_tokens;
	final_output.reasoning_tokens = total_reasoning_tokens;
	final_output.latency_ms = total_latency_ms;
	append_simple_operation_event(
		runtime_task,
		confirmation ? "browser_debug_confirmation_required" :
			       "browser_pairing_required",
		confirmation ? "waiting_for_user_choice" :
			       "waiting_for_live_tab",
		"", executed_tool_calls);
	std::string save_error;
	if (save_coding_progress(progress_store, provider, runtime_task,
				 project_path, original_prompt,
				 persistent_transcript, accumulated_reasoning,
				 err, executed_tool_calls, save_error))
		mark_runtime_recovery_available(runtime_task,
						progress_store.relative_path());
	else
		err += " " + save_error;
	return false;
}

coding_tool_loop_t::step_t coding_tool_loop_t::reject_browser_edit()
{
	const std::string guidance =
		"\n<browser_evidence_required>Requested operation not executed. " +
		browser_evidence.guidance() + "</browser_evidence_required>\n";
	transcript += guidance;
	persistent_transcript += guidance;
	provider_tool_history.clear();
	read_coverage.clear();
	provider_history_base = transcript;
	append_simple_operation_event(runtime_task, "browser_edit_blocked",
				      "missing_live_evidence", "",
				      executed_tool_calls);
	if (!save_coding_progress(progress_store, provider, runtime_task,
				  project_path, original_prompt,
				  persistent_transcript, accumulated_reasoning,
				  "", executed_tool_calls, err))
		return step_t::failed;
	return step_t::next_turn;
}

coding_tool_loop_t::step_t
coding_tool_loop_t::check_tool_admission(coding_turn_t &turn)
{
	refresh_browser_evidence();
	if (!browser_evidence.enabled &&
	    !webcool::ai::browser_repair_evidence_t::requests_debug(
		    original_prompt) &&
	    turn.message.tool.name.compare(0, 8, "browser.") == 0) {
		next_tool_guidance =
			"Live browser debugging is not enabled for this task. Continue ordinary source repair and workspace.validate without asking for pairing; browser debugging requires user consent after " +
			std::to_string(browser_debug_report_threshold) +
			" ordinary attempts on the same unresolved issue, or an explicit user request.";
		transcript += "\n" + next_tool_guidance;
		persistent_transcript += "\n" + next_tool_guidance;
		provider_tool_history.clear();
		provider_history_base = transcript;
		return step_t::next_turn;
	}
	// Includes mixed native batches. Read-only batches and fixed validation remain available.
	const bool source_edit =
		is_incremental_proposal_tool(turn.message.tool.name) ||
		turn.message.tool.name == "workspace.create" ||
		turn.message.tool.name == "workspace.mkdir" ||
		(turn.message.tool.name == "workspace.execute_batch" &&
		 !repair_review_read_only(turn.message.tool));
	if (source_edit && !browser_evidence.permits_edits())
		return reject_browser_edit();
	if (browser_evidence.needs_initial_observation() &&
	    turn.message.tool.name != "browser.status" &&
	    turn.message.tool.name != "browser.snapshot" &&
	    turn.message.tool.name != "browser.overlays")
		return reject_browser_edit();
	// A draft is not deployed to this page. Reload is not an appropriate way to
	// validate a staged proposal and can lose the live connection on navigation.
	if (browser_evidence.enabled &&
	    turn.message.tool.name == "browser.interact" && !changes.empty()) {
		acl::json options(turn.message.tool.content.c_str());
		if (options.finish() &&
		    json_text(options["action"]) == "reload") {
			next_tool_guidance =
				"Source proposals have not been applied. Do not reload the live tab to validate a private draft. Use workspace.validate; after review/application, update the service and verify the affected Firefox tab. If disconnected, re-pair before claiming live verification.";
			transcript += "\n" + next_tool_guidance;
			persistent_transcript += "\n" + next_tool_guidance;
			provider_tool_history.clear();
			provider_history_base = transcript;
			return step_t::next_turn;
		}
	}
	const bool browser_investigation =
		!proposal_only_turn && turn.investigation_checkpoint &&
		turn.message.tool.name.compare(0, 8, "browser.") == 0;
	const bool supplemental_read =
		repair_review_read_only(turn.message.tool) &&
		(!repair_required_reads.empty() ||
		 (turn.investigation_checkpoint &&
		  supplementary_read_calls + turn.provider_tool_call_count <=
			  4));
	if ((proposal_only_turn || turn.investigation_checkpoint) &&
	    !request_contains_only_proposals(turn.message.tool) &&
	    !supplemental_read && !browser_investigation) {
		// Some compatible gateways ignore the advertised tool_choice policy or
		// emit the legacy JSON-in-text protocol. Never let that bypass the
		// proposal-only guard and execute another read/validation cycle.
		const std::string guidance =
			"\n<tool_policy_rejection>" + turn.message.tool.name +
			" was not executed and does not consume tool quota. Submit a patch using retained source pages, "
			"or use an advertised read tool for missing source within the remaining bounded allowance. "
			"If blocked, report the concrete missing evidence.</tool_policy_rejection>\n";
		transcript += guidance;
		persistent_transcript += guidance;
		provider_tool_history.clear();
		read_coverage.clear();
		provider_history_base = transcript;
		if (!save_coding_progress(
			    progress_store, provider, runtime_task,
			    project_path, original_prompt,
			    persistent_transcript, accumulated_reasoning, "",
			    executed_tool_calls, err))
			return step_t::failed;
		return step_t::next_turn;
	}
	if (turn.provider_tool_call_count > 16 ||
	    turn.provider_tool_call_count >
		    effective_tool_call_limit - turn.call) {
		final_output = turn.output;
		// Reaching the budget after producing reviewable work is a successful
		// bounded delivery, not a transport interruption. Keep the revision and
		// let the user review it instead of forcing another recovery cycle.
		if (!changes.empty()) {
			const std::string delivery_fingerprint =
				staged_change_fingerprint(changes);
			bool already_validated =
				delivery_fingerprint ==
					last_validated_fingerprint &&
				last_validation_passed;
			std::string validation_state =
				already_validated ?
					successful_validation_summary(
						last_validation_report,
						chinese) :
					"";
			if (!already_validated) {
				agent_tool_trace_t validation_trace;
				validation_trace.name = "workspace.validate";
				validation_trace.path = project_path;
				const std::string validation =
					validate_staged_draft(
						runtime_task->user_root,
						project_path, runtime_task->id,
						changes, sandbox_limits,
						validation_trace);
				traces.push_back(validation_trace);
				record_acceptance_evidence(validation);
				last_validated_fingerprint =
					delivery_fingerprint;
				already_validated =
					validation.find(
						"\"validation_passed\":true") !=
						std::string::npos ||
					validation.find(
						"\"compile_repair_completed\":true") !=
						std::string::npos;
				last_validation_passed = already_validated;
				if (already_validated) {
					validation_state =
						successful_validation_summary(
							validation, chinese);
				} else if (validation.find(
						   "\"commands_executed\":0") !=
					   std::string::npos) {
					validation_state = "固定验证工具不可用";
				} else {
					validation_state =
						"固定构建或测试未通过";
				}
			}
			final_output.text =
				already_validated ?
					validation_state + "。" :
				validation_state == "固定验证工具不可用" ?
					"本轮已达到工具调用额度；现有待审查修订已完整保存，"
					"但当前环境没有可执行的固定构建/测试命令。" :
					"本轮已达到工具调用额度；现有待审查修订已完整保存，"
					"自动构建或测试未通过，可审查现有修订后继续修复。";
			final_output.reasoning = accumulated_reasoning;
			final_output.input_tokens = total_input_tokens;
			final_output.cached_input_tokens =
				total_cached_input_tokens;
			final_output.output_tokens = total_output_tokens;
			final_output.reasoning_tokens = total_reasoning_tokens;
			final_output.latency_ms = total_latency_ms;
			rejected_changes = validate_change_proposals(
				workspace, project_path, changes);
			memory_summary = deterministic_delivery_summary(
				original_prompt, changes, validation_state);
			return step_t::completed;
		}
		err = turn.provider_tool_call_count > 16 ?
			      "模型单次返回的工具调用超过安全批次上限，现有成果已保存；"
			      "请继续当前会话并缩小下一批操作。" :
			      "本次运行已达到工具调用上限，已保存断点；请从断点继续当前会话。";
		final_output.reasoning = accumulated_reasoning;
		final_output.input_tokens = total_input_tokens;
		final_output.cached_input_tokens = total_cached_input_tokens;
		final_output.output_tokens = total_output_tokens;
		final_output.reasoning_tokens = total_reasoning_tokens;
		final_output.latency_ms = total_latency_ms;
		// A tool-limit boundary is not a completed coding result. Preserve the
		// last executable transcript and expose recovery instead of storing an
		// empty "accepted" result that cannot actually be resumed.
		std::string save_err;
		if (!save_coding_progress(
			    progress_store, provider, runtime_task,
			    project_path, original_prompt,
			    persistent_transcript, accumulated_reasoning, err,
			    turn.call, save_err)) {
			webcool::ai::ai_log_error("agent.runtime",
						  "save-tool-limit-progress",
						  save_err);
			err += " (additionally failed to save recovery progress: " +
			       save_err + ")";
		} else {
			mark_runtime_recovery_available(
				runtime_task, progress_store.relative_path());
			err += " Recovery progress was saved at " +
			       progress_store.relative_path() + ".";
		}
		webcool::ai::ai_log_error("agent.runtime", "tool-call-limit",
					  err);
		return step_t::failed;
	}
	return step_t::proceed;
}

bool coding_tool_loop_t::execute_tool(coding_turn_t &turn)
{
	if (!wait_while_runtime_paused(runtime_task)) {
		err = "agent run cancelled";
		return false;
	}
	update_runtime_progress(runtime_task, "tool_call",
				turn.message.tool.name, executed_tool_calls);
	// Rebase the worker just before executing the returned tool. Without this
	// boundary an acceptance performed during the provider request leaves the
	// old proposal in `changes`; validating any later proposal then sees the
	// accepted file as an invalid no-op and falsely trips the no-progress guard.
	turn.review_generation_baseline = changes;
	size_t resolved_reviews = 0;
	std::string review_sync_err;
	if (!synchronize_live_review_state(runtime_task, project_path, changes,
					   resolved_reviews, review_sync_err)) {
		err = review_sync_err;
		return false;
	}
	if (resolved_reviews > 0) {
		unavailable_validation.report.clear();
		validation_cache.report.clear();
		progress_supervisor.reset_after_external_progress();
		read_context.clear();
		read_coverage.clear();
		repair_required_reads.clear();
		repair_supplied_versions.clear();
		pending_repair_edits.clear();
		unresolved_repair_contract.clear();
		repair_contract_findings.clear();
		repair_failures.clear();
		repair_changed_paths.clear();
		draft_cycles.clear();
	}
	turn.staged_before = changes.size();
	turn.changes_before_tool = changes;
	turn.staged_fingerprint_before = staged_change_fingerprint(changes);
	// Evidence supplied on the previous turn is only valid for that exact
	// private draft version. Keep the read gate if anything changed meanwhile.
	if (!repair_supplied_versions.empty() &&
	    !repair_review_read_only(turn.message.tool)) {
		webcool::ai::agent_workspace_t review_draft(
			persistent_draft_root(runtime_task->user_root,
					      project_path, runtime_task->id));
		for (const auto &entry : repair_supplied_versions) {
			std::string relative, content, read_err;
			bool truncated = false;
			if (!project_path_to_draft_path(
				    project_path, entry.first, relative) ||
			    !review_draft.read(relative, content, truncated,
					       read_err) ||
			    truncated ||
			    webcool::ai::agent_workspace_t::content_sha256(
				    content) != entry.second)
				repair_required_reads[entry.first] =
					truncated || !read_err.empty() ?
						entry.second :
						webcool::ai::agent_workspace_t::
							content_sha256(content);
		}
		repair_supplied_versions.clear();
	}
	turn.repair_blocked = !repair_required_reads.empty() &&
			      !repair_review_read_only(turn.message.tool);
	if (turn.repair_blocked) {
		turn.trace.name = turn.message.tool.name;
		turn.trace.path = turn.message.tool.path;
		turn.trace.query = turn.message.tool.query;
		turn.trace.ok = false;
		turn.trace.truncated = false;
		turn.trace.native = false;
	}
	if (!turn.repair_blocked) {
		executed_tool_calls += turn.provider_tool_call_count;
		if (turn.investigation_checkpoint &&
		    repair_review_read_only(turn.message.tool))
			supplementary_read_calls +=
				turn.provider_tool_call_count;
	}
	if (!turn.repair_blocked && browser_evidence.enabled &&
	    provider.allow_file_content &&
	    turn.message.tool.name == "browser.patch_style") {
		acl::json options(turn.message.tool.content.c_str());
		if (options.finish() && json_text(options["undo"]).empty()) {
			browser_experiment_images.clear();
			browser_verified_images.clear();
			// Each hypothesis needs its own baseline. Models sometimes stack a
			// hide-frame probe and a color-scheme probe, masking the latter.
			if (!browser_evidence.active_patch().empty()) {
				const auto restored =
					webcool::ai::browser_debug_tool(
						runtime_task->user_root,
						project_path, runtime_task->id,
						"browser.patch_style", "",
						"{\"undo\":\"all\"}");
				acl::json undo(restored.c_str());
				const bool ok = undo.finish() &&
						json_bool(undo["ok"], false);
				browser_evidence.observe("browser.patch_style",
							 "", "", "", "all", ok);
				append_simple_operation_event(
					runtime_task,
					"browser_experiment_reset",
					"undo_before_next_hypothesis", "",
					executed_tool_calls);
				if (!ok) {
					err = "Could not restore the previous browser experiment; reconnect the affected tab before continuing.";
					return false;
				}
				probe_browser_experiment();
			}
			browser_evidence.describe_experiment(
				"selector=" + turn.message.tool.query +
				"; temporary styles=" +
				turn.message.tool.content);
			browser_experiment_selector =
				browser_evidence.experiment_target().empty() ?
					turn.message.tool.query :
					browser_evidence.experiment_target();
			probe_browser_experiment();
		}
	}
	turn.tool_result =
		turn.repair_blocked ?
			repair_review_required(repair_required_reads, chinese) :
			execute_workspace_tool(
				workspace, runtime_task->user_root,
				project_path, provider.allow_file_content,
				turn.message.tool, turn.trace, &changes,
				runtime_task->id, sandbox_limits, chinese,
				&unavailable_validation, &validation_cache,
				&saved_proposal_batch, &turn.batch_validation);
	if (turn.tool_result.find("read_batch content must be a JSON array") !=
	    std::string::npos) {
		++read_batch_argument_errors;
		acl::json repair_json;
		acl::json_node &repair_event = repair_json.create_node();
		repair_event.add_text("event", "tool_argument_repair");
		repair_event.add_text("tool", "workspace.read_batch");
		repair_event.add_number("failures", read_batch_argument_errors);
		repair_event.add_text("action",
				      read_batch_argument_errors >= 2 ?
					      "fallback_to_single_read" :
					      "provide_argument_example");
		append_runtime_operation_event(runtime_task, repair_event);
	}
	// Close the smaller race in which the user reviews a generation after the
	// pre-tool snapshot but before proposal validation completes. Rebase and
	// retry this same deterministic local tool once; no provider request or
	// additional tool-budget unit is consumed.
	if (!turn.trace.ok) {
		size_t late_resolved_reviews = 0;
		if (!synchronize_live_review_state(
			    runtime_task, project_path, changes,
			    late_resolved_reviews, review_sync_err)) {
			err = review_sync_err;
			return false;
		}
		if (late_resolved_reviews > 0) {
			repair_required_reads.clear();
			repair_supplied_versions.clear();
			pending_repair_edits.clear();
			unresolved_repair_contract.clear();
			repair_contract_findings.clear();
			repair_failures.clear();
			repair_changed_paths.clear();
			draft_cycles.clear();
			unavailable_validation.report.clear();
			validation_cache.report.clear();
			progress_supervisor.reset_after_external_progress();
			read_context.clear();
			read_coverage.clear();
			if (turn.repair_blocked) {
				executed_tool_calls +=
					turn.provider_tool_call_count;
				turn.repair_blocked = false;
			}
			turn.trace = agent_tool_trace_t();
			turn.tool_result = execute_workspace_tool(
				workspace, runtime_task->user_root,
				project_path, provider.allow_file_content,
				turn.message.tool, turn.trace, &changes,
				runtime_task->id, sandbox_limits, chinese,
				&unavailable_validation, &validation_cache,
				&saved_proposal_batch, &turn.batch_validation);
		}
	}
	if (!turn.repair_blocked)
		consume_repair_reads(turn.message.tool.name, turn.tool_result,
				     repair_required_reads);
	if (turn.repair_blocked) {
		acl::json event_json;
		acl::json_node &event = event_json.create_node();
		event.add_text("event", "repair_edit_blocked");
		event.add_text("tool", turn.message.tool.name.c_str());
		append_runtime_operation_event(runtime_task, event);
	}
	turn.tool_result =
		annotate_read_coverage(turn.message.tool.name, turn.tool_result,
				       read_coverage, chinese);
	turn.trace.native = turn.output.native_tool_call;
	traces.push_back(turn.trace);
	return true;
}

void coding_tool_loop_t::record_tool_outputs(coding_turn_t &turn)
{
	if (browser_evidence.enabled &&
	    turn.message.tool.name.compare(0, 8, "browser.") == 0) {
		refresh_browser_evidence();
		acl::json result(turn.tool_result.c_str());
		auto *data = result.finish() ? result["data"] : NULL;
		// Empty/no-match inspections cannot satisfy the evidence gate.
		auto *array = json_array_node(data);
		const std::string observed = browser_observation(
			array,
			!browser_evidence.experiment_target().empty() &&
				turn.message.tool.name == "browser.inspect");
		auto *object = data && data->is_object() ?
				       data :
				       (data ? data->get_obj() : NULL);
		if (turn.trace.ok &&
		    (turn.message.tool.name == "browser.overlays" ||
		     turn.message.tool.name == "browser.snapshot")) {
			auto *frames =
				array ? array :
					(object ? json_array_node((
							  *object)["iframes"]) :
						  NULL);
			browser_evidence.suspect_overlay(
				suspected_frame(frames));
			if (!browser_evidence.experiment_target().empty()) {
				result.get_root().add_text(
					"runtime_diagnostic_hypothesis",
					browser_evidence.overlay_guidance()
						.c_str());
				result.get_root().add_text(
					"runtime_experiment_target",
					browser_evidence.experiment_target()
						.c_str());
				turn.tool_result =
					serialize_json(result.get_root());
			}
		}
		browser_evidence.observe(
			turn.message.tool.name, turn.message.tool.query,
			observed,
			object ? json_text((*object)["patch_id"]) : "",
			object ? json_text((*object)["undone"]) : "",
			turn.trace.ok);
		if (turn.trace.ok &&
		    turn.message.tool.name == "browser.patch_style") {
			// Inspect the patched selector automatically, including after undo.
			// The model may inspect a different region to judge the visual symptom.
			const auto inspection = probe_browser_experiment();
			result.get_root().add_text("runtime_inspection",
						   inspection.c_str());
			result.get_root().add_text(
				"runtime_evidence_note",
				"Automatic inspection of the experiment selector. Style/geometry evidence only; assess the visible symptom separately.");
			turn.tool_result = serialize_json(result.get_root());
		}
	}
	// Prepare an official Responses continuation for the next turn. If a
	// gateway omitted response/call IDs or returned an unexpected batch shape,
	// leave these fields empty and safely fall back to the compact transcript.
	provider_response_id.clear();
	provider_tool_outputs.clear();
	provider_response_pending = false;
	turn.have_native_outputs =
		turn.output.native_tool_call &&
		build_native_tool_outputs(turn.output.tool_calls,
					  turn.tool_result,
					  turn.current_tool_outputs);
	if (provider.protocol == "openai_responses" && !stateless_responses &&
	    provider.responses_store && turn.output.native_tool_call &&
	    !turn.output.response_id.empty() &&
	    completion_calls_have_ids(turn.output.tool_calls) &&
	    turn.have_native_outputs) {
		provider_response_id = turn.output.response_id;
		provider_tool_outputs = turn.current_tool_outputs;
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		runtime_task->provider_response_id = provider_response_id;
		runtime_task->provider_response_pending = false;
		runtime_task->provider_tool_outputs = provider_tool_outputs;
	} else if ((provider.protocol != "openai_responses" ||
		    stateless_responses) &&
		   turn.have_native_outputs) {
		webcool::ai::completion_tool_exchange_t exchange;
		exchange.preceding_instructions = turn.input.turn_instructions;
		exchange.assistant_text = turn.output.text;
		exchange.reasoning_content = turn.output.reasoning;
		exchange.calls = turn.output.tool_calls;
		exchange.outputs = turn.current_tool_outputs;
		provider_tool_history.push_back(exchange);
	}
}

void coding_tool_loop_t::materialize_tool_changes(coding_turn_t &turn)
{
	turn.staged_fingerprint_after = staged_change_fingerprint(changes);
	turn.staged_progress =
		turn.staged_fingerprint_before != turn.staged_fingerprint_after;
	turn.validation_unavailable =
		turn.message.tool.name == "workspace.validate" &&
		turn.tool_result.find("\"commands_executed\":0") !=
			std::string::npos;
	turn.validation_failed =
		turn.message.tool.name == "workspace.validate" &&
		turn.tool_result.find("\"validation_passed\":false") !=
			std::string::npos &&
		turn.tool_result.find("\"commands_executed\":0") ==
			std::string::npos;
	if (turn.staged_progress && turn.trace.ok) {
		has_task_revision = true;
		supplementary_read_calls = 0;
	}
	if (turn.staged_progress) {
		// Use the pre-review snapshot as the generation baseline. Although a
		// resolved proposal is no longer part of the active overlay, a later edit
		// of that path must advance its generation and become pending again.
		webcool::ai::assign_agent_review_generations(
			turn.review_generation_baseline, changes);
		// The durable worktree is the authoritative draft filesystem. Rebuild it
		// from the formal baseline plus the complete accumulated proposal after
		// every mutation, never from only the latest tool call. If materializing
		// fails, roll this tool back in memory as well so the model cannot continue
		// from a revision that does not exist on disk.
		size_t skipped_files = 0;
		std::string draft_err;
		if (!materialize_staged_worktree(runtime_task->user_root,
						 project_path, runtime_task->id,
						 changes, skipped_files,
						 draft_err)) {
			changes = turn.changes_before_tool;
			turn.staged_progress = false;
			turn.trace.ok = false;
			traces.back().ok = false;
			turn.tool_result = tool_error_json(draft_err);
			webcool::ai::ai_log_error(
				"agent.runtime",
				"materialize-incremental-draft", draft_err);
		}
	}
	// Mutations invalidate old source pages before another compaction can
	// present stale text as the current file. Reads carry a full-file hash.
	if (turn.staged_progress) {
		for (const auto &change : changes) {
			bool unchanged = false;
			for (const auto &previous : turn.changes_before_tool) {
				if (previous.path == change.path &&
				    previous.operation == change.operation &&
				    previous.target_path ==
					    change.target_path &&
				    previous.content == change.content) {
					unchanged = true;
					break;
				}
			}
			if (unchanged)
				continue;
			read_context.erase(change.path);
			if (!change.target_path.empty())
				read_context.erase(change.target_path);
		}
		for (const auto &previous : turn.changes_before_tool) {
			bool retained = false;
			for (const auto &change : changes)
				if (previous.path == change.path) {
					retained = true;
					break;
				}
			if (!retained) {
				read_context.erase(previous.path);
				if (!previous.target_path.empty())
					read_context.erase(
						previous.target_path);
			}
		}
	}
}

void coding_tool_loop_t::observe_tool_progress(coding_turn_t &turn)
{
	std::vector<std::string> priority_paths;
	for (const auto &change : changes)
		priority_paths.push_back(change.path);
	for (const auto &source : repair_required_reads)
		priority_paths.push_back(source.first);
	read_context.prioritize(priority_paths);
	const std::string tool_signature =
		turn.message.tool.name + "\n" + turn.message.tool.path + "\n" +
		turn.message.tool.query + "\n" + turn.message.tool.old_text +
		"\n" + turn.message.tool.target_path + "\n" +
		webcool::ai::agent_workspace_t::content_sha256(
			turn.message.tool.content);
	if (turn.trace.ok && repair_review_read_only(turn.message.tool) &&
	    read_result_is_retained(turn.message.tool.name, turn.tool_result,
				    read_context)) {
		next_tool_guidance +=
			"\n<duplicate_read>The returned version and page were already retained. "
			"Reuse these pages; continue with next_content/next_query only if source is missing, "
			"then submit the requested changes.</duplicate_read>\n";
		append_simple_operation_event(
			runtime_task, "duplicate_read_observed",
			turn.message.tool.name, turn.message.tool.path,
			executed_tool_calls);
	}
	turn.reads_retained =
		turn.have_native_outputs && !turn.current_tool_outputs.empty();
	if (turn.have_native_outputs) {
		for (size_t i = 0; i < turn.current_tool_outputs.size(); ++i) {
			const bool retained = remember_read_result(
				turn.output.tool_calls[i].name,
				turn.current_tool_outputs[i].output,
				read_context);
			turn.reads_retained = turn.reads_retained && retained;
		}
	} else {
		turn.reads_retained = remember_read_result(
			turn.message.tool.name, turn.tool_result, read_context);
	}
	if (turn.reads_retained) {
		if (turn.have_native_outputs) {
			for (size_t i = 0; i < turn.current_tool_outputs.size();
			     ++i)
				turn.reads_retained =
					read_result_is_retained(
						turn.output.tool_calls[i].name,
						turn.current_tool_outputs[i]
							.output,
						read_context) &&
					turn.reads_retained;
		} else
			turn.reads_retained = read_result_is_retained(
				turn.message.tool.name, turn.tool_result,
				read_context);
	}
	turn.draft_cycle =
		turn.trace.ok && turn.staged_progress &&
		draft_cycles.observe(
			staged_cycle_fingerprint(turn.changes_before_tool),
			staged_cycle_fingerprint(changes));
	if (turn.draft_cycle) {
		acl::json cycle_json;
		acl::json_node &cycle_event = cycle_json.create_node();
		cycle_event.add_text("event", "draft_cycle_detected");
		cycle_event.add_text("draft_sha256",
				     staged_cycle_fingerprint(changes).c_str());
		cycle_event.add_text("tool", turn.message.tool.name.c_str());
		append_runtime_operation_event(runtime_task, cycle_event);
	}
	std::vector<std::string> observations;
	if (turn.have_native_outputs) {
		for (size_t i = 0; i < turn.current_tool_outputs.size(); ++i)
			collect_observation_signatures(
				turn.output.tool_calls[i].name,
				turn.current_tool_outputs[i].output,
				observations);
	} else {
		collect_observation_signatures(turn.message.tool.name,
					       turn.tool_result, observations);
	}
	turn.progress_decision = progress_supervisor.observe_many(
		observations, turn.trace.ok && !turn.draft_cycle,
		turn.staged_progress && !turn.draft_cycle);
	progress_supervisor.observe_investigation(turn.provider_tool_call_count,
						  turn.staged_progress &&
							  !turn.draft_cycle);
	collect_proposal_failure_causes(turn.tool_result,
					turn.proposal_failures);
	turn.repeated_proposal_failure =
		progress_supervisor.observe_proposal_failures(
			turn.proposal_failures, turn.staged_progress);
	if (turn.repeated_proposal_failure)
		turn.progress_decision = webcool::ai::agent_progress_stop;
	else if (!turn.proposal_failures.empty())
		turn.progress_decision = webcool::ai::agent_progress_replan;
	acl::json progress_json;
	acl::json_node &progress_event = progress_json.create_node();
	progress_event.add_text("event", "progress_guard_observed");
	progress_event.add_text("tool", turn.message.tool.name.c_str());
	if (!turn.trace.path.empty()) {
		progress_event.add_text("path", turn.trace.path.c_str());
	}
	progress_event.add_text(
		"tool_signature_sha256",
		webcool::ai::agent_workspace_t::content_sha256(tool_signature)
			.c_str());
	progress_event.add_number("calls_without_revision",
				  progress_supervisor.investigation_calls());
	progress_event.add_bool("repeated_proposal_failure",
				turn.repeated_proposal_failure);
	progress_event.add_bool("tool_ok", turn.trace.ok);
	progress_event.add_bool("draft_changed", turn.staged_progress);
	progress_event.add_bool("returned_to_previous_draft", turn.draft_cycle);
	progress_event.add_number("individual_observations",
				  observations.size());
	progress_event.add_text(
		"evidence_sha256",
		webcool::ai::agent_workspace_t::content_sha256(turn.tool_result)
			.c_str());
	progress_event.add_number("pending_change_count",
				  static_cast<long long>(changes.size()));
	progress_event.add_number(
		"consecutive_without_draft_change",
		static_cast<long long>(
			progress_supervisor.consecutive_no_progress()));
	progress_event.add_text(
		"decision",
		turn.progress_decision == webcool::ai::agent_progress_replan ?
			"replan" :
		turn.progress_decision == webcool::ai::agent_progress_stop ?
			"stop" :
			"continue");
	append_runtime_operation_event(runtime_task, progress_event);
}

} // namespace agent_detail
} // namespace action
