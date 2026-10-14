#include <libai/agent.h>
#include <cassert>
using namespace libai;
int main() {
    int effects=0;
    auto model=std::make_shared<function_model_t>([](const model_request_t &q,const stream_sink_t &) {
        assert(q.tools.empty()); assert(q.max_output_tokens==7);
        model_response_t r; tool_call_t c;c.id="unexpected";c.name="write";
        r.calls.push_back(c);return r;
    });
    agent_t agent(model);tool_spec_t t;t.name="write";
    agent.add_tool(t,[&](const tool_call_t &,const tool_context_t &){++effects;return tool_result_t::success("ok");});
    agent.configure_model(R"({"version":1,"tool_calling":false,"max_output_tokens":7})");
    bool rejected=false;
    try{agent.configure_model(R"({"version":1,"tool_calling":"false"})");}catch(const std::exception&){rejected=true;}
    assert(rejected);
    agent_session_t s;assert(agent.run(s,"write").status==run_status_t::protocol_error);assert(effects==0);
    auto multi=std::make_shared<function_model_t>([](const model_request_t &q,const stream_sink_t &){
        assert(!q.profile.multiple_tool_calls);model_response_t r;
        tool_call_t c;c.id="a";c.name="write";r.calls.push_back(c);c.id="b";r.calls.push_back(c);return r;
    });
    agent_t a(multi);a.add_tool(t,[&](const tool_call_t &,const tool_context_t &){++effects;return tool_result_t::success("ok");});
    a.configure_model(R"({"version":1,"multiple_tool_calls":false})");agent_session_t ms;
    assert(a.run(ms,"write").status==run_status_t::protocol_error && effects==0);
    int model_calls=0;
    agent_t bounded(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++model_calls;return model_response_t();}));
    bounded.configure_model(R"({"version":1,"max_context_bytes":30})");agent_session_t bs;
    assert(bounded.run(bs,std::string(100,'x')).status==run_status_t::budget_exceeded && model_calls==0);
    for (const char *wire:{"{}",R"({"version":2})",R"({"version":1,"max_output_tokens":0})",R"({"version":1,"vision":true})",R"({"version":1,"tool_calling":true,"tool_calling":false})"}) {
        bool invalid=false;try{parse_model_profile(wire);}catch(const std::exception&){invalid=true;}assert(invalid);
    }
}
