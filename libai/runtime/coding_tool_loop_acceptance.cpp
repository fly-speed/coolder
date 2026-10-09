#include "stdafx.h"
#include "coding_tool_loop_internal.h"
#include "browser_debug.h"
#include "../storage/task_contract_store.h"
#include "../validation/task_acceptance.h"
#include "../context/task_delivery_summary.h"
namespace action
{
namespace agent_detail
{
bool coding_tool_loop_t::prepare_task_contract()
{
	if (runtime_task->text_preview || runtime_task->assistant_chat)
		return true;
	if (!webcool::ai::begin_task_contract(runtime_task->user_root,
	        project_path, runtime_task->session_id, runtime_task->id,
	        original_prompt,
	        initial_completed_tool_calls > 0 ||
	            runtime_task->resumed_after_restart ||
	            runtime_task->resumed_from_progress,
	        task_contract, err))
		return false;
	acl::json contract(task_contract.c_str());
	auto *requests =
	    contract.finish() ? json_array_node(contract["requests"]) : NULL;
	std::vector<std::string> recent;
	for (auto *item = requests ? requests->first_child() : NULL; item;
	     item = requests->next_child()) {
		auto *request = item->is_object() ? item : item->get_obj();
		if (!request)
			continue;
		recent.push_back(json_text((*request)["text"]));
	}
	if (!webcool::ai::read_only_analysis_task(original_prompt) &&
	    !webcool::ai::verification_only_task(original_prompt)) {
		unsigned long reports = 0;
		const bool threshold_reached =
		    webcool::ai::browser_repair_evidence_t::applies_history(
		        recent, browser_debug_report_threshold + 1, &reports);
		const bool explicit_request =
		    webcool::ai::browser_repair_evidence_t::requests_debug(
		        original_prompt);
		browser_evidence.enabled =
		    explicit_request || threshold_reached;
		std::lock_guard<webcool::mutex> lock(g_agent_runtime_mutex);
		runtime_task->browser_debug_retry_count =
		    reports ? reports - 1 : 0;
		runtime_task->browser_debug_confirmation_required =
		    threshold_reached && !explicit_request &&
		    !runtime_task->browser_debug_consent;
	}
	std::lock_guard<webcool::mutex> lock(g_agent_runtime_mutex);
	runtime_task->task_contract_json = task_contract;
	runtime_task->task_acceptance_status = "pending_verification";
	return true;
}
void coding_tool_loop_t::record_acceptance_evidence(const std::string &report)
{
	// Capture each trusted validator result, including auto-build, final and
	// budget-boundary paths. Never extract evidence from model prose.
	acceptance_report = report;
	acl::json evidence(report.c_str());
	acceptance_draft =
	    evidence.finish() ? json_text(evidence["proposal_sha256"]) : "";
	acceptance_baseline = evidence.finish() ?
	    json_text(evidence["source_baseline_sha256"]) :
	    "";
}
bool coding_tool_loop_t::run()
{
	if (!prepare_task_contract())
		return false;
	const bool completed = run_impl();
	finish_task_acceptance(completed);
	return completed;
}
void coding_tool_loop_t::finish_task_acceptance(bool completed)
{
	refresh_browser_evidence();
	bool temporary_cleanup_failed = false;
	if (browser_evidence.enabled &&
	    !browser_evidence.active_patch().empty()) {
		// A failed or early-ended run must not leave its temporary experiment active.
		acl::json undo;
		auto &options = undo.create_node();
		options.add_text("undo", "all");
		const auto result = webcool::ai::browser_debug_tool(
		    runtime_task->user_root, project_path, runtime_task->id,
		    "browser.patch_style", "", serialize_json(options));
		acl::json cleanup(result.c_str());
		temporary_cleanup_failed =
		    !cleanup.finish() || !json_bool(cleanup["ok"], false);
		append_simple_operation_event(runtime_task,
		    "browser_temporary_style_cleanup",
		    temporary_cleanup_failed ? "failed" : "undo_acknowledged",
		    "", executed_tool_calls);
	}
	if (task_contract.empty())
		return;
	const bool current = !acceptance_report.empty() &&
	    !acceptance_baseline.empty() &&
	    acceptance_draft ==
	        webcool::ai::agent_workspace_t::content_sha256(
	            staged_change_fingerprint(changes)) &&
	    acceptance_baseline ==
	        validation_baseline_fingerprint(workspace, project_path);
	acl::json report(acceptance_report.c_str());
	const bool valid = report.finish();
	const std::string browser_status =
	    valid ? json_text(report["browser_acceptance"]) : "";
	const bool browser_blocked =
	    browser_status == "unavailable" || browser_status == "blocked";
	const bool executed = valid && !browser_blocked &&
	    json_number(report["commands_executed"], 0) > 0;
	const bool passed =
	    valid && json_bool(report["validation_passed"], false);
	const bool compile_passed =
	    valid && json_bool(report["compile_repair_completed"], false);
	const bool compile_scope =
	    webcool::ai::focused_compile_repair(original_prompt);
	const std::string state = webcool::ai::task_acceptance_status(
	    current, executed, passed, compile_scope, compile_passed);
	acl::json json;
	auto &evidence = json.create_node();
	evidence.add_number("version", 1);
	evidence.add_text("status", state.c_str());
	evidence.add_text("run_id", runtime_task->id.c_str());
	evidence.add_bool("run_completed", completed);
	evidence.add_bool("evidence_current_at_run_end", current);
	evidence.add_bool("all_user_requirements_verified", false);
	evidence.add_text("evidence_source", "runtime_fixed_validator");
	auto &coverage = json.create_array();
	evidence.add_child("requirement_coverage", coverage);
	acl::json contract(task_contract.c_str());
	auto *requests =
	    contract.finish() ? json_array_node(contract["requests"]) : NULL;
	for (auto *item = requests ? requests->first_child() : NULL; item;
	     item = requests->next_child()) {
		auto *request = item->is_object() ? item : item->get_obj();
		if (!request)
			continue;
		auto &requirement = coverage.add_child(false, true);
		requirement.add_text(
		    "request_id", json_text((*request)["id"]).c_str());
		requirement.add_text("status", "coverage_not_established");
		requirement.add_text("reason",
		    "Passing configured checks does not establish coverage of every requested behavior.");
	}
	evidence.add_text("scope",
	    compile_scope ? "compiler_repair" : "configured_checks_only");
	evidence.add_text("draft_sha256", acceptance_draft.c_str());
	evidence.add_text("baseline_sha256", acceptance_baseline.c_str());
	evidence.add_text("environment",
	    "private_draft_sandbox; historical evidence, revalidate after applying or external edits");
	evidence.add_number("observed_at", static_cast<long long>(time(NULL)));
	// Reports are bounded independently from model memory. Oversized diagnostics
	// stay in the operation log; no silent claim that missing details verified a requirement.
	evidence.add_text("report",
	    acceptance_report.size() <= 128 * 1024 ?
	        acceptance_report.c_str() :
	        "report exceeds evidence limit; see operation log");
	std::string summary = browser_status == "blocked" ?
	    (chinese ?
	            "浏览器未执行：验收服务端口被占用，需启用独立验收端口；功能尚未验收。" :
	            "Browser did not run: service port occupied; configure an isolated validation port.") :
	    browser_status == "unavailable" ?
	    (chinese ?
	            "跨浏览器验收未完成：部分浏览器运行环境不可用；功能尚未验收。" :
	            "Cross-browser acceptance incomplete: a required browser runtime is unavailable.") :
	    state == "scope_verified" ?
	    (chinese ?
	            "编译修复范围已验证；其它业务要求不在此次验证范围内。" :
	            "Compiler repair verified; other requirements were not verified.") :
	    state == "verification_failed" ?
	    (chinese ? "验证失败；任务尚未验收。" :
	               "Verification failed; task acceptance is pending.") :
	    state == "checks_passed" ?
	    (chinese ?
	            "已配置检查通过；用户要求仍待逐项验收，不能据此确认功能完整可用。" :
	            "Configured checks passed; user requirements still need acceptance.") :
	    (chinese ?
	            "待验收：缺少当前版本的有效验证证据。" :
	            "Acceptance pending: no valid evidence for the current revision.");
	if (browser_evidence.enabled) {
		refresh_browser_evidence();
		evidence.add_bool("reversible_live_style_observed",
		    browser_evidence.permits_edits());
		evidence.add_text(
		    "applied_source_firefox_verification", "pending");
		summary += chinese ?
		    " Firefox 正式源码效果待复验：临时样式实验与草稿检查不能证明当前页面已修复。" :
		    " Applied-source Firefox verification pending: temporary styles and draft checks do not prove the live page is fixed.";
	}
	if (temporary_cleanup_failed)
		summary += chinese ?
		    " 临时样式撤销失败，请在扩展中断开连接以清理。" :
		    " Temporary-style cleanup failed; disconnect the extension to clear it.";
	evidence.add_text("summary", summary.c_str());
	evidence.add_text(
	    "requirement_progress_json", requirement_progress_json.c_str());
	if (completed) {
		final_output.text =
		    "WebCool：" + summary + "\n\n" + final_output.text;
		std::vector<std::pair<std::string, std::string>> edits;
		for (const auto &change : changes) {
			// Proposal reasons are model-authored. Operation/path are the
			// durable facts; do not infer business completion from either.
			edits.push_back(
			    { change.operation + " " + change.path, "" });
		}
		completion_summary = webcool::ai::task_delivery_summary(
		    original_prompt, edits, completion_summary, summary,
		    chinese, requirement_progress_json);
		memory_summary = summary + "\n" + memory_summary;
	}
	{
		std::lock_guard<webcool::mutex> lock(g_agent_runtime_mutex);
		runtime_task->task_contract_json = task_contract;
		runtime_task->task_acceptance_json =
		    evidence.to_string().c_str();
		runtime_task->task_acceptance_status = state;
	}
	evidence.add_text("event", "task_acceptance_evaluated");
	append_runtime_operation_event(runtime_task, evidence);
}
}
}
