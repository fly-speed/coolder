#include "stdafx.h"
// Repair convergence, durable proposals and validated completion.
#include "coding_tool_loop_internal.h"
#include "../context/coding_test_changes.h"
#include "../context/compile_failure_cache.h"

namespace action
{
namespace agent_detail
{

bool coding_tool_loop_t::current_cached_compile_failure()
{
	if (!libai::ai::deterministic_compile_failure(validation_cache.report) ||
	    validation_cache.execution_revision != runtime_task->coding_execution_revision ||
	    validation_cache.fingerprint != validation_baseline_fingerprint(workspace, project_path) +
	        "\n" + staged_change_fingerprint(changes)) return false;
	libai::ai::agent_workspace_t draft(persistent_draft_root(runtime_task->user_root, project_path, runtime_task->id));
	if (validation_cache.draft_fingerprint.empty() ||
	    validation_cache.draft_fingerprint != validation_baseline_fingerprint(draft, "")) return false;
	std::string receipt_error;
	return validate_cached_command_receipts(runtime_task->user_root, project_path,
	    validation_cache.report, receipt_error);
}

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

			const size_t at = libai::ai::first_edit_offset(
			    before.content, change.content);
			pending_repair_edits[change.path] = {
				libai::ai::edit_excerpt(
				    before.content, at, 768)
				    .content,
				libai::ai::edit_excerpt(
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
	acl::json failure_report((turn.repair_report.empty() ? turn.tool_result : turn.repair_report).c_str());
	if (failure_report.finish()) {
		const std::string failed =
		    json_text(failure_report["failed_command_id"]);
		focus.add_text("failed_command", failed.c_str());
		acl::json_node *cmds =
		    json_array_node(failure_report["commands"]);
		auto &reported = focus_json.create_array();
		focus.add_child("failed_commands", reported);
		for (acl::json_node *item = cmds ? cmds->first_child() : NULL;
		     item; item = cmds->next_child()) {
			acl::json_node *command =
			    item->is_object() ? item : item->get_obj();
			if (!(command && (json_text((*command)["command_id"]) == failed ||
			        (!json_bool((*command)["passed"], true) &&
			            libai::ai::compile_command(json_text((*command)["command_id"]))))))
				continue;
			auto &record = reported.add_child(false, true);
			record.add_text("command_id", json_text((*command)["command_id"]).c_str());
			record.add_text("working_directory", json_text((*command)["working_directory"]).c_str());
			if ((*command)["arguments"]) record.add_text("arguments_json", serialize_json(*(*command)["arguments"]).c_str());
			record.add_text("observed_failure_excerpt", libai::ai::failure_examples(json_text((*command)["diagnostic"])).c_str());
			if (auto *diagnostics=json_array_node((*command)["diagnostics"]))
				record.add_child("diagnostics",focus_json.duplicate_node(*diagnostics));
		}
	}
	focus.add_text("repair_scope","Repair the reported errors together with their required imports, definitions and callers in one related edit. These are observed diagnostics, not proof that later compilation has no additional errors.");
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
	acl::json report((turn.repair_report.empty() ? turn.tool_result : turn.repair_report).c_str());
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
			if (!(command && (json_text((*command)["command_id"]) == json_text(report["failed_command_id"]) ||
			        (!json_bool((*command)["passed"], true) &&
			            libai::ai::compile_command(json_text((*command)["command_id"]))))))
				continue;
			diagnostic += json_text((*command)["diagnostic"]);
			auto *diagnostics = json_array_node((*command)["diagnostics"]);
			for (auto *entry = diagnostics ? diagnostics->first_child() : NULL;
			     entry; entry = diagnostics->next_child()) {
				auto *object = entry->is_object() ? entry : entry->get_obj();
				const auto path = object ? json_text((*object)["path"]) : "";
				if (!path.empty() && path[0] != '/' && path.find("..") == std::string::npos &&
				    path.find('\\') == std::string::npos)
					related_files.insert(project_path + "/" + path);
			}
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
	    libai::ai::repair_test_sources(diagnostic, tests);
	selected.insert(related_files.begin(), related_files.end());
	libai::ai::agent_workspace_t review_workspace(persistent_draft_root(
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
		    libai::ai::agent_workspace_t::content_sha256(content);
	}
	std::set<std::string> failure_directories;
	for (const auto &file : selected) failure_directories.insert(file.substr(0, file.find_last_of('/')));
	for (const auto &file : related_files) failure_directories.insert(file.substr(0, file.find_last_of('/')));
	for (const auto &change : changes) {
		if (!(change.operation == "write" &&
		        (failure_directories.count(change.path.substr(0, change.path.find_last_of('/'))) ||
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
		    libai::ai::agent_workspace_t::content_sha256(
		        change.content);
	}
}

void coding_tool_loop_t::review_validation_failure(coding_turn_t &turn, const std::string &report)
{
	acl::json json(report.c_str());
	if (!json.finish() || json_bool(json["validation_passed"], true) ||
	    json_number(json["commands_executed"], 0) == 0) return;
	turn.repair_report = report;
	turn.validation_failed = true;
	const bool repeated = repeated_repair_failure(report, turn.staged_fingerprint_after, repair_failures);
	turn.reconsider_repair = true;
	build_repair_focus(turn);
	select_repair_sources(turn);
	if (repeated)
		turn.repair_focus += "\n<repair_reassessment>The same failure survived a changed revision. "
		    "Before another patch, reproduce the failing case and compare actual/expected state, return values and side effects. "
		    "Use a different evidenced fix, not another speculative rewrite. A false return can be the expected terminal "
		    "result; assert the resulting state separately and check the API contract before changing tests. A timeout names unfinished work, "
		    "not a package that already printed ok. Keep any test correction justified by the requested behavior.</repair_reassessment>\n";
}

void coding_tool_loop_t::track_repair_failures(coding_turn_t &turn)
{
	track_repair_edits(turn);
	if (turn.message.tool.name == "workspace.validate" &&
	    !turn.repair_blocked) {
		review_validation_failure(turn, turn.tool_result);
		repair_changed_paths.clear();
		pending_repair_edits.clear();
		last_validation_report = turn.tool_result;
		record_acceptance_evidence(turn.tool_result);
		acl::json evidence_json;
		acl::json_node &evidence = evidence_json.create_node();
		evidence.add_text("event", "validation_completed");
		evidence.add_text("report", turn.tool_result.c_str());
		evidence.add_text("draft_sha256",
		    libai::ai::agent_workspace_t::content_sha256(
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
		libai::ai::agent_result_t staged_result;
		staged_result.run_id = runtime_task->id;
		staged_result.project_path = project_path;
		staged_result.session_id = runtime_task->session_id;
		staged_result.text = "智能体仍在运行；已保存增量文件提案。";
		staged_result.reasoning =
		    runtime_reasoning_snapshot(runtime_task);
		staged_result.changes = changes;
		staged_result.decision = "pending";
		staged_result.saved_at = static_cast<long long>(time(NULL));
		libai::ai::agent_result_store_t staged_store(
		    runtime_task->user_root, project_path);
		std::string staged_save_err;
		if (!staged_store.save(staged_result, staged_save_err)) {
			libai::ai::ai_log_error("agent.runtime",
			    "persist-incremental-proposal", staged_save_err);
		} else {
			// A user may accept an earlier generation while the model is still
			// running. Reload the atomically merged artifact before publishing it,
			// otherwise this worker's stale in-memory "pending" value could make the
			// accepted badge reappear until the next refresh.
			libai::ai::agent_result_t merged_staged_result;
			bool merged_found = false;
			std::string merged_load_err;
			if (staged_store.load(runtime_task->id,
			        merged_staged_result, merged_found,
			        merged_load_err) &&
			    merged_found) {
				changes = merged_staged_result.changes;
				staged_result = merged_staged_result;
			} else if (!merged_load_err.empty()) {
				libai::ai::ai_log_error("agent.runtime",
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
	acl::json tool_result(turn.tool_result.c_str());
	if (turn.output.response_status == "completed" &&
	    turn.output.native_tool_call && turn.output.tool_calls.size() == 1 &&
	    turn.message.tool.name == "workspace.propose_batch" &&
	    !turn.trace.ok && !turn.repair_blocked &&
	    tool_result.finish() &&
	    json_text(tool_result["code"]) == "proposal_batch_unparsed") {
		// Completion belongs to this model round, not to source generation or
		// business acceptance. Retain the exact payload instead of regenerating
		// a large source batch merely because its nested content is not JSON.
		acl::json archived;
		auto &record = archived.create_node();
		record.add_number("version", 1);
		record.add_text("kind", "unparsed_completed_proposal_batch");
		record.add_text("run_id", runtime_task->id.c_str());
		record.add_text("response_status", "completed");
		record.add_text("tool", turn.message.tool.name.c_str());
		record.add_text("raw_arguments_json", turn.output.tool_calls[0].arguments_json.c_str());
		record.add_text("content", turn.message.tool.content.c_str());
		record.add_text("content_sha256",
		    libai::ai::agent_workspace_t::content_sha256(turn.message.tool.content).c_str());
		record.add_text("parse_error", json_text(tool_result["error"]).c_str());
		record.add_bool("executed", false);
		record.add_number("existing_staged_changes", changes.size());
		libai::ai::agent_request_store_t store(runtime_task->user_root, project_path, runtime_task->id);
		std::string relative_path;
		if (!store.append_response(serialize_json(record), relative_path, err))
			return step_t::failed;
		acl::json event_json;
		auto &event = event_json.create_node();
		event.add_text("event", "completed_batch_content_recorded");
		event.add_text("tool", turn.message.tool.name.c_str());
		event.add_text("response_archive", relative_path.c_str());
		event.add_text("parse_status", "unparsed");
		event.add_bool("round_completed", true);
		event.add_bool("executed", false);
		event.add_bool("source_changed", false);
		event.add_number("content_bytes", turn.message.tool.content.size());
		event.add_number("existing_staged_changes", changes.size());
		append_runtime_operation_event(runtime_task, event);
		final_output = turn.output;
		final_output.text = chinese ?
		    "本轮模型响应已结束。批次 content 未能解析为文件清单，原始内容已保留记录；本轮未据此暂存或写入文件，也未确认项目实现完成。" :
		    "This model round has ended. Batch content could not be parsed as a file list and was recorded exactly; it staged or wrote no files and does not establish project completion.";
		final_output.text += "\n" + relative_path;
		if (!changes.empty()) final_output.text += chinese ?
		    "\n此前已暂存的修改仍保留，验证和审查状态以现有记录为准。" :
		    "\nEarlier staged changes remain available with their existing validation and review state.";
		final_output.reasoning = accumulated_reasoning;
		final_output.input_tokens = total_input_tokens;
		final_output.cached_input_tokens = total_cached_input_tokens;
		final_output.output_tokens = total_output_tokens;
		final_output.reasoning_tokens = total_reasoning_tokens;
		final_output.latency_ms = total_latency_ms;
		completion_summary = "- " + final_output.text;
		memory_summary = deterministic_delivery_summary(original_prompt, changes, final_output.text);
		rejected_changes = validate_change_proposals(workspace, project_path, changes);
		return step_t::completed;
	}
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
	const bool repair_probe = !libai::ai::focused_compile_repair(original_prompt) &&
	    !focused_go_test_packages(pending_test_failure_report).empty();
	const bool test_only = !repair_probe && !verified_batch && turn.trace.ok && turn.staged_progress &&
	    !libai::ai::focused_compile_repair(original_prompt) &&
	    libai::ai::coding_only_test_changes(turn.changes_before_tool, changes);
	std::string implementation_feedback;
	if (turn.trace.ok && turn.staged_progress && !changes.empty()) {
		acl::json checkpoint_json; auto &checkpoint = checkpoint_json.create_node();
		checkpoint.add_text("event", "implementation_checkpoint_submitted");
		auto &paths = checkpoint_json.create_array(); checkpoint.add_child("changed_paths", paths);
		size_t bytes = 0, files = 0, tests = 0;
		for (const auto &change : changes) {
			if (change.review_status == "rejected") continue;
			const auto before = std::find_if(turn.changes_before_tool.begin(), turn.changes_before_tool.end(),
			    [&](const agent_change_proposal_t &old) { return old.path == change.path && old.operation == change.operation && old.content == change.content && old.target_path == change.target_path; });
			if (before != turn.changes_before_tool.end()) continue;
			if (change.operation == "write") bytes += change.content.size();
			++files;
			if (libai::ai::coding_test_source(change.path)) ++tests;
			if (files <= 64) paths.add_array_text(change.path.c_str());
		}
		checkpoint.add_number("changed_files", files).add_number("changed_source_bytes", bytes)
		    .add_number("changed_test_files", tests).add_bool("path_list_complete", files <= 64);
		append_runtime_operation_event(runtime_task, checkpoint);
		size_t staged_tests=0, changed_sources=0;
		for (const auto &change:changes) {
			if (change.review_status=="rejected" || change.operation!="write")continue;
			if (libai::ai::coding_test_source(change.path)) {++staged_tests;continue;}
			const auto dot=change.path.find_last_of('.');
			const auto extension=dot==std::string::npos?"":change.path.substr(dot);
			const std::set<std::string> source_extensions={".go",".cpp",".cc",".c",".h",".hpp",".rs",".java",".py",".js",".ts",".html",".css",".cjs",".mjs"};
			if (!source_extensions.count(extension))continue;
			const auto old=std::find_if(turn.changes_before_tool.begin(),turn.changes_before_tool.end(),[&](const agent_change_proposal_t &before) {
				return before.path==change.path && before.operation==change.operation && before.content==change.content;
			});
			if (old==turn.changes_before_tool.end())++changed_sources;
		}
		if (changed_sources && !verified_batch && !libai::ai::focused_compile_repair(original_prompt) &&
		    !libai::ai::read_only_analysis_task(original_prompt) && !libai::ai::verification_only_task(original_prompt)) {
			acl::json evidence;auto &record=evidence.create_node();
			record.add_number("changed_source_files",changed_sources).add_number("changed_test_files",tests)
			    .add_number("staged_test_files",staged_tests);
			implementation_feedback="\n<implementation_evidence>"+serialize_json(record)+"</implementation_evidence>\n";
			if (!staged_tests)implementation_feedback+="No separate test files changed. Build-only is not test evidence. Use relevant existing or inline tests, or include required behavior tests with related implementation before final validation.\n";
		}

	}
	if (test_only) {
		acl::json deferred;
		auto &event = deferred.create_node();
		event.add_text("event", "automatic_build_deferred");
		event.add_text("reason", "test_source_only");
		event.add_bool("final_validation_required", true);
		event.add_number("commands_executed", 0);
		append_runtime_operation_event(runtime_task, event);
		turn.automatic_build_feedback =
		    "\n<test_validation_pending>Only test source changed. The intermediate automatic build was deferred. "
		    "Run workspace.validate once to compile and execute the fixed tests for this revision. Do not reread retained "
		    "source to confirm it was saved or add tests to build scripts to obtain evidence.</test_validation_pending>\n";
		next_tool_guidance += turn.automatic_build_feedback;
	}
	if (!test_only && !verified_batch && turn.trace.ok && turn.staged_progress &&
	    !changes.empty()) {
		const bool focused =
		    libai::ai::focused_compile_repair(original_prompt);
		agent_tool_trace_t validation_trace{};
		validation_trace.name = "workspace.validate";
		validation_trace.path = project_path;
		update_runtime_progress(runtime_task, "tool_call",
		    "workspace.validate", executed_tool_calls);
		completion_validation = validate_staged_draft(
		    runtime_task->user_root, project_path, runtime_task->id,
		    changes, sandbox_limits, validation_trace, NULL, !focused,
		    turn.effect_key, repair_probe ? pending_test_failure_report : "");
		if (validation_trace.ok && libai::ai::deterministic_compile_failure(completion_validation)) {
			acl::json failure(completion_validation.c_str());
			failure.finish();
			validation_cache.fingerprint = json_text(failure["source_baseline_sha256"]) + "\n" +
			    staged_change_fingerprint(changes);
			libai::ai::agent_workspace_t draft(persistent_draft_root(runtime_task->user_root, project_path, runtime_task->id));
			validation_cache.draft_fingerprint = validation_baseline_fingerprint(draft, "");
			validation_cache.execution_revision = runtime_task->coding_execution_revision;
			validation_cache.report = completion_validation;
		}
		traces.push_back(validation_trace);
		complete_runtime_tool(
		    runtime_task, validation_trace, executed_tool_calls);
		automatic_compile_check = validation_trace.ok;
		record_acceptance_evidence(completion_validation);
		review_validation_failure(turn, completion_validation);
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
		    repair_probe ? "automatic_repair_test" : focused ? "focused_compile_repair" : "automatic_build");
		node.add_text("report", completion_validation.c_str());
		append_runtime_operation_event(runtime_task, node);
		const std::string build_feedback = "\n<automatic_build>" +
		    completion_validation +
		    (repair_probe ? "\nThis is a focused repair test, not full validation. If it passes, finish with one full workspace.validate. " :
		    "\nThis is an intermediate build-only gate. test_status=not-run is expected here. ") +
		    "workspace.validate independently runs configured tests (including go.test); do not add test invocations "
		    "to build scripts merely to change this status. "
		    "\nIf compilation failed, repair the reported compiler diagnostics before continuing. "
		    "Do not remove the build script, suppress its error status, or expand scope to unrelated issues. "
		    "A successful build does not authorize unrelated work.</automatic_build>\n";
		next_tool_guidance += build_feedback;
		// Keep evidence in the durable tool transcript as well as the next request.
		turn.automatic_build_feedback = build_feedback;
	}
	if (!implementation_feedback.empty()) {
		next_tool_guidance+=implementation_feedback;
		turn.automatic_build_feedback+=implementation_feedback;
	}
	const bool compile_completed =
	    libai::ai::focused_compile_repair(original_prompt) &&
	    completion_validation.find("\"compile_repair_completed\":true") !=
	        std::string::npos;
	const bool validation_passed =
	    (turn.message.tool.name == "workspace.validate" ||
	        verified_batch) &&
	    turn.trace.ok &&
	    (!changes.empty() ||
	        libai::ai::verification_only_task(original_prompt)) &&
	    completion_validation.find("\"validation_passed\":true") !=
	        std::string::npos;
	if (!(validation_passed ||
	        (compile_completed && turn.trace.ok &&
	            (automatic_compile_check || verified_batch ||
	                turn.message.tool.name == "workspace.validate"))))
		return step_t::proceed;
	if (!compile_completed &&
	    !libai::ai::verification_only_task(original_prompt) &&
	    !acceptance_review_requested &&
	    executed_tool_calls < effective_tool_call_limit) {
		acceptance_review_requested = true;
		next_tool_guidance +=
		    "\n<acceptance_review>Configured checks passed for this revision. Before ending, map each user request "
		    "ID to observable evidence, distinguishing tested behavior from inferred behavior. Default to returning final JSON "
		    "now using retained source and validator evidence. Missing a separate direct test is not by itself evidence of a "
		    "missing implementation: report uncovered behavior as unverified instead of starting another coverage-expansion cycle. "
		    "Additional tools or edits are warranted only for a concrete implementation defect or an already identified unmet "
		    "requested behavior. Do not add routine duplicate coverage or reread generated files to confirm they were saved. "
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
