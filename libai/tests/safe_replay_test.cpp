#include <libai/agent.h>
#include <cassert>
using namespace libai;
struct Store : execution_store_t {
    execution_snapshot_t snapshot; bool leased=false, fail_done=false;
    execution_snapshot_t open(const std::string &) override {assert(!leased);leased=true;return snapshot;}
    std::string commit(const std::string &,const std::string &expected,const std::string &state) override {
        assert(leased && expected==snapshot.revision);
        if(fail_done && state.find("\"state\":\"done\"")!=std::string::npos){fail_done=false;throw std::runtime_error("result not persisted");}
        snapshot.state_json=state;snapshot.revision=std::to_string(snapshot.revision.empty()?1:std::stoull(snapshot.revision)+1);return snapshot.revision;
    }
    void close(const std::string &) noexcept override {leased=false;}
};
int main() {
    auto store=std::make_shared<Store>();run_options_t options;options.execution_store=store;options.execution_scope="read";
    int models=0, handlers=0;
    auto model=std::make_shared<function_model_t>([&](const model_request_t &request,const stream_sink_t &){
        ++models;model_response_t result;
        if(request.messages.back().role==message_t::role_t::tool){assert(request.messages.back().tool_error);result.text="handled failure";}
        else {tool_call_t call;call.id="a";call.name="read";result.calls={call};}return result;
    });
    tool_spec_t spec;spec.name="read";spec.side_effect_free=true;
    auto configure=[&](agent_t &agent){agent.add_tool(spec,[&](const tool_call_t &,const tool_context_t &context){
        ++handlers;assert(context.idempotency_key=="read.0");return tool_result_t::failure("not found");
    });};
    agent_t first(model);configure(first);agent_session_t session;store->fail_done=true;
    assert(first.run(session,"input",options).status==run_status_t::recovery_required && handlers==1 && models==1);
    agent_t changed(model);auto mutation=spec;mutation.side_effect_free=false;
    changed.add_tool(mutation,[](const tool_call_t &,const tool_context_t &)->tool_result_t{assert(false);return tool_result_t();});
    agent_session_t fresh;assert(changed.run(fresh,"input",options).status==run_status_t::recovery_required && handlers==1);
    agent_t second(model);configure(second);
    auto lower=options;lower.max_tool_calls=1;
    assert(second.run(fresh,"input",lower).status==run_status_t::budget_exceeded && handlers==1);
    auto result=second.run(fresh,"input",options);
    assert(result.status==run_status_t::completed && handlers==2 && models==2 && result.tool_calls==2);
    assert(second.run(fresh,"input",options).status==run_status_t::completed && handlers==2 && models==2);
    auto attempts=store->snapshot.state_json.find("\"attempts\":2");assert(attempts!=std::string::npos);
    store->snapshot.state_json.replace(attempts,12,"\"attempts\":0");
    auto total=store->snapshot.state_json.find("\"tool_calls\":2");assert(total!=std::string::npos);
    store->snapshot.state_json.replace(total,14,"\"tool_calls\":0");
    assert(second.run(fresh,"input",options).status==run_status_t::recovery_required && handlers==2 && models==2);
    // Model arguments cannot claim a mutating tool is side-effect-free.
    store=std::make_shared<Store>();options.execution_store=store;options.execution_scope="mutation";
    agent_t unsafe(std::make_shared<function_model_t>([](const model_request_t &,const stream_sink_t &){
        model_response_t r;tool_call_t c;c.id="b";c.name="write";c.arguments="{\"side_effect_free\":true}";r.calls={c};return r;
    }));tool_spec_t write;write.name="write";write.parameters="{}";
    unsafe.add_tool(write,[](const tool_call_t &,const tool_context_t &){return tool_result_t::failure("effect may have happened");});
    session.messages.clear();assert(unsafe.run(session,"input",options).status==run_status_t::recovery_required);
}
