#include <libai/agent.h>
#include <libai/sdk/governance.h>
#include <cassert>
#include <cstdio>
using namespace libai;
static void check(const char *schema,const char *input,bool valid) {
    static int index=0;++index;
    const auto r=validate_json_schema(schema,input);
    if (r.valid!=valid) std::fprintf(stderr,"case %d code=%s keyword=%s\n",index,r.code.c_str(),r.keyword.c_str());
    assert(r.valid==valid);
}
int main() {
    check(R"({"type":"object","required":["n"],"properties":{"n":{"type":"integer","minimum":1}},"additionalProperties":false})",R"({"n":2})",true);
    check(R"({"type":"object","required":["n"],"properties":{"n":{"type":"integer"}}})",R"({"n":"2"})",false);
    check(R"({"type":"object","required":["n"]})","{}",false);
    check(R"({"type":"object","additionalProperties":false})",R"({"extra":1})",false);
    check(R"({"type":"array","items":{"type":"integer"},"minItems":2,"uniqueItems":true})","[1,2]",true);
    check(R"({"type":"array","items":{"type":"integer"},"uniqueItems":true})","[1,1]",false);
    check(R"({"type":"array","items":[{"type":"integer"},{"type":"string"}],"additionalItems":false})",R"([1,"a",3])",false);
    check(R"({"allOf":[{"minimum":1},{"maximum":4}]})","3",true);
    check(R"({"oneOf":[{"type":"number"},{"type":"integer"}]})","2",false);
    check(R"({"anyOf":[{"type":"integer"},{"type":"boolean"}]})","true",true);
    check(R"({"not":{"type":"integer"}})","2",false);
    check(R"({"dependencies":{"a":["b"]}})",R"({"a":1})",false);
    check(R"({"additionalProperties":{"type":"string"}})",R"({"a":2})",false);
    check(R"({"type":"string","minLength":2,"maxLength":2})",R"("😀中")",true);
    check(R"({"type":"string","pattern":"^[A-Z]+$"})",R"("abc")",false);
    check(R"({"definitions":{"positive":{"type":"integer","minimum":0}},"$ref":"#/definitions/positive"})","-1",false);
    check(R"({"enum":[{"a":1,"b":2}]})",R"({"b":2,"a":1})",true);
    // Draft-04 defines integer without a fraction/exponent part.
    check(R"({"type":"integer"})","1.0",false);
    check(R"({"type":"number"})","1.0",true);
    check(R"({"type":"object"})",R"({"a":1,"\u0061":2})",false);
    check("{}",R"({"a":1,"a":2})",false);
    check(R"({"required":42})","{}",false);
    check(R"({"$ref":"http://example.invalid/schema"})","{}",false);
    check(R"({"$ref":"#missing"})","{}",false);
    check(R"({"const":1})","2",false);
    check(R"({"propertyNames":{"pattern":"^[a-z]+$"}})",R"({"INVALID":1})",false);
    check(R"({"pattern":"("})",R"("anything")",false);
    check(R"({"$ref":"#"})","{}",false);
    check(R"({"allOf":[{"$ref":"#"}]})","{}",false);
    check(R"({"definitions":{"a":{"$ref":"#/definitions/b"},"b":{"$ref":"#/definitions/a"}},"$ref":"#/definitions/a"})","{}",false);
    std::string deep(70,'[');deep+="0";deep+=std::string(70,']');
    assert(!validate_json_schema("{}",deep).valid);
    int calls=0,authorized=0,round=0,ended_errors=0;
    auto model=std::make_shared<function_model_t>([&](const model_request_t &r,const stream_sink_t &){
        model_response_t out;
        if (round++==0) {
            tool_call_t a;a.id="first";a.name="charge";a.arguments=R"({"amount":2})";
            auto b=a;b.id="second";b.arguments=R"({"amount":"not a number"})";
            out.calls={a,b};
        } else if (round==2) {
            assert(r.messages[2].tool_error && r.messages[3].tool_error);
            tool_call_t a;a.id="repair";a.name="charge";a.arguments=R"({"amount":3})";
            out.calls={a};
        } else out.text="finished";
        return out;
    });
    agent_t agent(model);tool_spec_t tool;tool.name="charge";tool.requires_approval=true;
    tool.parameters=R"({"type":"object","required":["amount"],"properties":{"amount":{"type":"integer"}},"additionalProperties":false})";
    agent.add_tool(tool,[&](const tool_call_t &,const tool_context_t &){++calls;return tool_result_t::success("ok");});
    run_options_t options;options.authorize=[&](const tool_spec_t &,const tool_call_t &){++authorized;return true;};
    options.on_event=[&](const agent_event_t &e){if(e.kind==agent_event_t::kind_t::tool_end && e.error) ++ended_errors;};
    agent_session_t session;auto result=agent.run(session,"charge",options);
    assert(result.status==run_status_t::completed && calls==1 && authorized==1 && ended_errors==2);
    assert(result.tool_calls==1 && result.model_calls==3);
    bool rejected=false;
    try {tool.name="bad";tool.parameters=R"({"required":42})";agent.add_tool(tool,[](const tool_call_t &,const tool_context_t &){return tool_result_t();});}
    catch(const std::exception&){rejected=true;}assert(rejected);
    assert(governance_call("capabilities").find("draft4")!=std::string::npos);
    int streamed=0;
    auto streaming=std::make_shared<function_model_t>([](const model_request_t &,const stream_sink_t &sink){
        assert(!sink(std::string(4096,'x')));
        model_response_t r;r.text="done";return r;
    });
    agent_t limited(streaming);agent_session_t stream_session;run_options_t stream_options;
    stream_options.max_context_bytes=1024;
    stream_options.on_text=[&](const std::string &s){streamed+=s.size();return true;};
    auto stopped=limited.run(stream_session,"work",stream_options);
    assert(stopped.status==run_status_t::budget_exceeded && streamed==0);
}
