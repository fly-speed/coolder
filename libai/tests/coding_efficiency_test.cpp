#include <libai/runtime/coding_runtime.h>
#include <libai/runtime/coding_tool_loop_internal.h>
#include <libai/context/coding_turn_budget.h>
#include <libai/context/coding_test_changes.h>
#include <libai/context/coding_behavior_checks.h>
#include <libai/context/source_request_compactor.h>
#include <libai/context/compile_failure_cache.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <chrono>

using namespace libai::ai;
using namespace action::agent_detail;

namespace action { namespace agent_detail {
struct coding_loop_test_access_t {
    static void compile_cache(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        loop.model_tools.emplace_back("workspace.validate","fixture",false,true,false,"workspace");
        loop.model_tools.emplace_back("workspace.patch_set","fixture",true,true,true,"workspace");
        const auto agent=args.runtime_task->user_root+"/fixture/.libai_agent";
        assert(file_ops::make_private_directory(agent));
        assert(file_ops::make_private_directory(agent+"/worktrees"));
        const auto draft=persistent_draft_root(args.runtime_task->user_root,args.project_path,args.runtime_task->id);
        assert(file_ops::make_private_directory(draft));
        std::ofstream(draft+"/value.go") << "package value\nvar x = missing\n";
        agent_workspace_t staged(draft);
        loop.validation_cache.report=R"({"validation_passed":false,"validation_scope":"build-only","commands_executed":1,"failed_command_id":"go.build","commands":[{"command_id":"go.build","passed":false,"started":true,"exit_code":1,"diagnostics":[{"path":"value.go","line":2,"message":"undefined: missing"}]}]})";
        loop.validation_cache.execution_revision=args.runtime_task->coding_execution_revision;
        loop.validation_cache.fingerprint=validation_baseline_fingerprint(args.workspace,args.project_path)+"\n"+staged_change_fingerprint(args.changes);
        loop.validation_cache.draft_fingerprint=validation_baseline_fingerprint(staged,"");
        auto available=[&]() {coding_turn_t turn;loop.select_request_tools(turn);
            return std::any_of(turn.input.tools.begin(),turn.input.tools.end(),[](const agent_tool_t &t){return t.name=="workspace.validate";});};
        assert(!available());
        agent_tool_request_t validate; validate.name="workspace.validate";
        agent_tool_trace_t trace;
        const auto reused=execute_workspace_tool(args.workspace,args.runtime_task->user_root,
            args.project_path,true,validate,trace,&args.changes,args.runtime_task->id,
            args.sandbox_limits,false,nullptr,&loop.validation_cache,nullptr,nullptr,"efficiency.cache.1");
        acl::json reuse_json(reused.c_str()); assert(reuse_json.finish());
        assert(trace.ok && json_bool(reuse_json["validation_reused"],false));
        assert(!json_bool(reuse_json["validation_passed"],true));
        assert(!json_bool(reuse_json["executed_this_call"],true));
        assert(json_number(reuse_json["commands_executed_this_call"],-1)==0);

        const auto unbound_report=loop.validation_cache.report;
        loop.validation_cache.report=R"({"tool_execution_key":"missing.effect","validation_passed":false,"commands":[{"idempotency_key":"missing.validation.receipt","command_id":"go.build","passed":false,"started":true,"exit_code":1,"diagnostics":[{"path":"value.go","line":2}]}]})";
        assert(available()); // Missing durable receipts cannot justify hiding validation.
        const auto missing_receipt=execute_workspace_tool(args.workspace,args.runtime_task->user_root,
            args.project_path,true,validate,trace,&args.changes,args.runtime_task->id,
            args.sandbox_limits,false,nullptr,&loop.validation_cache,nullptr,nullptr,"efficiency.cache.2");
        assert(!trace.ok && missing_receipt.find("receipt")!=std::string::npos);
        loop.validation_cache.report=unbound_report;
        loop.validation_cache.execution_revision+="policy-change";assert(available());
        loop.validation_cache.execution_revision=args.runtime_task->coding_execution_revision;
        std::ofstream(draft+"/value.go") << "package value\nvar x = 7\n";assert(available());
        loop.validation_cache.draft_fingerprint=validation_baseline_fingerprint(staged,"");
        std::ofstream(args.runtime_task->user_root+"/fixture/src/external.go") << "package main\n";assert(available());
        std::remove((args.runtime_task->user_root+"/fixture/src/external.go").c_str());
        // Failure context starts near the diagnostic, includes related package
        // source and scripts, and identifies omitted ranges explicitly.
        std::string long_source="package value\n";
        for(int i=0;i<600;++i)long_source+="// padding for diagnostic window\n";
        long_source+="var diagnosticMarker = missing\n";
        std::ofstream(draft+"/value.go") << long_source;
        std::ofstream(draft+"/related.go") << "package value\nfunc Related() {}\n";
        std::ofstream(draft+"/build.sh") << "go build ./src\n";
        auto related=agent_change_proposal_t();related.path="fixture/related.go";
        related.operation="write";related.content="package value\nfunc Related() {}\n";
        const auto windows=coding_loop_detail::validation_repair_context(
            args.runtime_task->user_root,args.project_path,args.runtime_task->id,{related},
            R"({"commands":[{"command_id":"project.script-build","passed":false,"diagnostics":[{"path":"value.go","line":602}]}]})",20000);
        assert(windows.find("diagnosticMarker")!=std::string::npos);
        assert(windows.find("fixture/related.go")!=std::string::npos);
        assert(windows.find("fixture/build.sh")!=std::string::npos);
        assert(windows.find("read_hint")!=std::string::npos);
        assert(windows.find("package value\\n// padding")==std::string::npos);
        assert(deterministic_compile_failure(loop.validation_cache.report));
        assert(!deterministic_compile_failure(R"({"validation_passed":false,"commands":[{"command_id":"go.build","passed":false,"started":true,"timed_out":true,"diagnostics":[{"path":"x.go","line":2}]}]})"));
        assert(!deterministic_compile_failure(R"({"validation_passed":false,"commands":[{"command_id":"go.test","passed":false,"started":true,"diagnostics":[{"path":"x_test.go","line":2}]}]})"));
    }
    static void repair_evidence_scope(const coding_loop_arguments_t &args) {
        assert(args.changes.empty());
        coding_tool_loop_t loop(args);
        const auto draft=persistent_draft_root(args.runtime_task->user_root,args.project_path,args.runtime_task->id);
        assert(file_ops::make_private_directory(draft+"/src"));
        assert(file_ops::make_private_directory(draft+"/src/gate"));
        for(const char *name:{"value.go","value_test.go","unrelated.go","other_test.go"}) {
            const std::string path="src/gate/"+std::string(name),content="package gate\n// "+std::string(name)+"\n";
            std::ofstream(draft+"/"+path)<<content;
            agent_change_proposal_t c;c.path="fixture/"+path;c.operation="write";c.content=content;
            args.changes.push_back(c);
        }
        coding_turn_t turn;
        turn.tool_result=R"json({"failed_command_id":"go.test","commands":[{"command_id":"go.test","diagnostic":"--- FAIL: TestValue (0.00s)","diagnostics":[{"path":"src/gate/value_test.go","line":3}]}]})json";
        loop.select_repair_sources(turn);
        assert(loop.repair_required_reads.size()==2);
        assert(loop.repair_required_reads.count("fixture/src/gate/value_test.go"));
        assert(loop.repair_required_reads.count("fixture/src/gate/value.go"));
        assert(!loop.repair_required_reads.count("fixture/src/gate/unrelated.go"));
        loop.repair_required_reads.clear();
        // Existing evidence larger than the supply budget must still count as
        // reviewed, leaving the whole additional budget for missing sources.
        acl::json records;auto &list=records.create_array();
        for(const char *name:{"a.go","b.go","z.go"}) {
            const std::string file="fixture/src/gate/"+std::string(name);
            const std::string body="package gate\n// "+std::string(name==std::string("z.go")?32:20000,'x')+"\n";
            std::ofstream(draft+"/src/gate/"+name)<<body;
            const auto hash=agent_workspace_t::content_sha256(body);
            loop.repair_required_reads[file]=hash;
            if(name==std::string("z.go"))continue;
            auto &r=list.add_child(false,true);r.add_text("path",file.c_str())
                .add_text("file_sha256",hash.c_str()).add_number("offset",0).add_text("content",body.c_str());
        }
        turn.input.user_prompt="<read_working_set>"+std::string(list.to_string().c_str())+"</read_working_set>";
        loop.supply_repair_review_sources(turn);
        assert(loop.repair_required_reads.empty() && loop.repair_supplied_versions.size()==3);
        assert(turn.addition.find("fixture/src/gate/z.go")!=std::string::npos);
        assert(turn.addition.find("fixture/src/gate/a.go")==std::string::npos);
        args.changes.clear();
    }
    static void no_mutation_claim(const coding_loop_arguments_t &args) {
        assert(args.changes.empty());
        agent_change_proposal_t retained;retained.path="fixture/src/retained.go";
        retained.operation="write";retained.content="package main\n";
        args.changes.push_back(retained); // Prior work must not count as this invocation's repair.
        coding_tool_loop_t loop(args);
        loop.task_contract=R"({"requests":[]})";
        loop.acceptance_report=R"({"source_contract_review":{"status":"review_only","findings":[]}})";
        loop.initial_proposal_identity=agent_workspace_t::content_sha256(staged_cycle_fingerprint(args.changes));
        const auto old_text=args.final_output.text;
        args.final_output.text="All requested changes were fixed.";
        assert(loop.finish_task_acceptance(true));
        assert(args.final_output.text.find("created or changed no file proposals")!=std::string::npos ||
            args.final_output.text.find("本次执行未新增或修改文件提案")!=std::string::npos);
        acl::json evidence(args.runtime_task->task_acceptance_json.c_str());assert(evidence.finish());
        assert(args.runtime_task->task_acceptance_json.find("\"source_contract_review\":{")!=std::string::npos);
        assert(args.runtime_task->task_acceptance_json.find("\"source_contract_review\":\"source_contract_review\"")==std::string::npos);
        assert(json_number(evidence["mutation_batches_this_invocation"],-1)==0);
        assert(json_number(evidence["retained_proposal_count"],-1)==1);
        assert(!json_bool(evidence["proposals_changed_since_invocation_start"],true));
        args.final_output.text=old_text;
        args.changes.clear();
    }
    static void proposal_recovery(const coding_loop_arguments_t &args) {
        assert(args.changes.empty());
        std::string saved;
        agent_tool_trace_t trace;
        size_t sequence=0;
        auto invoke=[&](const std::string &content) {
            agent_tool_request_t request;request.name="workspace.propose_batch";
            request.content=content;request.content_present=true;
            return execute_workspace_tool(args.workspace,args.runtime_task->user_root,args.project_path,
                true,request,trace,&args.changes,args.runtime_task->id,args.sandbox_limits,false,
                nullptr,nullptr,&saved,nullptr,"recovery.fixture."+std::to_string(++sequence));
        };
        for (const char *content : {"round complete", "{\"summary\":\"done\"}"}) {
            const auto raw=invoke(content);
            acl::json parsed(raw.c_str());assert(parsed.finish());
            assert(!trace.ok && args.changes.empty());
            assert(json_text(parsed["code"])=="proposal_batch_unparsed");
        }
        for (const char *content : {"{\"saved_batch\":\"missing\",\"offset\":0,\"limit\":1}",
                "{\"saved_batch\":\"\"}", "{\"saved_batch\":7}"}) {
            const auto missing=invoke(content);
            acl::json missing_json(missing.c_str());assert(missing_json.finish());
            assert(!trace.ok && json_text(missing_json["code"])!="proposal_batch_unparsed");
        }
        const std::string payload=R"([{"path":"src/recovery.go","content":"package main\nvar Recovery = 6\n"},{"path":"src/helper.go","content":"package main\nvar Helper = 2\n"},{"path":"fixture/src/recovery.go","content":"package main\nvar Recovery = 7\n"}])";
        const auto rejected=invoke(payload);
        acl::json failure(rejected.c_str());assert(failure.finish());
        assert(!trace.ok && args.changes.empty() && saved==payload);
        assert(json_text(failure["error_code"])=="proposal_batch_duplicate_paths");
        assert(rejected.find("fixture/src/recovery.go")!=std::string::npos);
        assert(rejected.find("response_archive")!=std::string::npos);
        const auto key=json_text(failure["saved_batch"]);
        assert(key==agent_workspace_t::content_sha256(payload));
        auto reference=[&](const std::string &fields) { return "{\"saved_batch\":\""+key+"\","+fields+"}"; };
        const auto inspected=invoke(reference("\"inspect_index\":2"));
        assert(trace.ok && args.changes.empty() && inspected.find("Recovery = 7")!=std::string::npos);
        agent_tool_request_t inspect;inspect.name="workspace.propose_batch";inspect.content=reference("\"inspect_index\":2");
        assert(repair_review_read_only(inspect));
        inspect.content=reference("\"inspect_index\":2,\"offset\":0");assert(!repair_review_read_only(inspect));
        inspect.content=R"([{"path":"src/recovery.go","content":"package main","saved_batch":"nested","inspect_index":0}])";
        assert(!repair_review_read_only(inspect));
        for(const auto &fields:{"\"offset\":0,\"limit\":3,\"exclude_indices\":[99]",
                "\"offset\":0,\"limit\":3,\"exclude_indices\":[0,0]",
                "\"offset\":0,\"limit\":3,\"exclude_indices\":[\"0\"]",
                "\"inspect_index\":99", "\"inspect_index\":0,\"exclude_indices\":[0]",
                "\"inspect_index\":0,\"query\":4"}) {
            const auto invalid=invoke(reference(fields));
            assert(!trace.ok && args.changes.empty() && invalid.find("error")!=std::string::npos);
        }
        const auto mismatch=invoke("{\"saved_batch\":\"not-the-retained-hash\",\"offset\":0,\"limit\":3}");
        assert(!trace.ok && args.changes.empty());
        invoke(reference("\"offset\":0,\"limit\":3"));assert(!trace.ok && args.changes.empty());
        const auto replay=invoke(reference("\"offset\":0,\"limit\":3,\"exclude_indices\":[0]"));
        assert(trace.ok && replay.find("formal_source_changed\":false")!=std::string::npos);
        bool intended=false, helper=false;
        for(const auto &change:args.changes) {
            if(change.path=="fixture/src/recovery.go")intended=change.content.find("Recovery = 7")!=std::string::npos;
            if(change.path=="fixture/src/helper.go")helper=true;
        }
        assert(intended && helper);
        args.changes.clear();
        const auto forbidden=invoke(R"([{"path":"../outside.go","content":"x"},{"path":"../outside.go","content":"y"}])");
        assert(!trace.ok && args.changes.empty() && saved==payload);
        assert(forbidden.find("saved_batch")==std::string::npos);
        // Exclusion is relative to original array positions, not to a shrinking
        // count; slicing must not spill into later records.
        invoke(reference("\"offset\":0,\"limit\":2,\"exclude_indices\":[0]"));
        assert(trace.ok);
        for(const auto &change:args.changes)assert(change.path!="fixture/src/recovery.go");
        args.changes.clear();
        coding_tool_loop_t loop(args);
        for(const char *name:{"workspace.list","workspace.search","workspace.read","workspace.read_batch","workspace.propose_batch"})
            loop.model_tools.emplace_back(name,"fixture",false,true,true,"workspace");
        loop.saved_proposal_batch=saved;loop.proposal_batch_recovery=rejected;
        coding_turn_t turn;loop.select_request_tools(turn);
        for(const auto &tool:turn.input.tools)assert(tool.name!="workspace.list" && tool.name!="workspace.search");
        assert(turn.input.turn_instructions.find("proposal_batch_recovery")!=std::string::npos);
        loop.saved_proposal_batch="different retained payload";
        coding_turn_t stale;loop.select_request_tools(stale);
        assert(std::any_of(stale.input.tools.begin(),stale.input.tools.end(),[](const agent_tool_t &tool){return tool.name=="workspace.list";}));
        assert(stale.input.turn_instructions.find("proposal_batch_recovery")==std::string::npos);
        loop.proposal_batch_recovery.clear();coding_turn_t restored;loop.select_request_tools(restored);
        assert(std::any_of(restored.input.tools.begin(),restored.input.tools.end(),[](const agent_tool_t &tool){return tool.name=="workspace.list";}));
    }
    static void delivery_tools(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        for (const char *name : {"workspace.read", "workspace.read_batch", "workspace.search",
                 "workspace.list", "workspace.validate", "workspace.patch_set"})
            loop.model_tools.emplace_back(name, "fixture", false, true, true, "workspace");
        auto has = [](const coding_turn_t &turn, const char *name) {
            return std::any_of(turn.input.tools.begin(), turn.input.tools.end(),
                [&](const agent_tool_t &tool) { return tool.name == name; });
        };
        agent_change_proposal_t change;
        change.path = "fixture/src/core.go"; change.operation = "write"; change.content = "package main\n";
        args.changes.push_back(change);
        loop.staged_discovery_rounds = 2;
        loop.acceptance_report = R"({"validation_passed":false,"failed_command_id":"go.build"})";
        coding_turn_t focused;
        loop.select_request_tools(focused);
        assert(!has(focused,"workspace.list") && !has(focused,"workspace.search"));
        assert(has(focused,"workspace.read") && has(focused,"workspace.read_batch") && has(focused,"workspace.patch_set"));
        loop.repair_required_reads[change.path] = "required";
        coding_turn_t missing;
        loop.select_request_tools(missing);
        assert(has(missing,"workspace.search")); // Explicit missing evidence takes precedence.
        loop.repair_required_reads.clear();
        loop.acceptance_report = R"({"validation_passed":true,"validation_scope":"build-only"})";
        coding_turn_t implementing;
        loop.select_request_tools(implementing);
        assert(has(implementing,"workspace.search")); // New feature discovery stays available.
        loop.staged_discovery_rounds = 0;
        loop.acceptance_review_requested = true;
        loop.last_validation_passed = true;
        loop.acceptance_report = R"({"validation_passed":true,"validation_scope":"project","test_status":"passed","browser_acceptance":"disabled","functional_acceptance":"not-verified","source_contract_review":{"status":"review_only","findings":[]}})";
        loop.acceptance_draft = agent_workspace_t::content_sha256(staged_change_fingerprint(args.changes));
        loop.acceptance_baseline = validation_baseline_fingerprint(args.workspace,args.project_path);
        coding_turn_t final;
        loop.select_request_tools(final);
        assert(!has(final,"workspace.validate") && has(final,"workspace.patch_set"));
        assert(final.input.turn_instructions.find("final_evidence_review") != std::string::npos);
        assert(final.input.turn_instructions.find("disabled") != std::string::npos);
        assert(final.input.turn_instructions.find("\"source_contract_review\":{")!=std::string::npos);
        assert(final.input.turn_instructions.find("\"source_contract_review\":\"source_contract_review\"")==std::string::npos);
        args.changes.back().content += "// changed\n";
        coding_turn_t edited;
        loop.select_request_tools(edited);
        assert(has(edited,"workspace.validate"));
        args.changes.back() = change;
        const std::string external = args.runtime_task->user_root + "/fixture/src/external.go";
        std::ofstream(external) << "package main\n// concurrent source change\n";
        coding_turn_t rebased;
        loop.select_request_tools(rebased);
        assert(has(rebased,"workspace.validate"));
        std::remove(external.c_str());
        loop.acceptance_report = R"({"validation_passed":true,"validation_scope":"targeted-tests"})";
        coding_turn_t partial;
        loop.select_request_tools(partial);
        assert(has(partial,"workspace.validate"));
        args.changes.clear();
    }
    static void repair_session(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        const std::string failed = R"({"validation_scope":"project","validation_passed":false,"commands_executed":1,"failed_command_id":"go.test","test_status":"failed","commands":[{"command_id":"go.test","diagnostic":"--- FAIL: TestValue (0.01s)\nvalue_test.go:7: got 1, want 7","diagnostics":[{"path":"src/core/value_test.go","line":7}]}]})";
        assert((focused_go_test_packages(failed) == std::vector<std::string>{"./src/core"}));
        assert(focused_go_test_packages(R"({"failed_command_id":"go.test","commands":[{"command_id":"go.test","diagnostics":[{"path":"../outside_test.go"}]}]})").empty());
        loop.record_acceptance_evidence(failed);
        loop.record_acceptance_evidence(R"({"validation_scope":"build-only","validation_passed":true,"test_status":"not-run"})");
        assert(loop.pending_test_failure_report == failed && loop.acceptance_report == failed);
        loop.record_acceptance_evidence(R"({"validation_scope":"targeted-tests","validation_passed":true,"test_status":"passed"})");
        assert(loop.pending_test_failure_report == failed && loop.acceptance_report == failed);
        const std::string two_packages = R"({"validation_scope":"project","validation_passed":false,"failed_command_id":"go.test","test_status":"failed","commands":[{"command_id":"go.test","diagnostics":[{"path":"src/core/value_test.go"},{"path":"src/adapters/server_test.go"}]}]})";
        const std::string partial = R"({"validation_scope":"targeted-tests","validation_passed":false,"failed_command_id":"go.test","test_status":"failed","commands":[{"command_id":"go.test","diagnostics":[{"path":"src/core/value_test.go"}]}]})";
        loop.record_acceptance_evidence(failed);
        loop.unresolved_repair_contract="stale failure";
        loop.record_acceptance_evidence(R"({"validation_scope":"targeted-tests","validation_passed":true,"test_status":"passed","targeted_packages":["./src/core"]})");
        assert(loop.pending_test_failure_report.empty() && loop.unresolved_repair_contract.empty());
        assert(loop.repair_full_validation_pending);
        coding_turn_t ready;loop.build_request_instructions(ready);
        assert((ready.input.system_prompt+ready.input.turn_instructions).find("focused_tests_passed")!=std::string::npos);
        assert((ready.input.system_prompt+ready.input.turn_instructions).find("\"resolved\":false")==std::string::npos);
        loop.record_acceptance_evidence(two_packages);
        loop.record_acceptance_evidence(R"({"validation_scope":"targeted-tests","validation_passed":true,"test_status":"passed","targeted_packages":["./src/core"]})");
        assert(loop.pending_test_failure_report==two_packages); // Adapters still unresolved.
        loop.record_acceptance_evidence(partial);
        assert(loop.pending_test_failure_report == two_packages);
        assert(focused_go_test_packages(loop.pending_test_failure_report).size() == 2);
        loop.record_acceptance_evidence(R"({"validation_scope":"targeted-tests","validation_passed":true,"test_status":"passed"})");
        assert(loop.pending_test_failure_report == two_packages);
        for (const auto &extra : {std::string(",\"diagnostic\":\"panic: test timed out after 15s\""),
                 std::string(",\"diagnostic_truncated\":true"), std::string(",\"timed_out\":true"),
                 std::string(",\"diagnostic\":\"FAIL\\texample.com/app/src/unmapped\\n\""),
                 std::string(",\"diagnostic\":\"fatal error: concurrent map writes\"")}) {
            const auto mixed = std::string(R"({"failed_command_id":"go.test","commands":[{"command_id":"go.test","diagnostics":[{"path":"src/core/value_test.go"}])") + extra + "}]}";
            assert(focused_go_test_packages(mixed).empty());
            acl::json targeted(mixed.c_str()); assert(targeted.finish());
            targeted.get_root().add_text("validation_scope", "targeted-tests");
            targeted.get_root().add_text("test_status", "failed");
            targeted.get_root().add_bool("validation_passed", false);
            loop.record_acceptance_evidence(serialize_json(targeted.get_root()));
            assert(focused_go_test_packages(loop.pending_test_failure_report).empty());
            loop.record_acceptance_evidence(two_packages);
        }
        loop.record_acceptance_evidence(failed);
        assert(loop.last_validated_fingerprint.empty());
        coding_turn_t request;
        loop.build_request_options(request);
        loop.build_request_instructions(request);
        assert(request.input.thinking_mode == "disabled");
        std::string wire;
        assert(provider_client_t::serialize_completion_request(args.provider, request.input, true, wire));
        assert(wire.find("\"effort\":\"none\"") != std::string::npos);
        assert(wire.find("repair_session") != std::string::npos);
        coding_turn_t first; first.staged_fingerprint_after = "first";
        loop.review_validation_failure(first, failed);
        coding_turn_t second; second.staged_fingerprint_after = "second";
        loop.review_validation_failure(second, failed);
        assert(second.repair_focus.find("repair_reassessment") != std::string::npos);
        coding_turn_t third; third.staged_fingerprint_after = "third";
        loop.review_validation_failure(third, failed);
        assert(loop.repair_failures.stalled_tests());
        assert(third.repair_focus.find("go-test:TestValue")!=std::string::npos);
        loop.record_acceptance_evidence(R"({"validation_scope":"project","validation_passed":true,"test_status":"passed"})");
        assert(loop.pending_test_failure_report.empty());
        assert(loop.unresolved_repair_contract.empty() && loop.repair_supplied_versions.empty());
        assert(repair_failure_signatures("--- FAIL: TestValue (0.01s)\n") ==
            repair_failure_signatures("--- FAIL: TestValue (0.73s)\n"));
    }
    static void adaptive(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        auto check = [&](const char *effort) {
            coding_turn_t turn;loop.build_request_options(turn);
            if(turn.input.reasoning_effort!=effort)std::cerr<<"expected "<<effort<<" got "<<turn.input.reasoning_effort<<" trigger "<<turn.reasoning_trigger<<"\n";
            assert(turn.input.reasoning_effort==effort);
            assert(turn.input.thinking_mode==(std::string(effort)=="none"?"disabled":"enabled"));
            std::string wire;assert(provider_client_t::serialize_completion_request(args.provider,turn.input,true,wire));
            assert(wire.find("adaptive")==std::string::npos);
        };
        check("low"); // Initial implementation, rather than blind high effort.
        loop.acceptance_report=R"({"validation_passed":false})";
        check("none"); // An unclassified/environment failure is not a design problem.
        loop.acceptance_report=R"({"validation_passed":false,"test_status":"failed"})";
        check("low");
        const auto ids=repair_failure_signatures("--- FAIL: TestValue (0.01s)\n");
        loop.repair_failures.observe("go.test","one",ids,false);
        loop.repair_failures.observe("go.test","two",ids,false);
        check("high");check("low"); // One escalation for the same failed revision.
        agent_tool_trace_t read;read.name="workspace.read";args.traces.push_back(read);
        check("none");args.traces.clear();
        loop.reasoning_validation_report=R"({"validation_passed":true,"test_status":"passed"})";
        check("low"); // Empty unit-test draft still represents initial implementation.
        loop.output_budget_recovery_mode=true;check("none");
    }
    static void oversized_checkpoint(const coding_loop_arguments_t &args) {
        auto task=args.runtime_task;const auto session=task->session_id;
        task->session_id=std::string(32,'c');
        agent_progress_store_t store(task->user_root,args.project_path,task->session_id);
        const auto old_changes=task->changes;const auto old_outputs=task->provider_tool_outputs;
        const auto old_response=task->provider_response_id;
        agent_change_proposal_t kept;kept.operation="write";kept.path="fixture/src/checkpoint.go";
        kept.content="package main\n// EXACT_PROPOSAL\n";kept.review_status="pending";
        task->changes={kept};task->provider_response_id="provider-checkpoint";
        task->provider_tool_outputs={{"call-checkpoint","{\"ok\":true}"}};
        const std::string tail="<assistant_tool_call>{\"name\":\"workspace.validate\"}</assistant_tool_call>\n<tool_result>{\"validation_passed\":false,\"diagnostic\":\"LATEST_FAILURE\"}</tool_result>";
        std::string error;
        assert(coding_loop_detail::save_coding_progress(store,args.provider,task,args.project_path,args.original_prompt,
            std::string(300*1024,'x')+tail,"","",45,error));
        agent_progress_t restored;bool found=false;
        assert(store.load(restored,found,error) && found);
        assert(restored.transcript.size()<=kMaxToolTranscriptBytes);
        assert(restored.transcript.find("LATEST_FAILURE")!=std::string::npos);
        assert(restored.original_prompt==args.original_prompt && restored.completed_tool_calls==45);
        assert(restored.staged_changes.size()==1 && restored.staged_changes[0].content==kept.content);
        assert(restored.provider_tool_outputs.size()==1 && restored.provider_tool_outputs[0].output=="{\"ok\":true}");
        assert(restored.provider_response_id=="provider-checkpoint");
        assert(store.remove(error));task->session_id=session;task->changes=old_changes;
        task->provider_tool_outputs=old_outputs;task->provider_response_id=old_response;
    }
    static void test_only_validation(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        coding_turn_t turn;
        turn.trace.ok = true;
        turn.staged_progress = true;
        agent_change_proposal_t test;
        test.path = "fixture/src/main_test.go";
        test.operation = "write";
        test.content = "package main\n// previous\n";
        turn.changes_before_tool.push_back(test);
        test.content = "package main\n// revised\n";
        args.changes.push_back(test);
        turn.tool_result = "{\"ok\":true}";
        assert(loop.finish_validated_result(turn) == coding_tool_loop_t::step_t::proceed);
        assert(args.traces.empty());
        assert(!loop.last_validation_passed);
        assert(loop.last_validated_fingerprint.empty());
        assert(loop.next_tool_guidance.find("test_validation_pending") != std::string::npos);
        args.changes.clear();
    }
    static void bounded_investigation(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        assert(loop.model_tool_call_limit() + 1 == loop.effective_tool_call_limit);
        for (const char *name : {"workspace.read", "workspace.read_batch", "workspace.patch_set"})
            loop.model_tools.emplace_back(name, "fixture", false, true, true, "workspace");
        loop.proposal_tools.push_back(loop.model_tools.back());
        loop.pending_test_failure_report=R"({"failed_command_id":"go.test"})";
        loop.progress_supervisor.observe_investigation(6, false);
        coding_turn_t turn; loop.select_request_tools(turn);
        assert(turn.investigation_checkpoint);
        assert(turn.input.tools.size()==3); // Missing source can still be read.
        loop.supplementary_read_calls=4;
        coding_turn_t bounded;loop.select_request_tools(bounded);
        assert(bounded.input.tools.size()==1 && bounded.input.tools[0].name=="workspace.patch_set");
        loop.repair_required_reads["fixture/missing.go"]="version";
        coding_turn_t required;loop.select_request_tools(required);
        assert(required.input.tools.size()==3); // Required evidence overrides the limit.
        loop.repair_required_reads.clear();
        loop.progress_supervisor.observe_investigation(1,true);
        loop.supplementary_read_calls=0;
        coding_turn_t edited;loop.select_request_tools(edited);
        assert(!edited.investigation_checkpoint && edited.input.tools.size()==3);
    }

