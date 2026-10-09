#include "stdafx.h"
// Repair convergence, durable proposals and validated completion.
#include "coding_tool_loop_internal.h"

namespace action
{
namespace agent_detail
{

void coding_tool_loop_t::track_repair_edits(coding_turn_t &turn)
{
	if (!turn.staged_progress)
		return;
	for (const auto &change : changes) {
		bool changed = true;
		for (const auto &before : turn.changes_before_tool) {
			if (!(before.path == change.path &&
			        before.content == change.content &&
			        before.operation == change.operation))
				continue;

			changed = false;
			break;
		}
		if (changed)
			repair_changed_paths.insert(change.path);
		if (!(changed && change.operation == "write" &&
		        last_validation_report.find(
		            "\"validation_passed\":false") !=
		            std::string::npos &&
		        (pending_repair_edits.count(change.path) ||
		            pending_repair_edits.size() < 3)))
			continue;
		for (const auto &before : turn.changes_before_tool) {
			if (!(before.path == change.path &&
			        before.operation == "write"))
				continue;

			const size_t at = webcool::ai::first_edit_offset(
			    before.content, change.content);
			pending_repair_edits[change.path] = {
				webcool::ai::edit_excerpt(
				    before.content, at, 768)
				    .content,
				webcool::ai::edit_excerpt(
				    change.content, at, 768)
				    .content
			};
			break;
		}
	}
}

void coding_tool_loop_t::build_repair_focus(coding_turn_t &turn)
{
	acl::json focus_json;
	acl::json_node &focus = focus_json.create_node();
	acl::json failure_report(turn.tool_result.c_str());
	if (failure_report.finish()) {
		const std::string failed =
		    json_text(failure_report["failed_command_id"]);
		focus.add_text("failed_command", failed.c_str());
		acl::json_node *cmds =
		    json_array_node(failure_report["commands"]);
		for (acl::json_node *item = cmds ? cmds->first_child() : NULL;
		     item; item = cmds->next_child()) {
			acl::json_node *command =
			    item->is_object() ? item : item->get_obj();
			if (!(command &&
			        json_text((*command)["command_id"]) == failed))
				continue;
			focus.add_text("observed_failure_excerpt",
			    webcool::ai::failure_examples(
			        json_text((*command)["diagnostic"]))
			        .c_str());
		}
	}
	acl::json_node &edits = focus_json.create_array();
	focus.add_child("last_edits", edits);
	auto add_failures = [&](const char *name,
	                        const std::set<std::string> &values) {
		acl::json_node &list = focus_json.create_array();
		focus.add_child(name, list);
		size_t count = 0;
		for (const auto &value : values) {
			if (++count > 12)
				break;
			list.add_array_text(value.c_str());
		}
	};
	add_failures("still_reported", repair_failures.persisting());
	add_failures("newly_reported", repair_failures.newly_reported());
	add_failures("no_longer_reported_not_proven_fixed",
	    repair_failures.no_longer_reported());
	add_failures("returned_failures", repair_failures.returned_failures());
	focus.add_bool(
	    "persistent_test_identities", repair_failures.stalled_tests());
	if (repair_failures.alternating()) {
		acl::json_node &history = focus_json.create_array();
		focus.add_child("alternating_failure_evidence", history);
		size_t count = 0;
		for (const auto &failure :
		    repair_failures.alternating_evidence()) {
			if (++count > 16)
				break;
			history.add_array_text(failure.c_str());
		}
	}
	if (repair_failures.regressed()) {
		focus.add_number("previous_failure_signatures",
		    repair_failures.previous_count());
		focus.add_number("current_failure_signatures",
		    repair_failures.current_count());
		acl::json_node &added = focus_json.create_array();
		focus.add_child("new_failure_diagnostics", added);
		size_t count = 0;
		for (const auto &failure : repair_failures.added_failures()) {
			if (++count > 8)
				break;
			added.add_array_text(failure.c_str());
		}
	}
	for (const auto &edit : pending_repair_edits) {
		acl::json_node &record = edits.add_child(false, true);
		record.add_text("path", edit.first.c_str());
		record.add_text("before_excerpt", edit.second.first.c_str());
		record.add_text("after_excerpt", edit.second.second.c_str());
	}
	turn.repair_focus =
	    (repair_failures.regressed() ?
	            std::string(
	                prompt_text(prompt_id::repair_regression, chinese)) :
	            "") +
	    std::string(prompt_text(prompt_id::repair_failure_focus, chinese)) +
	    "\n<repair_failure_focus>\n" + serialize_json(focus) +
	    "\n</repair_failure_focus>\n";
	turn.repair_focus = std::string(prompt_text(
	                        prompt_id::repair_contract_review, chinese)) +
	    turn.repair_focus;
	// Keep evidence for persistent and returning failures too, not
	// just exact A/B/A cycles. Preserve the rationale across reviews.
	unresolved_repair_contract = turn.repair_focus;
}

void coding_tool_loop_t::select_repair_sources(coding_turn_t &turn)
{
	std::set<std::string> tests;
	for (const auto &change : changes) {
		if (!(change.operation == "write" &&
		        (change.path.find("/tests/") != std::string::npos ||
		            change.path.find("/test/") != std::string::npos)))
			continue;
		tests.insert(change.path);
	}
	acl::json report(turn.tool_result.c_str());
	std::string diagnostic;
	std::set<std::string> related_files;
	bool related_complete = false;
	if (report.finish()) {
		acl::json_node *commands = json_array_node(report["commands"]);
		for (acl::json_node *item = commands ? commands->first_child() :
		                                       NULL;
		     item; item = commands->next_child()) {
			acl::json_node *command =
			    item->is_object() ? item : item->get_obj();
			if (!(command &&
			        json_text((*command)["command_id"]) ==
			            json_text(report["failed_command_id"])))
				continue;
			diagnostic += json_text((*command)["diagnostic"]);
			related_complete = json_bool(
			    (*command)["failed_test_files_complete"], false);
			acl::json_node *related =
			    json_array_node((*command)["failed_test_files"]);
			for (acl::json_node *file =
			         related ? related->first_child() : NULL;
			     file; file = related->next_child())
				related_files.insert(
				    project_path + "/" + json_text(file));
		}
	}
	auto selected = related_complete ?
	    related_files :
	    webcool::ai::repair_test_sources(diagnostic, tests);
	selected.insert(related_files.begin(), related_files.end());
	webcool::ai::agent_workspace_t review_workspace(persistent_draft_root(
	    runtime_task->user_root, project_path, runtime_task->id));
	for (const auto &file : related_files) {
		std::string relative, content, read_err;
		bool truncated = false;
		if (!(project_path_to_draft_path(
		          project_path, file, relative) &&
		        review_workspace.read(
		            relative, content, truncated, read_err) &&
		        !truncated))
			continue;
		repair_required_reads[file] =
		    webcool::ai::agent_workspace_t::content_sha256(content);
	}
	for (const auto &change : changes) {
		if (!(change.operation == "write" &&
		        (repair_changed_paths.count(change.path) ||
		            selected.count(change.path) ||
		            ((repair_failures.alternating() ||
		                 repair_failures.stalled_tests() ||
		                 !repair_failures.returned_failures()
		                      .empty()) &&
		                (change.path.find("/src/") !=
		                        std::string::npos ||
		                    tests.count(change.path) ||
		                    change.path ==
		                        project_path + "/CMakeLists.txt")))))
			continue;
		repair_required_reads[change.path] =
		    webcool::ai::agent_workspace_t::content_sha256(
		        change.content);
	}
}

void coding_tool_loop_t::track_repair_failures(coding_turn_t &turn)
{
	track_repair_edits(turn);
	turn.reconsider_repair = !turn.repair_blocked &&
	    turn.message.tool.name == "workspace.validate" &&
	    repeated_repair_failure(turn.tool_result,
	        turn.staged_fingerprint_after, repair_failures);
	if (turn.message.tool.name == "workspace.validate" &&
	    !turn.repair_blocked) {
		if (turn.reconsider_repair) {
			build_repair_focus(turn);
			select_repair_sources(turn);
		}
		repair_changed_paths.clear();
		pending_repair_edits.clear();
		last_validation_report = turn.tool_result;
		record_acceptance_evidence(turn.tool_result);
		acl::json evidence_json;
		acl::json_node &evidence = evidence_json.create_node();
		evidence.add_text("event", "validation_completed");
		evidence.add_text("report", turn.tool_result.c_str());
		evidence.add_text("draft_sha256",
		    webcool::ai::agent_workspace_t::content_sha256(
		        turn.staged_fingerprint_after)
		        .c_str());
		append_runtime_operation_event(runtime_task, evidence);
	}
	if (turn.staged_progress) {
		proposal_only_turn = false;
		requirement_progress_json.clear();
	}
	if (turn.message.tool.name == "workspace.validate" &&
	    turn.tool_result.find("\"validation_passed\":true") !=
	        std::string::npos) {
		last_validated_fingerprint = turn.staged_fingerprint_after;
		last_validation_passed = true;
		draft_cycles.clear();
	}
}

void coding_tool_loop_t::persist_staged_result(coding_turn_t &turn)
{
	if (turn.trace.ok && turn.staged_progress) {
		// Persist every incremental snapshot under the project's private agent
		// directory. A browser refresh cannot lose already generated code, and no
		// formal source path is touched before review acceptance.
		webcool::ai::agent_result_t staged_result;
		staged_result.run_id = runtime_task->id;
		staged_result.project_path = project_path;
		staged_result.session_id = runtime_task->session_id;
		staged_result.text = "智能体仍在运行；已保存增量文件提案。";
		staged_result.reasoning =
		    runtime_reasoning_snapshot(runtime_task);
		staged_result.changes = changes;
		staged_result.decision = "pending";
		staged_result.saved_at = static_cast<long long>(time(NULL));
		webcool::ai::agent_result_store_t staged_store(
		    runtime_task->user_root, project_path);
		std::string staged_save_err;
		if (!staged_store.save(staged_result, staged_save_err)) {
			webcool::ai::ai_log_error("agent.runtime",
			    "persist-incremental-proposal", staged_save_err);
		} else {
			// A user may accept an earlier generation while the model is still
			// running. Reload the atomically merged artifact before publishing it,
			// otherwise this worker's stale in-memory "pending" value could make the
			// accepted badge reappear until the next refresh.
			webcool::ai::agent_result_t merged_staged_result;
			bool merged_found = false;
			std::string merged_load_err;
			if (staged_store.load(runtime_task->id,
			        merged_staged_result, merged_found,
			        merged_load_err) &&
			    merged_found) {
				changes = merged_staged_result.changes;
				staged_result = merged_staged_result;
			} else if (!merged_load_err.empty()) {
				webcool::ai::ai_log_error("agent.runtime",
				    "reload-incremental-review-state",
				    merged_load_err);
			}
			publish_runtime_staged_changes(
			    runtime_task, staged_result.changes);
			set_runtime_result_artifact(runtime_task,
			    staged_store.relative_path(runtime_task->id),
			    "pending");
			acl::json staged_json;
			acl::json_node &staged_event =
			    staged_json.create_node();
			staged_event.add_text(
			    "event", "staged_revision_persisted");
			staged_event.add_text("path", turn.trace.path.c_str());
			staged_event.add_number("pending_change_count",
			    static_cast<long long>(
			        staged_result.changes.size()));
			staged_event.add_number("reasoning_snapshot_bytes",
			    static_cast<long long>(
			        staged_result.reasoning.size()));
			append_runtime_operation_event(
			    runtime_task, staged_event);
		}
		// Count proposal publication as live workspace progress without claiming
		// that the formal source tree changed.
		if (changes.size() != turn.staged_before) {
			update_runtime_progress(runtime_task, "staged_change",
			    turn.trace.path, executed_tool_calls);
		}
	}
	complete_runtime_tool(runtime_task, turn.trace, executed_tool_calls);
	if (turn.validation_unavailable) {
		// Missing validators are a capability limitation, not task completion.
		// Retain the result for this exact revision, hide the unavailable tool,
		// and let the model continue reading and proposing implementation work.
		unavailable_validation.fingerprint =
		    turn.staged_fingerprint_after;
		unavailable_validation.report = turn.tool_result;
		last_validated_fingerprint = turn.staged_fingerprint_after;
		last_validation_passed = false;
		proposal_only_turn = false;
	}
}

coding_tool_loop_t::step_t coding_tool_loop_t::finish_validated_result(
    coding_turn_t &turn)
{
	const bool verified_batch = turn.trace.ok &&
	    !turn.batch_validation.report.empty() &&
	    turn.batch_validation.draft == staged_cycle_fingerprint(changes) &&
	    turn.batch_validation.baseline ==
	        validation_baseline_fingerprint(workspace, project_path);
	if (verified_batch) {
		last_validation_report = turn.batch_validation.report;
		record_acceptance_evidence(turn.batch_validation.report);
		last_validated_fingerprint = staged_change_fingerprint(changes);
		last_validation_passed = true;
		draft_cycles.clear();
		acl::json evidence_json;
		acl::json_node &evidence = evidence_json.create_node();
		evidence.add_text("event", "validation_completed");
		evidence.add_text("origin", "provider_batch");
		evidence.add_bool("reused_for_completion", true);
		evidence.add_text(
		    "report", turn.batch_validation.report.c_str());
		evidence.add_text(
		    "draft_sha256", turn.batch_validation.draft.c_str());
		append_runtime_operation_event(runtime_task, evidence);
	}
	std::string completion_validation =
	    verified_batch ? turn.batch_validation.report : turn.tool_result;
	bool automatic_compile_check = false;
	if (!verified_batch && turn.trace.ok && turn.staged_progress &&
	    !changes.empty()) {
		const bool focused =
		    webcool::ai::focused_compile_repair(original_prompt);
		agent_tool_trace_t validation_trace{};
		validation_trace.name = "workspace.validate";
		validation_trace.path = project_path;
		update_runtime_progress(runtime_task, "tool_call",
		    "workspace.validate", executed_tool_calls);
		completion_validation = validate_staged_draft(
		    runtime_task->user_root, project_path, runtime_task->id,
		    changes, sandbox_limits, validation_trace, NULL, !focused);
		traces.push_back(validation_trace);
		complete_runtime_tool(
		    runtime_task, validation_trace, executed_tool_calls);
		automatic_compile_check = validation_trace.ok;
		record_acceptance_evidence(completion_validation);
		// Build-only success cannot stand in for the task's final validation.
		if (focused) {
			last_validation_report = completion_validation;
			record_acceptance_evidence(completion_validation);
			last_validated_fingerprint =
			    staged_change_fingerprint(changes);
			last_validation_passed =
			    completion_validation.find(
			        "\"validation_passed\":true") !=
			    std::string::npos;
		}
		acl::json event;
		auto &node = event.create_node();
		node.add_text("event", "validation_completed");
		node.add_text("origin",
		    focused ? "focused_compile_repair" : "automatic_build");
		node.add_text("report", completion_validation.c_str());
		append_runtime_operation_event(runtime_task, node);
		const std::string build_feedback = "\n<automatic_build>" +
		    completion_validation +
		    "\nIf compilation failed, repair the reported compiler diagnostics before continuing. "
		    "Do not remove the build script, suppress its error status, or expand scope to unrelated issues. "
		    "A successful build does not authorize unrelated work.</automatic_build>\n";
		next_tool_guidance += build_feedback;
		// Keep evidence in the durable tool transcript as well as the next request.
		turn.automatic_build_feedback = build_feedback;
	}
	const bool compile_completed =
	    webcool::ai::focused_compile_repair(original_prompt) &&
	    completion_validation.find("\"compile_repair_completed\":true") !=
	        std::string::npos;
	const bool validation_passed =
	    (turn.message.tool.name == "workspace.validate" ||
	        verified_batch) &&
	    turn.trace.ok &&
	    (!changes.empty() ||
	        webcool::ai::verification_only_task(original_prompt)) &&
	    completion_validation.find("\"validation_passed\":true") !=
	        std::string::npos;
	if (!(validation_passed ||
	        (compile_completed && turn.trace.ok &&
	            (automatic_compile_check || verified_batch ||
	                turn.message.tool.name == "workspace.validate"))))
		return step_t::proceed;
	if (!compile_completed &&
	    !webcool::ai::verification_only_task(original_prompt) &&
	    !acceptance_review_requested &&
	    executed_tool_calls < effective_tool_call_limit) {
		acceptance_review_requested = true;
		next_tool_guidance +=
		    "\n<acceptance_review>Configured checks passed for this revision. Before ending, map each user request "
		    "ID to observable evidence, distinguishing tested behavior from inferred behavior. Inspect uncovered "
		    "input/state/output paths or add a targeted behavioral regression test when feasible. "
		    "Do not repeat an unchanged passing build. If required runtime/browser checks are unavailable, "
		    "finish with the exact unverified requirements and next verification step. Do not claim full task "
		    "completion merely from these checks. Return final JSON with requirement_progress covering each requested behavior, "
		    "its implementation status, supporting changes and remaining work. This is one bounded acceptance review.</acceptance_review>\n";
		return step_t::proceed;
	}
	// A staged revision plus a successful fixed validator is the durable end
	// condition for one agent turn. Do not ask the model for a ceremonial
	// summary: compatible providers can forget compacted tool history and
	// restart analysis, wasting the remainder of the tool budget.
	final_output = turn.output;
	final_output.text =
	    successful_validation_summary(completion_validation, chinese) +
	    "。";
	final_output.reasoning = accumulated_reasoning;
	final_output.input_tokens = total_input_tokens;
	final_output.cached_input_tokens = total_cached_input_tokens;
	final_output.output_tokens = total_output_tokens;
	final_output.reasoning_tokens = total_reasoning_tokens;
	final_output.latency_ms = total_latency_ms;
	rejected_changes =
	    validate_change_proposals(workspace, project_path, changes);
	memory_summary =
	    deterministic_delivery_summary(original_prompt, changes,
	        successful_validation_summary(completion_validation, chinese));
	return step_t::completed;
}

} // namespace agent_detail
} // namespace action
