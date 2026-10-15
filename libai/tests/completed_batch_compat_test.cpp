#include <libai/runtime/coding_tool_loop_internal.h>
#include <libai/common/file_ops.h>
#include <cassert>
#include <fstream>
#include <iostream>

using namespace libai::ai;
using namespace action::agent_detail;

namespace action { namespace agent_detail {
struct coding_loop_test_access_t {
    static void check(const coding_loop_arguments_t &args) {
        coding_tool_loop_t loop(args);
        coding_turn_t turn;
        turn.output.response_status = "incomplete";
        turn.output.native_tool_call = true;
        completion_tool_call_t call;
        call.id = "call"; call.name = "workspace.propose_batch";
        call.arguments_json = "{\"content\":\"not a file list\"}";
        turn.output.tool_calls.push_back(call);
        turn.message.tool.name = "workspace.propose_batch";
        turn.message.tool.content = "not a file list";
        turn.tool_result = "{\"ok\":false,\"code\":\"proposal_batch_unparsed\",\"error\":\"invalid batch\"}";
        assert(loop.finish_validated_result(turn) == coding_tool_loop_t::step_t::proceed);
        turn.output.response_status = "completed";
        turn.repair_blocked = true;
        assert(loop.finish_validated_result(turn) == coding_tool_loop_t::step_t::proceed);
        turn.repair_blocked = false;
        turn.tool_result = "{\"ok\":false,\"error\":\"path escapes project\"}";
        assert(loop.finish_validated_result(turn) == coding_tool_loop_t::step_t::proceed);
        turn.tool_result = "{\"ok\":false,\"code\":\"proposal_batch_unparsed\",\"error\":\"invalid batch\"}";
        assert(loop.finish_validated_result(turn) == coding_tool_loop_t::step_t::completed);
        assert(args.changes.empty());
        assert(args.final_output.text.find("does not establish project completion") != std::string::npos);
    }
};
}}

int main(int argc, char **argv) {
    assert(argc == 2); const std::string root = argv[1], project = "fixture";
    assert(file_ops::make_private_directory(root));
    assert(file_ops::make_private_directory(root + "/fixture"));
    agent_workspace_t workspace(root); provider_config_t provider;
    auto task = std::make_shared<agent_runtime_task_t>(); task->id = std::string(32, 'b');
    task->user_root = root; task->project_path = project; task->ui_language = "en";
    completion_result_t output; std::vector<completion_image_t> images;
    std::vector<agent_tool_trace_t> traces; std::vector<agent_change_proposal_t> changes;
    std::string key, prompt="Generate files.", initial=prompt, reasoning, thinking, effort,
        memory, summary, title, error;
    sandbox_limits_t limits; size_t rejected=0;
    coding_loop_arguments_t args{provider,key,workspace,project,prompt,initial,reasoning,images,
        0,64,131072,6,32768,limits,thinking,effort,output,traces,changes,memory,summary,title,rejected,task,error};
    coding_loop_test_access_t::check(args);
    agent_request_store_t archive(root, project, task->id);
    std::string response, request;
    assert(archive.append_response("{\"content\":\"raw \\\"quote\\\"\"}", response, error));
    assert(response.find("responses/response-") != std::string::npos);
    assert(archive.append("{}", request, error));
    assert(request.find("requests/request-") != std::string::npos);
    assert(!archive.append_response("not JSON", response, error));
    std::cout << "PASS: completed-only compatibility; incomplete, policy/path errors remain strict; response archive is separate\n";
}