    static void budgets(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        coding_turn_t ordinary;
        loop.build_request_options(ordinary);
        assert(ordinary.input.reasoning_effort == "low");
        assert(ordinary.input.thinking_mode == "enabled");
        assert(ordinary.input.max_output_tokens == 65536);
        loop.build_request_instructions(ordinary);
        std::string wire;
        assert(provider_client_t::serialize_completion_request(args.provider, ordinary.input, true, wire));
        assert(wire.find("\"effort\":\"low\"") != std::string::npos);
        assert(wire.find("implementation_contract") != std::string::npos);
        assert(wire.find("generation_checkpoints") == std::string::npos);
        // Unclassified failures do not justify high reasoning.
        loop.acceptance_report = "{\"validation_passed\":false}";
        coding_turn_t repair;
        loop.build_request_options(repair);
        assert(repair.input.reasoning_effort == "low");
        assert(repair.input.max_output_tokens == 65536);
        loop.acceptance_report = "{\"validation_passed\":true}";
        coding_turn_t passed;
        loop.build_request_options(passed);
        assert(passed.input.reasoning_effort == "low");
        loop.output_budget_recovery_mode = true;
        coding_turn_t recovery;
        loop.build_request_options(recovery);
        assert(recovery.input.reasoning_effort == "none");
        assert(recovery.input.thinking_mode == "disabled");
    }
};
}}

