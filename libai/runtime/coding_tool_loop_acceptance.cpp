#include "stdafx.h"
#include "coding_tool_loop_internal.h"
#include "browser_debug.h"
#include "../storage/task_contract_store.h"
#include "../validation/task_acceptance.h"
#include "../context/task_delivery_summary.h"
#include "../sdk/file_execution_store.h"
#include "../common/file_ops.h"
namespace action
{
namespace agent_detail
{
bool coding_tool_loop_t::prepare_task_contract()
{
	if (runtime_task->text_preview || runtime_task->assistant_chat)
		return true;
	if (!libai::ai::begin_task_contract(runtime_task->user_root,
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
	if (!libai::ai::read_only_analysis_task(original_prompt) &&
	    !libai::ai::verification_only_task(original_prompt)) {
		unsigned long reports = 0;
		const bool threshold_reached =
		    libai::ai::browser_repair_evidence_t::applies_history(
		        recent, browser_debug_report_threshold + 1, &reports);
		const bool explicit_request =
		    libai::ai::browser_repair_evidence_t::requests_debug(
		        original_prompt);
		browser_evidence.enabled =
		    explicit_request || threshold_reached;
		std::lock_guard<libai::mutex> lock(g_agent_runtime_mutex);
		runtime_task->browser_debug_retry_count =
		    reports ? reports - 1 : 0;
		runtime_task->browser_debug_confirmation_required =
		    threshold_reached && !explicit_request &&
		    !runtime_task->browser_debug_consent;
	}
	std::lock_guard<libai::mutex> lock(g_agent_runtime_mutex);
	runtime_task->task_contract_json = task_contract;
	runtime_task->task_acceptance_status = "pending_verification";
	return true;
}
bool coding_tool_loop_t::current_full_validation_passed()
{
	if (!acceptance_review_requested || !last_validation_passed || acceptance_report.empty() ||
	    acceptance_draft.empty() || acceptance_baseline.empty()) return false;
	acl::json report(acceptance_report.c_str());
	if (!report.finish() || !json_bool(report["validation_passed"], false) ||
	    json_text(report["validation_scope"]) != "project") return false;
	return acceptance_draft == libai::ai::agent_workspace_t::content_sha256(
	        staged_change_fingerprint(changes)) &&
	    acceptance_baseline == validation_baseline_fingerprint(workspace, project_path);
}

void coding_tool_loop_t::record_acceptance_evidence(const std::string &report)
{
	// Capture each trusted validator result, including auto-build, final and
	// budget-boundary paths. Never extract evidence from model prose.
	acl::json evidence(report.c_str());
	if (evidence.finish()) {
		reasoning_validation_report = report;
		const auto scope = json_text(evidence["validation_scope"]);
		const bool passed = json_bool(evidence["validation_passed"], false);
        auto clear_failures = [&]() {
            pending_test_failure_report.clear();
            repair_failures.clear();
            unresolved_repair_contract.clear();
            repair_contract_findings.clear();
            repair_required_reads.clear();
            repair_supplied_versions.clear();
            pending_repair_edits.clear();
            repair_changed_paths.clear();
        };
        if (scope == "targeted-tests" && passed && json_text(evidence["test_status"]) == "passed") {
            const auto pending = focused_go_test_packages(pending_test_failure_report);
            std::set<std::string> tested;
            auto *packages = json_array_node(evidence["targeted_packages"]);
            for (auto *item = packages ? packages->first_child() : NULL; item; item = packages->next_child())
                tested.insert(json_text(item));
            // A passing partial check clears only failures it actually covered.
            // Missing/ambiguous scope stays unresolved and needs a full run.
            if (!pending.empty() && std::all_of(pending.begin(), pending.end(),
                    [&](const std::string &p) { return tested.count(p) != 0; })) {
                clear_failures();
                repair_full_validation_pending = true;
                acceptance_report = report;
                acceptance_draft.clear();
                acceptance_baseline.clear();
            }
            return; // Not final acceptance evidence.
        }
        if (scope == "project") {
            // A later HTTP failure must not resurrect Go tests that passed.
            auto *commands = json_array_node(evidence["commands"]);
            for (auto *item = commands ? commands->first_child() : NULL; item; item = commands->next_child()) {
                auto *command = item->is_object() ? item : item->get_obj();
                acl::json pending(pending_test_failure_report.c_str());
                if (command && json_text((*command)["command_id"]) == "go.test" &&
                    json_bool((*command)["passed"], false) && pending.finish() &&
                    json_text(pending["failed_command_id"]) == "go.test") clear_failures();
            }
            repair_full_validation_pending = false;
        }
        if (!passed && (json_text(evidence["test_status"]) == "failed" ||
                libai::ai::is_test_command(json_text(evidence["failed_command_id"])))) {
            if (scope != "targeted-tests" || pending_test_failure_report.empty()) {
                pending_test_failure_report = report;
            } else {
                const auto previous = focused_go_test_packages(pending_test_failure_report);
                const auto current = focused_go_test_packages(report);
                const bool covered = !previous.empty() && !current.empty() &&
                    std::all_of(current.begin(), current.end(), [&](const std::string &package) {
                        return std::find(previous.begin(), previous.end(), package) != previous.end();
                    });
                if (!covered) {
                    acl::json pending(pending_test_failure_report.c_str());
                    if (pending.finish()) {
                        pending.get_root().add_bool("repair_requires_full_validation", true);
                        pending_test_failure_report = serialize_json(pending.get_root());
                    }
                }
            }
        }
        if (scope == "targeted-tests") return;
        if (scope == "build-only" && passed && !pending_test_failure_report.empty()) return;
        if (passed && scope != "build-only" && json_text(evidence["test_status"]) == "passed") {
            clear_failures();
            repair_full_validation_pending = false;
        }
	}
	acceptance_report = report;
	acceptance_draft =
	    evidence.finish() ? json_text(evidence["proposal_sha256"]) : "";
	acceptance_baseline = evidence.finish() ?
	    json_text(evidence["source_baseline_sha256"]) :
	    "";
}
bool coding_tool_loop_t::run()
{
#if defined(__unix__) || defined(__APPLE__)
    // Hold a pinned SDK lease across startup, requests, tools and acceptance.
    // The HTTP admission map cannot exclude another service process.
    std::shared_ptr<libai::file_execution_store_t> execution_lease;
    const std::string lease_scope="coding_loop_"+(runtime_task->session_id.empty()?std::string("single"):runtime_task->session_id);
    if(!runtime_task->text_preview && !runtime_task->assistant_chat) {
        std::string project;
        if(!libai::ai::agent_workspace_t::resolve_project_state_root(runtime_task->user_root,project_path,project,err))return false;
        const auto directory=libai::ai::file_ops::join_path(project,".libai_agent");
        if(!libai::ai::file_ops::make_private_directory(directory)) {
            err="cannot prepare coding execution lease directory";return false;
        }
        try {
            execution_lease.reset(new libai::file_execution_store_t(directory,libai::execution_file_codec_t::plaintext()));
            const auto snapshot=execution_lease->open(lease_scope);
            // Reserved namespace: this scope owns only a lock, not a snapshot.
            if(!snapshot.state_json.empty()) {err="unexpected coding execution lease state";return false;}
            std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
            if(runtime_task->retain_coding_worker_lease)runtime_task->coding_worker_lease=execution_lease;
        }catch(const std::exception &error){err=std::string("coding execution lease unavailable: ")+error.what();return false;}
    }
#endif
	if (!prepare_task_contract())
		return false;
	const bool completed = run_impl();
	return finish_task_acceptance(completed);
}
bool coding_tool_loop_t::cleanup_browser_style(
    const std::function<std::string(const std::string &)> &dispatch)
{
	const auto original_error = err;
	auto fail = [&]() {
		if (!original_error.empty())
			err = original_error + " Browser cleanup: " + err;
		return false;
	};
	// Cleanup is an external tool call too; never exceed the persisted ledger
	// or silently bypass the host's tool budget on a terminal path.
	if (executed_tool_calls >= effective_tool_call_limit ||
	    executed_tool_calls >= 256) {
		err = "browser cleanup requires an available tool budget; host cleanup required";
		return fail();
	}
	coding_turn_t cleanup;
	cleanup.call = executed_tool_calls;
	cleanup.provider_tool_call_count = 1;
	cleanup.message.tool.name = "browser.patch_style";
	cleanup.message.tool.content = "{\"undo\":\"all\"}";
	cleanup.message.tool.content_present = true;
	if (!begin_tool_effect(cleanup))
		return fail();
	++executed_tool_calls;
	try {
		cleanup.tool_result = dispatch(cleanup.effect_key);
	} catch (const std::exception &error) {
		err = std::string("browser cleanup outcome unknown: ") + error.what();
		return fail();
	} catch (...) {
		err = "browser cleanup outcome unknown";
		return fail();
	}
	acl::json result(cleanup.tool_result.c_str());
	cleanup.trace.ok = result.finish() && json_bool(result["ok"], false);
	if (settle_tool_effect(cleanup, step_t::proceed) == step_t::failed)
		return fail();
	err = original_error;
	return true;
}

bool coding_tool_loop_t::finish_task_acceptance(bool completed)
{
	refresh_browser_evidence();
	bool temporary_cleanup_failed = false;
	if (browser_evidence.enabled &&
	    !browser_evidence.active_patch().empty()) {
		// A failed or early-ended run must not leave its temporary experiment active.
		acl::json undo;
		auto &options = undo.create_node();
		options.add_text("undo", "all");
		temporary_cleanup_failed = !cleanup_browser_style([&](const std::string &key) {
			return libai::ai::browser_debug_tool(runtime_task->user_root,
			    project_path, runtime_task->id, "browser.patch_style", "",
			    serialize_json(options), key);
		});
		if (temporary_cleanup_failed)
			completed = false;
		append_simple_operation_event(runtime_task,
		    "browser_temporary_style_cleanup",
		    temporary_cleanup_failed ? "failed" : "undo_acknowledged",
		    "", executed_tool_calls);
	}
	if (task_contract.empty())
		return completed;
	const bool current = !acceptance_report.empty() &&
	    !acceptance_baseline.empty() &&
	    acceptance_draft ==
	        libai::ai::agent_workspace_t::content_sha256(
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
	    libai::ai::focused_compile_repair(original_prompt);
	const std::string state = libai::ai::task_acceptance_status(
	    current, executed, passed, compile_scope, compile_passed);
	acl::json json;
	auto &evidence = json.create_node();
	evidence.add_number("version", 1);
	evidence.add_text("status", state.c_str());
	evidence.add_text("run_id", runtime_task->id.c_str());
	evidence.add_bool("run_completed", completed);
	evidence.add_number("mutation_batches_this_invocation",mutation_batches_this_invocation);
	evidence.add_number("retained_proposal_count",changes.size());
	evidence.add_bool("proposals_changed_since_invocation_start",!initial_proposal_identity.empty() &&
	    initial_proposal_identity!=libai::ai::agent_workspace_t::content_sha256(staged_cycle_fingerprint(changes)));
	evidence.add_bool("evidence_current_at_run_end", current);
	evidence.add_bool("all_user_requirements_verified", false);
	evidence.add_text("evidence_source", "runtime_fixed_validator");
	if(valid && report["source_contract_review"]) {
		auto *review=report["source_contract_review"];
		if(!review->is_object())review=review->get_obj();
		if(review)evidence.add_child("source_contract_review",json.duplicate_node(*review));
	}
	bool browser_interaction_required = libai::ai::needs_browser_interaction(original_prompt);
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
		browser_interaction_required = browser_interaction_required ||
		    libai::ai::needs_browser_interaction(json_text((*request)["text"]));
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
	if (browser_interaction_required) {
		evidence.add_text("browser_interaction_status",
		    current && !browser_status.empty() ? browser_status.c_str() : "unverified");
		evidence.add_text("direct_site_acceptance", "coverage_not_established");
		summary += " " + libai::ai::browser_evidence_summary(current, browser_status, chinese);
	}
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
	if (completed && mutation_batches_this_invocation==0 &&
	    !libai::ai::read_only_analysis_task(original_prompt) && !libai::ai::verification_only_task(original_prompt))
		summary += chinese ? " 本次执行未新增或修改文件提案；已有提案不代表本轮实施了修复。" :
		    " This invocation created or changed no file proposals; retained proposals do not establish a repair performed in this invocation.";
	evidence.add_text("summary", summary.c_str());
	evidence.add_text(
	    "requirement_progress_json", requirement_progress_json.c_str());
	if (completed) {
		final_output.text =
		    "LibAI：" + summary + "\n\n" + final_output.text;
		std::vector<std::pair<std::string, std::string>> edits;
		for (const auto &change : changes) {
			// Proposal reasons are model-authored. Operation/path are the
			// durable facts; do not infer business completion from either.
			edits.push_back(
			    { change.operation + " " + change.path, "" });
		}
		completion_summary = libai::ai::task_delivery_summary(
		    original_prompt, edits, completion_summary, summary,
		    chinese, requirement_progress_json);
		memory_summary = summary + "\n" + memory_summary;
	}
	{
		std::lock_guard<libai::mutex> lock(g_agent_runtime_mutex);
		runtime_task->task_contract_json = task_contract;
		runtime_task->task_acceptance_json =
		    evidence.to_string().c_str();
		runtime_task->task_acceptance_status = state;
	}
	evidence.add_text("event", "task_acceptance_evaluated");
	append_runtime_operation_event(runtime_task, evidence);
	return completed;
}
}
}
