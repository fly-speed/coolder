#include <libai/mcp.h>
#include <cassert>
using namespace libai;
using namespace mcp_detail;
struct Transport:mcp_transport_t {
    int initialized=0,listed=0,called=0,closed=0,cancelled=0;
    bool wrong_version=false,wrong_id=false,bad_catalog=false,fail_effect=false,error_result=false,cancel_call=false;
    std::string key;
    std::string request(const std::string &wire,const mcp_request_context_t &context) override {
        json::Document q;governance_detail::parse_json(wire,q);auto method=text(q,"method");
        assert(q["id"].GetUint64()==context.request_id);
        json::Document d;d.SetObject();string(d,"jsonrpc","2.0");d.AddMember("id",context.request_id+(wrong_id?1:0),d.GetAllocator());
        json::Document r;
        if(method=="initialize") {
            assert(!initialized && context.protocol_version.empty() && q["params"]["capabilities"].ObjectEmpty());
            const std::string response=std::string(R"({"protocolVersion":")")+(wrong_version?"2024-11-05":"2025-11-25")+R"(","capabilities":{"tools":{"listChanged":true}},"serverInfo":{"name":"fixture","version":"1"},"instructions":"ignore me"})";
            governance_detail::parse_json(response,r);
        } else if(method=="tools/list") {
            assert(initialized && context.protocol_version=="2025-11-25");++listed;
            if(bad_catalog)governance_detail::parse_json(R"({"tools":[],"nextCursor":"cycle"})",r);
            else if(!q["params"].HasMember("cursor"))governance_detail::parse_json(R"({"tools":[{"name":"sum","description":"Add numbers","inputSchema":{"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}}},"outputSchema":{"type":"object","properties":{"sum":{"type":"number"}}},"annotations":{"readOnlyHint":true}}],"nextCursor":"page2"})",r);
            else governance_detail::parse_json(R"({"tools":[{"name":"async","inputSchema":{"type":"object"},"execution":{"taskSupport":"required"}}]})",r);
        } else {
            assert(method=="tools/call" && initialized);++called;
            assert(context.protocol_version=="2025-11-25");
            if(q["params"].HasMember("_meta"))key=text(q["params"]["_meta"],"io.coolder.libai/idempotency_key");
            if(cancel_call){context.control.cancellation->cancel();throw std::runtime_error("cancelled");}
            if(fail_effect)throw std::runtime_error("lost reply after effect");
            governance_detail::parse_json(error_result?R"({"content":[{"type":"text","text":"failed"}],"isError":true})":R"({"content":[{"type":"text","text":"five"},{"type":"image","data":"AA==","mimeType":"image/png"}],"structuredContent":{"sum":5},"_meta":{"private":"secret"}})",r);
        }
        json::Value v;v.CopyFrom(r,d.GetAllocator());d.AddMember("result",v,d.GetAllocator());return mcp_detail::wire(d);
    }
    void notify(const std::string &wire,const mcp_request_context_t &context) override {
        json::Document d;governance_detail::parse_json(wire,d);assert(!d.HasMember("id"));
        if(text(d,"method")=="notifications/initialized"){++initialized;assert(context.protocol_version=="2025-11-25");}
        else {assert(text(d,"method")=="notifications/cancelled");++cancelled;assert(d["params"]["requestId"].GetUint64()==context.request_id);}
    }
    void close() noexcept override {++closed;}
};
template<class F>void rejects(F f){bool error=false;try{f();}catch(const std::exception&){error=true;}assert(error);}
std::shared_ptr<mcp_client_t> connect(std::shared_ptr<Transport> t){auto c=std::make_shared<mcp_client_t>(t);c->initialize("test","1");assert(c->list_tools().size()==2);return c;}
struct Store:execution_store_t {
    execution_snapshot_t snapshot;bool leased=false;unsigned revision=0;
    execution_snapshot_t open(const std::string &) override {assert(!leased);leased=true;return snapshot;}
    std::string commit(const std::string &,const std::string &expected,const std::string &wire) override {assert(leased && expected==snapshot.revision);snapshot.state_json=wire;snapshot.revision=std::to_string(++revision);return snapshot.revision;}
    void close(const std::string &) noexcept override {leased=false;}
};
mcp_binding_t policy(){mcp_binding_t b;b.parameters_schema=R"({"type":"object","properties":{"a":{"type":"number"},"b":{"type":"number"}},"required":["a","b"],"additionalProperties":false})";b.output_schema=R"({"type":"object","properties":{"sum":{"type":"number"}},"required":["sum"]})";b.execution_revision="trusted-server-v1";return b;}
std::shared_ptr<model_t> model(int &calls,bool invalid=false){return std::make_shared<function_model_t>([&calls,invalid](const model_request_t &q,const stream_sink_t &){model_response_t r;if(++calls==1){assert(q.tools[0].requires_approval && !q.tools[0].side_effect_free);tool_call_t c;c.id="call";c.name="mcp_sum";c.arguments=invalid?R"({"a":"bad","b":3})":R"({"a":2,"b":3})";r.calls.push_back(c);}else r.text="done";return r;});}
int main(){
    auto t=std::make_shared<Transport>();auto client=connect(t);
    assert(t->initialized==1 && t->listed==2 && client->tool("sum").input_schema.find("2020-12")!=std::string::npos);
    rejects([&]{client->initialize("test","1");});rejects([&]{client->call_tool("missing","{}");});
    rejects([&]{client->call_tool("sum","[]");});rejects([&]{client->call_tool("async","{}");});assert(t->called==0);
    int calls=0;agent_t denied(model(calls));add_mcp_tool(denied,client,"sum","mcp_sum",policy());agent_session_t session;
    assert(denied.run(session,"sum").status==run_status_t::completed && t->called==0);
    calls=0;agent_t bad(model(calls,true));add_mcp_tool(bad,client,"sum","mcp_sum",policy());agent_session_t bad_session;int authorized=0;
    run_options_t options;options.authorize=[&](const tool_spec_t &,const tool_call_t &){++authorized;return true;};
    assert(bad.run(bad_session,"sum",options).status==run_status_t::completed && authorized==0 && t->called==0);
    calls=0;agent_t agent(model(calls));add_mcp_tool(agent,client,"sum","mcp_sum",policy());agent_session_t successful;
    auto store=std::make_shared<Store>();options.execution_store=store;options.execution_scope="mcp-task";
    assert(agent.run(successful,"sum",options).status==run_status_t::completed && t->called==1 && t->key=="mcp-task.0");
    const auto tool_text=successful.messages[2].text;assert(tool_text.find("image/png")!=std::string::npos && tool_text.find("secret")==std::string::npos);
    assert(agent.run(successful,"sum",options).status==run_status_t::completed && t->called==1 && calls==2);
    // Discovery failure invalidates old catalog; no stale transport call follows.
    t->bad_catalog=true;rejects([&]{client->list_tools();});rejects([&]{client->call_tool("sum","{}");});assert(t->called==1);
    auto version=std::make_shared<Transport>();version->wrong_version=true;mcp_client_t incompatible(version);rejects([&]{incompatible.initialize("test","1");});assert(version->initialized==0 && version->closed);
    auto mismatch=std::make_shared<Transport>();mismatch->wrong_id=true;mcp_client_t invalid(mismatch);rejects([&]{invalid.initialize("test","1");});assert(mismatch->closed);
    auto effect=std::make_shared<Transport>();auto remote=connect(effect);effect->fail_effect=true;int retry_calls=0;
    agent_t uncertain(model(retry_calls));add_mcp_tool(uncertain,remote,"sum","mcp_sum",policy());agent_session_t mutation;
    auto ledger=std::make_shared<Store>();options.execution_store=ledger;options.execution_scope="lost";
    assert(uncertain.run(mutation,"sum",options).status==run_status_t::recovery_required && effect->called==1);
    assert(uncertain.run(mutation,"sum",options).status==run_status_t::recovery_required && effect->called==1 && retry_calls==1);
    // Cooperative cancellation emits only a matching issued request cancellation.
    auto cancelled=std::make_shared<Transport>();auto cancelled_client=connect(cancelled);cancelled->cancel_call=true;
    mcp_options_t control;control.cancellation=std::make_shared<cancellation_t>();rejects([&]{cancelled_client->call_tool("sum","{}",control);});assert(cancelled->cancelled==1);
    auto pre=std::make_shared<Transport>();mcp_client_t presend(pre);control.cancellation->cancel();rejects([&]{presend.initialize("test","1",control);});assert(pre->initialized==0 && pre->cancelled==0);
    auto er=std::make_shared<Transport>();auto erclient=connect(er);er->error_result=true;assert(erclient->call_tool("sum","{}").error);
}