static agent_change_proposal_t source(const std::string &path, const std::string &content) {
    agent_change_proposal_t file;
    file.operation = "write"; file.path = path; file.content = content;
    file.draft_hash = agent_workspace_t::content_sha256(content);
    file.generation = 1;
    return file;
}

static void source_retention() {
    std::vector<agent_change_proposal_t> complete_files;
    for (int i=0; i<12; ++i)
        complete_files.push_back(source("project/src/file" + std::to_string(i) + ".go", std::string(2000, 'a')));
    complete_files.push_back(source("project/src/larger.go", std::string(16000, 'b')));
    auto complete = staged_source_records(complete_files, 65536);
    assert(complete.size() <= 65536);
    assert(complete.find("next_query") == std::string::npos);
    assert(complete.find("eof\":false") == std::string::npos);
    for (const auto &file : complete_files) assert(complete.find(file.content) != std::string::npos);
    std::vector<agent_change_proposal_t> files;
    files.push_back(source("project/src/main.go", "package main\nfunc main() {}\n"));
    files.push_back(source("project/src/core.go", std::string(24000, 'a')));
    files.push_back(source("project/src/application.go", std::string(24000, 'b')));
    files.push_back(source("project/src/core_test.go", "package main\n// 测试 \"边界\"\n"));
    auto records = staged_source_records(files, 8192);
    assert(records.size() <= 8192);
    acl::json parsed(records.c_str()); assert(parsed.finish());
    for (const auto &file : files) assert(records.find(file.path) != std::string::npos);
    assert(records.find("next_query") != std::string::npos);
    assert(records.find("eof\":false") != std::string::npos);
    // A large rejected/binary proposal may never displace authoritative source.
    auto rejected = source("project/rejected.go", std::string(100000, 'x'));
    rejected.review_status = "rejected"; files.push_back(rejected);
    files.push_back(source("project/binary.go", std::string("a\0b", 3)));
    records = staged_source_records(files, 8192);
    assert(records.find("rejected.go") == std::string::npos);
    assert(records.find("binary.go") == std::string::npos);
    agent_read_context_t reads(4096);
    const auto main = files[0];
    auto main_record = staged_source_records({main}, 4096);
    assert(reads.put(main.path, main.draft_hash, 0, main_record.substr(1, main_record.size()-2)));
    std::vector<agent_tool_trace_t> traces;
    auto compact = coding_loop_detail::compact_coding_transcript("task", files, traces,
        "<read_working_set>" + reads.records() + "</read_working_set>", false, &reads);
    const std::string marker = "package main\\nfunc main() {}\\n";
    assert(compact.find(marker) != std::string::npos);
    assert(compact.find(marker) == compact.rfind(marker));
    const auto staged = compact.substr(compact.find("<staged_working_set>"));
    assert(staged.find(main.path) == std::string::npos);
    // A changed draft cannot be mistaken for its formerly retained prefix.
    files[0] = source(main.path, "package main\nfunc main() { println(42) }\n");
    compact = coding_loop_detail::compact_coding_transcript("task", files, traces,
        "<read_working_set>" + reads.records() + "</read_working_set>", false, &reads);
    assert(compact.substr(compact.find("<staged_working_set>")).find(files[0].path) != std::string::npos);
    assert(staged_source_records(files, 2) == "[]");
    // Successful source pages may be cached, but partial batch errors must
    // still be delivered in the next model request.
    const auto partial = "{\"failed_files\":1,\"files\":" + main_record +
        ",\"errors\":[{\"path\":\"project/src\",\"path_state\":\"directory\"}]}";
    agent_read_context_t partial_reads(8192);
    assert(!remember_read_result("workspace.read_batch", partial, partial_reads));
    assert(partial_reads.contains(main.path, main.draft_hash, 0));
    assert(!read_result_is_retained("workspace.read_batch", partial, partial_reads));
    const auto success = "{\"files\":" + main_record + "}";
    assert(read_result_is_retained("workspace.read_batch", success, partial_reads));
    const auto omitted = "{\"files\":" + main_record +
        ",\"omitted_paths\":[\"project/missing-page.go\"],\"next_content\":\"[]\"}";
    assert(!remember_read_result("workspace.read_batch", omitted, partial_reads));
    assert(!read_result_is_retained("workspace.read_batch", omitted, partial_reads));

}

