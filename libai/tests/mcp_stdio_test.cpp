#include <libai/mcp_stdio.h>
#include <cassert>
#include <iostream>
#include <cstdlib>
using namespace libai;
using namespace mcp_detail;
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}assert(caught);}
int descriptors(){int count=0;for(int fd=0;fd<512;++fd)if(fcntl(fd,F_GETFD)>=0)++count;return count;}
void emit(const std::string &wire){std::cout<<wire<<'\n'<<std::flush;}
void result(const json::Document &request,const std::string &value,bool wrong=false){
    json::Document r;governance_detail::parse_json(value,r);json::Document d;d.SetObject();string(d,"jsonrpc","2.0");
    d.AddMember("id",request["id"].GetUint64()+(wrong?1:0),d.GetAllocator());
    json::Value copy;copy.CopyFrom(r,d.GetAllocator());d.AddMember("result",copy,d.GetAllocator());emit(wire(d));
}
int server(const std::string &mode,const std::string &literal){
    if(mode=="blocked"){std::this_thread::sleep_for(std::chrono::seconds(5));return 0;}
    bool initialized=false;std::string input;
    while(std::getline(std::cin,input)){
        json::Document q;governance_detail::parse_json(input,q);auto method=text(q,"method");
        if(method=="notifications/initialized"){initialized=true;continue;}
        if(method=="notifications/cancelled")continue;
        if(mode=="exit")return 0;
        if(mode=="timeout"){std::this_thread::sleep_for(std::chrono::seconds(5));return 0;}
        if(mode=="malformed"){emit("not JSON");return 0;}
        if(mode=="oversize"){std::cout<<std::string(2*1024*1024+1,'x')<<std::flush;return 0;}
        if(mode=="flood"){for(int i=0;i<257;++i)emit(R"({"jsonrpc":"2.0","method":"notifications/message","params":{}})");return 0;}
        if(method=="initialize"){
            assert(!initialized);
            // stderr is not protocol output; even a full pipe's worth must not deadlock.
            std::cerr<<std::string(256*1024,'s')<<std::flush;
            emit(R"({"jsonrpc":"2.0","method":"ping","id":"server-ping"})");
            assert(std::getline(std::cin,input));json::Document ping;governance_detail::parse_json(input,ping);
            assert(text(ping,"id")=="server-ping" && ping.HasMember("result"));
            emit(R"({"jsonrpc":"2.0","method":"sampling/createMessage","id":100,"params":{}})");
            assert(std::getline(std::cin,input));json::Document unsupported;governance_detail::parse_json(input,unsupported);
            assert(unsupported["error"]["code"].GetInt()==-32601);
            result(q,R"({"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"stdio-fixture","version":"1"}})",mode=="wrong-id");
        }else if(method=="tools/list"){
            assert(initialized);result(q,R"({"tools":[{"name":"echo","inputSchema":{"type":"object"}}]})");
        }else{
            assert(initialized && method=="tools/call");
            json::Document r; r.SetObject();json::Value content(json::kArrayType),block(json::kObjectType);
            block.AddMember("type","text",r.GetAllocator());std::string value=literal;
            const char *env=std::getenv("LIBAI_MCP_FIXTURE");if(env)value+=env;
            json::Value t;t.SetString(value.c_str(),static_cast<json::SizeType>(value.size()),r.GetAllocator());
            block.AddMember("text",t,r.GetAllocator());content.PushBack(block,r.GetAllocator());r.AddMember("content",content,r.GetAllocator());
            r.AddMember("pid",static_cast<int>(getpid()),r.GetAllocator());result(q,wire(r));
        }
    }return 0;
}
std::shared_ptr<mcp_stdio_transport_t> transport(const std::string &self,const std::string &mode="normal"){
    mcp_stdio_options_t options;options.executable=self;options.arguments={"--server",mode,"literal;$()` space"};
    options.inherit_environment=false;options.environment={"LIBAI_MCP_FIXTURE=env-value"};
    return std::make_shared<mcp_stdio_transport_t>(options);
}
struct Model:model_t {
    int calls=0;
    model_response_t complete(const model_request_t &,const stream_sink_t &) override{
        model_response_t r;if(++calls==1){tool_call_t c;c.id="echo1";c.name="local_echo";c.arguments="{}";r.calls.push_back(c);}else r.text="complete";return r;
    }
};
int main(int argc,char **argv){
    if(argc>1 && std::string(argv[1])=="--server")return server(argv[2],argv[3]);
    char path[4096];assert(realpath(argv[0],path));std::string self=path;
    int initial_descriptors=descriptors();
    mcp_stdio_options_t invalid;invalid.executable="sh";rejects([&]{mcp_stdio_transport_t t(invalid);});
    invalid.executable="/this/executable/does/not/exist";
    for(int attempt=0;attempt<20;++attempt)rejects([&]{mcp_stdio_transport_t t(invalid);});
    assert(descriptors()==initial_descriptors);
    auto t=transport(self);auto client=std::make_shared<mcp_client_t>(t);client->initialize("test","1");assert(client->list_tools().size()==1);
    auto value=client->call_tool("echo","{}");assert(value.json.find("literal;$()` spaceenv-value")!=std::string::npos);
    auto model=std::make_shared<Model>();agent_t agent(model);mcp_binding_t binding;
    binding.parameters_schema=R"({"type":"object","additionalProperties":false})";binding.requires_approval=false;binding.execution_revision="fixture-1";
    add_mcp_tool(agent,client,"echo","local_echo",binding);agent_session_t session;
    assert(agent.run(session,"echo").status==run_status_t::completed && model->calls==2);
    client->close();client->close();t->close();
    for(auto mode:{"wrong-id","malformed","oversize","flood","exit","timeout"}){
        auto begin=std::chrono::steady_clock::now();mcp_client_t c(transport(self,mode));mcp_options_t options;options.timeout=std::chrono::milliseconds(150);
        rejects([&]{c.initialize("test","1",options);});assert(std::chrono::steady_clock::now()-begin<std::chrono::seconds(2));
    }
    // Large writes to a non-reading process remain cancellable/deadline bounded.
    auto blocked=transport(self,"blocked");mcp_request_context_t ctx;ctx.control.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(80);
    rejects([&]{blocked->notify("{\"payload\":\""+std::string(1024*1024,'a')+"\"}",ctx);});blocked->close();
    auto cancellation=std::make_shared<cancellation_t>();auto slow=transport(self,"timeout");mcp_client_t c(slow);mcp_options_t options;options.cancellation=cancellation;
    std::thread trigger([&]{std::this_thread::sleep_for(std::chrono::milliseconds(60));cancellation->cancel();});
    rejects([&]{c.initialize("test","1",options);});trigger.join();
    // Broken stdin must report an error, without killing the application via SIGPIPE.
    auto dead=transport(self,"exit");mcp_request_context_t probe;probe.control.deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    dead->notify(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",probe);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));rejects([&]{dead->notify(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",probe);});dead->close();
    int status=0;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    assert(descriptors()==initial_descriptors);
}
