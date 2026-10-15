#include <libai/runtime/coding_runtime.h>
#include <libai/runtime/coding_tool_loop_internal.h>
#include <libai/context/coding_turn_budget.h>
#include <libai/context/coding_test_changes.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <chrono>

using namespace libai::ai;
using namespace action::agent_detail;

namespace action { namespace agent_detail {
struct coding_loop_test_access_t {
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
        loop.record_acceptance_evidence(R"({"validation_scope":"project","validation_passed":true,"test_status":"passed"})");
        assert(loop.pending_test_failure_report.empty());
        assert(repair_failure_signatures("--- FAIL: TestValue (0.01s)\n") ==
            repair_failure_signatures("--- FAIL: TestValue (0.73s)\n"));
    }
    static void adaptive(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        auto check = [&](bool enabled) {
            coding_turn_t turn;
            loop.build_request_options(turn);
            assert(turn.input.thinking_mode == (enabled ? "enabled" : "disabled"));
            assert(turn.input.reasoning_effort == (enabled ? "high" : "none"));
            assert(turn.reasoning_policy == (enabled ? "adaptive_thinking_on" : "adaptive_thinking_off"));
            std::string wire;
            assert(provider_client_t::serialize_completion_request(args.provider, turn.input, true, wire));
            assert(wire.find("adaptive") == std::string::npos);
            assert(wire.find(enabled ? "\"effort\":\"high\"" : "\"effort\":\"none\"") != std::string::npos);
        };
        check(false);
        loop.acceptance_report = "{\"validation_passed\":false}";
        check(true);
        loop.acceptance_report = "{\"validation_passed\":true}";
        check(false);
        loop.acceptance_report = "{\"validation_passed\":false}";
        loop.output_budget_recovery_mode = true;
        coding_turn_t recovery;
        loop.build_request_options(recovery);
        assert(recovery.input.thinking_mode == "disabled");
        assert(recovery.input.reasoning_effort == "none");
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
        assert(wire.find("read_before_design") != std::string::npos);
        assert(wire.find("call the necessary read/list tools immediately") != std::string::npos);
        // A real validator failure escalates; successful later evidence clears it.
        loop.acceptance_report = "{\"validation_passed\":false}";
        coding_turn_t repair;
        loop.build_request_options(repair);
        assert(repair.input.reasoning_effort == "high");
        assert(repair.input.max_output_tokens == 131072);
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
    const auto &main = files[0];
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
    provider_config_t provider; provider.id = "fixture"; provider.protocol = "openai_responses";
    provider.model = "deepseek-v4-flash"; provider.allow_file_content = true;
    provider.base_url = "http://127.0.0.1:1/v1";
    std::string initial, error;
    assert(compose_initial_prompt(workspace, provider, root, project, "Implement the project.",
        "", "standard", "", initial, error, false));
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
    coding_loop_test_access_t::test_only_validation(args);
    thinking = "adaptive"; effort = "max";
    coding_loop_test_access_t::adaptive(args);
    thinking = "enabled"; effort.clear();
    thinking = "disabled";
    coding_loop_test_access_t::repair_session(args);
    thinking = "enabled";
    auto large = coding_turn_budget("low", "adaptive", 131072, true, true, true);
    assert(large.thinking == "enabled" && large.effort == "high" && large.output_tokens == 131072);
    auto toggle_only = coding_turn_budget("", "adaptive", 131072, false, true, false);
    assert(toggle_only.thinking == "disabled" && toggle_only.effort.empty());
    assert(coding_turn_budget("high", "enabled", 131072, true, true, false).effort == "high");
    assert(coding_turn_budget("max", "enabled", 131072, true, true, false).output_tokens == 131072);
    assert(coding_turn_budget("", "disabled", 131072, true, true, false).effort.empty());
    assert(coding_turn_budget("", "enabled", 4096, true, true, false).output_tokens == 4096);
    assert(coding_turn_budget("", "enabled", 131072, false, true, false).effort.empty());
    source_retention();
    test_change_scope();
    std::cout << "PASS: adaptive requests, recovery precedence, bootstrap visibility and bounded versioned source retention\n";
}