static void test_change_scope() {
    auto implementation = source("project/src/main.go", "package main\n");
    auto test = source("project/src/main_test.go", "package main\n// old\n");
    auto revised = source(test.path, "package main\n// new\n");
    assert(coding_only_test_changes({implementation,test}, {implementation,revised}));
    assert(coding_only_test_changes({}, {source("project/test_main.py", "assert True")}));
    assert(!coding_only_test_changes({test}, {test}));
    assert(!coding_only_test_changes({test}, {}));
    assert(!coding_only_test_changes({}, {revised,source("project/build.sh", "go test ./...\n")}));
    implementation.content += "func main() {}\n";
    assert(!coding_only_test_changes({test}, {implementation,revised}));
    revised.operation = "delete";
    assert(!coding_only_test_changes({test}, {revised}));
}

static void request_source_compaction() {
    const std::string body="package fixture\n// unique-source-anchor\n"+std::string(4096,'x')+"\n";
    const auto version=agent_workspace_t::content_sha256(body);
    auto page=[&](const std::string &hash,size_t offset,const std::string &bytes) {
        acl::json json;auto &r=json.create_node();r.add_text("path","fixture/src/a.go")
            .add_text("file_sha256",hash.c_str()).add_number("offset",offset).add_number("next_offset",offset+bytes.size())
            .add_text("content",bytes.c_str());return std::string(r.to_string().c_str());
    };
    completion_request_t request;
    completion_tool_exchange_t exchange;completion_tool_call_t call;
    call.name="workspace.propose";call.path="fixture/src/a.go";call.content=body;
    exchange.calls.push_back(call);exchange.outputs.push_back({"call",page(version,0,body)});
    request.tool_history.push_back(exchange);
    request.user_prompt="task contract unchanged\n<staged_working_set>["+page(version,0,body)+"]</staged_working_set>";
    request.turn_instructions="<repair_review_sources>["+page(version,100,body.substr(100,1024))+"]</repair_review_sources>";
    const auto canonical=request;
    const auto stats=source_request_compactor_t("fixture").compact(request);
    if(stats.records_reused!=3)std::cerr << "dedup records=" << stats.records_reused << " bytes=" << stats.source_bytes_elided
        << " user_ref=" << (request.user_prompt.find("source_ref")!=std::string::npos)
        << " output_ref=" << (request.tool_history[0].outputs[0].output.find("source_ref")!=std::string::npos)
        << " instruction_ref=" << (request.turn_instructions.find("source_ref")!=std::string::npos) << "\n";
    assert(stats.records_reused==3 && stats.source_bytes_elided==body.size()*2+1024);
    assert(stats.bytes_after<stats.bytes_before && request.tool_history[0].calls[0].content==body);
    assert(canonical.user_prompt.find("unique-source-anchor")!=std::string::npos);
    assert(request.user_prompt.find("unique-source-anchor")==std::string::npos);
    assert(request.user_prompt.find("source_ref")!=std::string::npos);
    // A new version or missing page must remain present, even when its path is known.
    auto changed=canonical;changed.turn_instructions="<read_working_set>["+page("different-version",0,body)+"]</read_working_set>";
    source_request_compactor_t("fixture").compact(changed);
    assert(changed.turn_instructions.find("unique-source-anchor")!=std::string::npos);
    // A retained prefix cannot stand in for a missing range, and identical
    // bytes from another path do not establish the requested file's identity.
    completion_request_t ranges;
    ranges.user_prompt="<read_working_set>["+page(version,0,body.substr(0,512))+"]</read_working_set>";
    ranges.turn_instructions="<repair_review_sources>["+page(version,512,body.substr(512,512))+"]</repair_review_sources>";
    auto missing=source_request_compactor_t("fixture").compact(ranges);
    assert(missing.records_reused==0 && ranges.turn_instructions.find("content_omitted")==std::string::npos);
    auto other=canonical;
    auto at=other.turn_instructions.find("fixture/src/a.go");
    other.turn_instructions.replace(at,std::string("fixture/src/a.go").size(),"fixture/src/b.go");
    source_request_compactor_t("fixture").compact(other);
    assert(other.turn_instructions.find("content_omitted")==std::string::npos);
    // Stateful requests do not send the local user prompt/history. Never elide
    // source based on an anchor that exists only in those unsent local fields.
    auto stateful=canonical;stateful.previous_response_id="provider-state";
    stateful.tool_outputs.push_back({"call",page(version,0,body)});
    const auto stateful_stats=source_request_compactor_t("fixture").compact(stateful);
    assert(stateful_stats.records_reused==0 && stateful.tool_outputs[0].output.find("unique-source-anchor")!=std::string::npos);
    source_request_compactor_t reusable("fixture");
    auto first=stateful, second=stateful;
    assert(reusable.compact(first).records_reused==0);
    assert(reusable.compact(second).records_reused==0);

}

