#include <libai_cpp/mcp.h>
#include <libai_cpp/agent.h>
#include <cassert>
#include <stdexcept>
using namespace libai::cpp;
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed);}
struct Transport:McpTransport {
    int initialized=0,called=0,closed=0,cancelled=0;
    bool lose_response=false,stop=false,changed=false;
    std::string key;
    std::string request(const std::string &wire,const McpRequestContext &ctx) override {
        std::string result;
        if(wire.find("\"method\":\"initialize\"")!=std::string::npos){
            assert(ctx.protocol_version.empty());
            result=R"({"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"fixture","version":"1"}})";
        }else if(wire.find("\"method\":\"tools/list\"")!=std::string::npos){
            assert(initialized && ctx.protocol_version=="2025-11-25");
            result=changed?R"({"tools":[{"name":"echo","inputSchema":{"type":"object","description":"changed"}}]})":R"({"tools":[{"name":"echo","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]})";
        }else{
            assert(initialized && ctx.protocol_version=="2025-11-25");++called;key=ctx.control.idempotency_key;
            if(stop){assert(ctx.control.cancellation);ctx.control.cancellation->cancel();throw std::runtime_error("cancelled");}
            if(lose_response)throw std::runtime_error("effect happened, response lost");
            result=R"({"content":[{"type":"text","text":"echo"}],"_meta":{"private":"secret"}})";
        }
        return "{\"jsonrpc\":\"2.0\",\"id\":"+std::to_string(ctx.request_id)+",\"result\":"+result+"}";
    }
    void notify(const std::string &wire,const McpRequestContext &ctx) override {
        if(wire.find("notifications/initialized")!=std::string::npos){assert(ctx.protocol_version=="2025-11-25");++initialized;}
        else {assert(wire.find("notifications/cancelled")!=std::string::npos);++cancelled;}
    }
    void close() noexcept override {++closed;}
};
struct Store:ExecutionStore {
    ExecutionSnapshot state;bool leased=false;int revision=0;
    ExecutionSnapshot open(const std::string &) override {assert(!leased);leased=true;return state;}
    std::string commit(const std::string &,const std::string &expected,const std::string &next) override {
        assert(leased && expected==state.revision);state.state_json=next;state.revision=std::to_string(++revision);return state.revision;
    }
    void close(const std::string &) noexcept override {leased=false;}
};
std::shared_ptr<McpClient> connect(std::shared_ptr<Transport> transport){
    auto c=std::make_shared<McpClient>(transport);c->initialize("test","1");
    assert(c->list_tools().size()==1 && c->tool("echo").input_schema.find("object")!=std::string::npos);return c;
}
McpBinding policy(){McpBinding b;b.parameters_schema=R"({"type":"object","additionalProperties":false})";b.execution_revision="host-fixture-1";return b;}
ModelFunction model(int &count,bool invalid=false){return [&count,invalid](const ModelRequest &q,const StreamSink &){
    ModelResponse r;if(++count==1){assert(q.tools.size()==1 && q.tools[0].requires_approval && !q.tools[0].side_effect_free);
        ToolCall c;c.name="local_echo";c.id="call1";c.arguments=invalid?R"({"bad":true})":"{}";r.calls.push_back(c);
    }else r.text="done";return r;
};}
int main(int argc,char **argv){
    rejects([]{McpClient c(nullptr);});auto t=std::make_shared<Transport>();auto c=connect(t);int calls=0;
    auto denied=policy();Agent agent(model(calls));agent.add_mcp_tool(c,"echo","local_echo",denied);
    assert(agent.run("echo").status==RunStatus::completed && t->called==0);
    calls=0;int authorized=0;Agent bad(model(calls,true));bad.add_mcp_tool(c,"echo","local_echo",policy());
    RunOptions opts;opts.authorize=[&](const ToolSpec &,const ToolCall &){++authorized;return true;};
    assert(bad.run("echo",opts).status==RunStatus::completed && authorized==0 && t->called==0);
    calls=0;auto store=std::make_shared<Store>();opts.execution_store=store;opts.execution_scope="cpp-mcp";
    {Agent successful(model(calls));successful.add_mcp_tool(c,"echo","local_echo",policy());
        rejects([&]{successful.add_mcp_tool(nullptr,"echo","bad",policy());});
        assert(successful.run("echo",opts).status==RunStatus::completed && t->called==1 && t->key=="cpp-mcp.0");
        assert(successful.run("echo",opts).status==RunStatus::completed && t->called==1 && calls==2);
        for(const auto &message:successful.messages())assert(message.text.find("secret")==std::string::npos);
    }
    auto uncertain=std::make_shared<Transport>();uncertain->lose_response=true;auto remote=connect(uncertain);
    auto unknown=std::make_shared<Store>();RunOptions recovery;recovery.execution_store=unknown;recovery.execution_scope="cpp-unknown";
    recovery.authorize=[](const ToolSpec &,const ToolCall &){return true;};int attempts=0;
    {Agent interrupted(model(attempts));interrupted.add_mcp_tool(remote,"echo","local_echo",policy());
        assert(interrupted.run("write",recovery).status==RunStatus::recovery_required && uncertain->called==1);}
    auto next_transport=std::make_shared<Transport>();auto next_connection=connect(next_transport);
    {Agent resumed(model(attempts));resumed.add_mcp_tool(next_connection,"echo","local_echo",policy());
        assert(resumed.run("write",recovery).status==RunStatus::recovery_required && uncertain->called==1 && next_transport->called==0 && attempts==1);}
    auto cancelled=std::make_shared<Transport>();auto cancelled_client=connect(cancelled);cancelled->stop=true;
    McpOptions control;control.cancellation=std::make_shared<Cancellation>();
    rejects([&]{cancelled_client->call_tool("echo","{}",control);});assert(control.cancellation->cancelled() && cancelled->cancelled==1);
    auto pinned=std::make_shared<Transport>();auto pinned_client=connect(pinned);int plans=0;Agent definition(model(plans));
    definition.add_mcp_tool(pinned_client,"echo","local_echo",policy());pinned->changed=true;pinned_client->list_tools();
    RunOptions allowed;allowed.authorize=[](const ToolSpec &,const ToolCall &){return true;};
    assert(definition.run("echo",allowed).status==RunStatus::completed && pinned->called==0);
    auto owned=std::make_shared<Transport>();int turns=0;
    {auto owner=connect(owned);Agent holder(model(turns));holder.add_mcp_tool(owner,"echo","local_echo",policy());owner.reset();
        assert(owned->closed==0 && holder.run("echo",allowed).status==RunStatus::completed && owned->called==1);}
    assert(owned->closed>0);
    if(argc==3){assert(McpClient::stdio_available());McpStdioOptions options;options.executable=argv[1];options.arguments={argv[2]};
        auto process=McpClient::stdio(options);process->initialize("consumer","1");assert(process->list_tools().size()==1);
        assert(process->call_tool("echo","{}").json.find("from stdio")!=std::string::npos);
        int n=0;Agent local(model(n));local.add_mcp_tool(process,"echo","local_echo",policy());
        assert(local.run("echo",allowed).status==RunStatus::completed && n==2);process->close();process->close();
    }
}