static void retained_read_coverage() {
    agent_read_context_t retained(4096);
    agent_read_coverage_t coverage;
    const auto page=[](const char *version, int offset, const char *content) {
        acl::json j;auto &o=j.create_node();o.add_bool("ok",true).add_text("path","p/clock.go")
            .add_text("file_sha256",version).add_number("offset",offset).add_text("content",content);
        return serialize_json(o);
    };
    const auto first=annotate_read_coverage("workspace.read",page("v1",0,"0123456789"),coverage,false);
    assert(remember_read_result("workspace.read",first,retained));
    auto repeated=annotate_read_coverage("workspace.read",page("v1",0,"0123456789"),coverage,false);
    assert(remember_read_result("workspace.read",repeated,retained));
    restore_read_coverage(retained,coverage); // Exact retained bytes survive history compaction.
    repeated=annotate_read_coverage("workspace.read",page("v1",4,"456789"),coverage,false);
    acl::json parsed(repeated.c_str());assert(parsed.finish());
    assert(json_number(parsed["new_read_bytes"],-1)==0 && json_bool(parsed["low_value_read"],false));
    std::vector<std::string> observations;collect_observation_signatures("workspace.read",repeated,observations);
    assert(observations.empty()); // Changing an offset cannot reset the guard.
    auto extended=annotate_read_coverage("workspace.read",page("v1",8,"89abcd"),coverage,false);
    acl::json extra(extended.c_str());assert(extra.finish() && json_number(extra["new_read_bytes"],-1)==4);
    auto changed=annotate_read_coverage("workspace.read",page("v2",0,"0123456789"),coverage,false);
    acl::json fresh(changed.c_str());assert(fresh.finish() && json_number(fresh["new_read_bytes"],-1)==10);
    retained.clear();restore_read_coverage(retained,coverage);
    auto evicted=annotate_read_coverage("workspace.read",page("v2",0,"0123456789"),coverage,false);
    acl::json retry(evicted.c_str());assert(retry.finish() && json_number(retry["new_read_bytes"],-1)==10);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(file_ops::make_private_directory(argv[1]));
    const std::string root = std::string(argv[1]) + "/run-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()), project = "fixture";
    assert(file_ops::make_private_directory(root));
    assert(file_ops::make_private_directory(root + "/fixture"));
    assert(file_ops::make_private_directory(root + "/fixture/src"));
    std::ofstream(root + "/fixture/go.mod") << "module fixture\n";
    std::ofstream(root + "/fixture/.gitignore") << "build/\n# bootstrap-ignore-marker\n";
    assert(file_ops::make_private_directory(root + "/fixture/src/existing_module"));
    std::ofstream(root + "/fixture/build.sh") << "#!/bin/sh\ngo build ./src\n";
    std::ofstream(root + "/fixture/src/main.go") << "package main\n// bootstrap-entry-marker\nfunc greeting() string { return \"hello\" }\nfunc main() {}\n";
    std::ofstream(root + "/fixture/src/main_test.go") << "package main\n// bootstrap-test-marker\nfunc checkGreeting() { _ = greeting() }\n";
    agent_workspace_t workspace(root);
    {
        agent_draft_store_t snapshot(root, project, std::string(32, 'c'));
        size_t skipped = 0; bool reused = false; std::string error, before;
        assert(snapshot.materialize({}, skipped, error));
        agent_workspace_t draft(snapshot.root_path());
        assert(draft.tree_sha256("", before, error, {}, {".libai-go-app"}));
        std::ofstream(snapshot.root_path()+"/.libai-go-app", std::ios::binary) << "verified artifact";
        assert(snapshot.record_validation_outputs(before, {".libai-go-app"}, error));
        assert(snapshot.materialize({}, skipped, error, &reused) && reused);
        std::ofstream(snapshot.root_path()+"/src/main.go", std::ios::app) << "// test changed source\n";
        assert(!snapshot.record_validation_outputs(before, {".libai-go-app"}, error));
        reused = false;
        assert(snapshot.materialize({}, skipped, error, &reused) && !reused);
        agent_change_proposal_t directory, script;
        directory.operation="mkdir";directory.path=project+"/tests";
        script.operation="write";script.path=project+"/tests/acceptance.test.cjs";script.content="// first\n";
        std::vector<agent_change_proposal_t> edits{directory,script};
        assert(snapshot.materialize(edits,skipped,error));
        assert(draft.tree_sha256("",before,error,{}, {".libai-go-app"}));
        std::ofstream(snapshot.root_path()+"/.libai-go-app") << "verified artifact";
        assert(snapshot.record_validation_outputs(before,{".libai-go-app"},error));
        edits[1].content="// changed assertion\n";
        assert(snapshot.materialize(edits,skipped,error));
        std::string artifact;bool truncated=false;
        assert(draft.read(".libai-go-app",artifact,truncated,error) && artifact=="verified artifact");
        assert(snapshot.remove(error));
    }
    provider_config_t provider; provider.id = "fixture"; provider.protocol = "openai_responses";
    provider.model = "deepseek-v4-flash"; provider.allow_file_content = true;
    provider.base_url = "http://127.0.0.1:1/v1";
    std::string initial, error;
    assert(compose_initial_prompt(workspace, provider, root, project, "Implement the project.",
        "", "standard", "", initial, error, false));
    assert(initial.find("module fixture") != std::string::npos);
    assert(initial.find("bootstrap-entry-marker") != std::string::npos);
    assert(initial.find("bootstrap-test-marker") != std::string::npos);
    assert(initial.find("bootstrap-ignore-marker") != std::string::npos);
    auto directory_begin = initial.find("<bootstrap_directories>");
    assert(directory_begin != std::string::npos);
    directory_begin += std::string("<bootstrap_directories>").size();
    auto directory_end = initial.find("</bootstrap_directories>", directory_begin);
    const auto directories = initial.substr(directory_begin, directory_end-directory_begin);
    acl::json directory_json(directories.c_str()); assert(directory_json.finish());
    assert(directories.find("fixture/src/existing_module") != std::string::npos);
    assert(directories.find("fixture/src/main_test.go") != std::string::npos);
    assert(directories.find("\"complete\":true") != std::string::npos);
    assert(initial.find("project_baseline") != std::string::npos);
    provider.allow_file_content = false;
    std::string restricted;
    assert(compose_initial_prompt(workspace, provider, root, project, "Implement the project.",
        "", "standard", "", restricted, error, false));
    assert(restricted.find("bootstrap-entry-marker") == std::string::npos);
    assert(restricted.find("bootstrap-ignore-marker") == std::string::npos);
    assert(restricted.find("<bootstrap_directories>") == std::string::npos);
    // A large directory is reported as incomplete instead of presenting a
    // truncated listing as authoritative absence of other source files.
    for (int i=0; i<70; ++i)
        std::ofstream(root + "/fixture/src/extra" + std::to_string(i) + ".txt") << "fixture\n";
    provider.allow_file_content = true;
    std::string bounded;
    assert(compose_initial_prompt(workspace, provider, root, project, "Implement the project.",
        "", "standard", "", bounded, error, false));
    auto bounded_begin = bounded.find("<bootstrap_directories>");
    auto bounded_end = bounded.find("</bootstrap_directories>", bounded_begin);
    auto bounded_listing = bounded.substr(bounded_begin, bounded_end-bounded_begin);
    assert(bounded_listing.find("\"complete\":false") != std::string::npos);
    assert(bounded_listing.size() < 8192);
    auto task = std::make_shared<agent_runtime_task_t>(); task->id = std::string(32, 'b');
    task->user_root = root; task->project_path = project;
    completion_result_t output; std::vector<completion_image_t> images;
    std::vector<agent_tool_trace_t> traces; std::vector<agent_change_proposal_t> changes;
    std::string key, prompt="Implement the project.", reasoning, thinking="enabled", effort,
        memory, summary, title;
    sandbox_limits_t limits; size_t rejected=0;
    coding_loop_arguments_t args{provider,key,workspace,project,prompt,initial,reasoning,images,
        0,64,131072,6,32768,limits,thinking,effort,output,traces,changes,memory,summary,title,rejected,task,error};
    coding_loop_test_access_t::budgets(args);
    coding_loop_test_access_t::bounded_investigation(args);
    coding_loop_test_access_t::test_only_validation(args);
    thinking = "adaptive"; effort = "max";
    coding_loop_test_access_t::adaptive(args);
    thinking = "enabled"; effort.clear();
    thinking = "disabled";
    coding_loop_test_access_t::repair_session(args);
    coding_loop_test_access_t::delivery_tools(args);
    coding_loop_test_access_t::compile_cache(args);
    coding_loop_test_access_t::proposal_recovery(args);
    coding_loop_test_access_t::repair_evidence_scope(args);
    coding_loop_test_access_t::no_mutation_claim(args);
    coding_loop_test_access_t::oversized_checkpoint(args);
    thinking = "enabled";
    auto large = coding_turn_budget("low", "adaptive", 131072, true, true, true, true);
    assert(large.thinking == "enabled" && large.effort == "high" && large.output_tokens == 131072);
    auto toggle_only = coding_turn_budget("", "adaptive", 131072, false, true, false);
    assert(toggle_only.thinking == "disabled" && toggle_only.effort.empty());
    assert(coding_turn_budget("high", "enabled", 131072, true, true, false).effort == "high");
    assert(coding_turn_budget("max", "enabled", 131072, true, true, false).output_tokens == 131072);
    assert(coding_turn_budget("", "disabled", 131072, true, true, false).effort == "none");
    assert(coding_turn_budget("", "enabled", 4096, true, true, false).output_tokens == 4096);
    assert(coding_turn_budget("", "enabled", 131072, false, true, false).effort.empty());
    source_retention();
    retained_read_coverage();
    test_change_scope();
    request_source_compaction();
    const auto behavior = coding_behavior_checks("Implement a shared application");
    assert(behavior.size() < 1500);
    assert(behavior.find("behavior_contract") != std::string::npos);
    assert(behavior.find("first and repeated updates") != std::string::npos);
    assert(behavior.find("valid state remains valid") != std::string::npos);
    assert(behavior.find("actual serialization, identity") != std::string::npos);
    assert(coding_behavior_checks("Implement a game") == behavior);
    assert(coding_behavior_checks("Implement a compiler library") == behavior);
    std::cout << "PASS: adaptive requests, recovery precedence, bootstrap visibility and bounded versioned source retention\n";
}
