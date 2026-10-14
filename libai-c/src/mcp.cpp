#include "bridge_internal.h"
#include <libai/mcp.h>
#include <libai/mcp_stdio.h>
#include <cstring>
#include <set>
struct libai_mcp_result {
    std::string response,error;
    bool set=false,failed=false;
};
struct libai_mcp {
    std::mutex mutex;
    std::shared_ptr<libai::mcp_client_t> client;
};
namespace {
namespace json=libai::mcp_detail::json;
json::Document object(const char *wire,std::initializer_list<const char*> allowed) {
    json::Document d;libai::governance_detail::parse_json(wire?bridge_text(wire):"{}",d);
    if(!d.IsObject())throw std::invalid_argument("MCP arguments must be an object");
    std::set<std::string> keys;for(auto key:allowed)keys.insert(key);
    for(auto i=d.MemberBegin();i!=d.MemberEnd();++i)
        if(!keys.count(std::string(i->name.GetString(),i->name.GetStringLength())))throw std::invalid_argument("MCP unknown argument");
    return d;
}
std::string text(const json::Value &d,const char *key){return libai::mcp_detail::text(d,key);}
bool boolean(const json::Value &d,const char *key,bool fallback){
    if(!d.HasMember(key))return fallback;if(!d[key].IsBool())throw std::invalid_argument("MCP boolean argument invalid");return d[key].GetBool();
}
std::chrono::milliseconds timeout(const json::Value &d) {
    if(!d.HasMember("timeout_ms"))return std::chrono::milliseconds(30000);
    if(!d["timeout_ms"].IsInt64() || d["timeout_ms"].GetInt64()<=0 || d["timeout_ms"].GetInt64()>86400000)
        throw std::invalid_argument("MCP timeout must be 1..86400000 milliseconds");
    return std::chrono::milliseconds(d["timeout_ms"].GetInt64());
}
std::unique_lock<std::mutex> lock(libai_mcp *handle) {
    if(!handle)throw std::invalid_argument("MCP handle is null");
    std::unique_lock<std::mutex> guard(handle->mutex,std::try_to_lock);
    if(!guard.owns_lock())throw std::logic_error("MCP handle is busy");return guard;
}
json::Document tool_json(const libai::mcp_tool_t &source) {
    json::Document d;d.SetObject();libai::mcp_detail::string(d,"name",source.name);
    libai::mcp_detail::string(d,"description",source.description);
    libai::mcp_detail::string(d,"input_schema",source.input_schema);
    libai::mcp_detail::string(d,"output_schema",source.output_schema);
    d.AddMember("requires_tasks",source.requires_tasks,d.GetAllocator());return d;
}
class CallbackTransport : public libai::mcp_transport_t {
    libai_mcp_transport_fn callback_;void *user_;libai_mcp_release_fn release_;
    bool active_=false,closed_=false;
    libai_mcp_result call(const char *operation,const std::string &wire,const libai::mcp_request_context_t &ctx) {
        if(closed_)throw std::runtime_error("MCP callback transport closed");
        libai_invocation invocation;invocation.deadline=ctx.control.deadline;
        invocation.cancellation=ctx.control.cancellation;invocation.idempotency_key=ctx.control.idempotency_key;
        json::Document context;context.SetObject();context.AddMember("request_id",ctx.request_id,context.GetAllocator());
        libai::mcp_detail::string(context,"protocol_version",ctx.protocol_version);
        auto serialized=libai::mcp_detail::wire(context);libai_mcp_result result;
        callback_(user_,operation,wire.c_str(),serialized.c_str(),&result,&invocation);
        if(result.failed || !result.error.empty())throw std::runtime_error(result.error.empty()?"MCP callback result failed":result.error);
        return result;
    }
public:
    CallbackTransport(libai_mcp_transport_fn callback,void *user,libai_mcp_release_fn release)
        :callback_(callback),user_(user),release_(release){}
    void activate() noexcept {active_=true;}
    ~CallbackTransport(){if(active_){close();if(release_)try{release_(user_);}catch(...){}}}
    std::string request(const std::string &wire,const libai::mcp_request_context_t &ctx) override {
        auto result=call("request",wire,ctx);if(!result.set)throw std::runtime_error("MCP callback did not set a response");return result.response;
    }
    void notify(const std::string &wire,const libai::mcp_request_context_t &ctx) override {call("notify",wire,ctx);}
    void close() noexcept override {
        if(!active_ || closed_)return;closed_=true;
        try{libai_mcp_result result;callback_(user_,"close","","{}",&result,nullptr);}catch(...){}
    }
};
std::vector<std::string> strings(const json::Value &d,const char *key) {
    std::vector<std::string> result;if(!d.HasMember(key))return result;
    if(!d[key].IsArray() || d[key].Size()>256)throw std::invalid_argument("MCP array argument invalid");
    for(const auto &v:d[key].GetArray()){
        if(!v.IsString())throw std::invalid_argument("MCP string array item invalid");
        result.emplace_back(v.GetString(),v.GetStringLength());
    }return result;
}
}
extern "C" {
libai_mcp *libai_mcp_create(libai_mcp_transport_fn callback,void *user,libai_mcp_release_fn release) {
    try {
        if(!callback)throw std::invalid_argument("MCP transport callback is null");
        auto transport=std::make_shared<CallbackTransport>(callback,user,release);
        std::unique_ptr<libai_mcp> handle(new libai_mcp);
        handle->client=std::make_shared<libai::mcp_client_t>(transport);
        transport->activate();return handle.release();
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP creation failed");}return nullptr;
}
int libai_mcp_result_response(libai_mcp_result *result,const char *wire,size_t bytes) {
    if(!result)return -1;
    try {
        if(result->set || !wire || bytes>2*1024*1024)throw std::invalid_argument("MCP response missing, duplicate or too large");
        result->response.assign(wire,bytes);result->set=true;return 0;
    }catch(...){result->failed=true;return -1;}
}
void libai_mcp_result_error(libai_mcp_result *result,const char *error) {
    if(!result)return;
    try {if(!error || std::strlen(error)>4096){result->failed=true;return;}result->error=error;if(result->error.empty())result->failed=true;}
    catch(...){result->failed=true;}
}
int libai_mcp_stdio_available(void) {
#if (defined(__unix__) || defined(__APPLE__)) && (!defined(__APPLE__) || !TARGET_OS_IPHONE) && (!defined(__ANDROID__) || __ANDROID_API__ >= 28)
    return 1;
#else
    return 0;
#endif
}
libai_mcp *libai_mcp_create_stdio(const char *options_json) {
    try {
#if (defined(__unix__) || defined(__APPLE__)) && (!defined(__APPLE__) || !TARGET_OS_IPHONE) && (!defined(__ANDROID__) || __ANDROID_API__ >= 28)
        auto d=object(options_json,{"executable","arguments","inherit_environment","environment"});
        libai::mcp_stdio_options_t options;options.executable=text(d,"executable");
        options.arguments=strings(d,"arguments");options.environment=strings(d,"environment");
        options.inherit_environment=boolean(d,"inherit_environment",true);
        std::unique_ptr<libai_mcp> handle(new libai_mcp);
        handle->client=std::make_shared<libai::mcp_client_t>(std::make_shared<libai::mcp_stdio_transport_t>(options));return handle.release();
#else
        (void)options_json;throw std::runtime_error("MCP stdio is unavailable on this platform");
#endif
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP stdio creation failed");}return nullptr;
}
char *libai_mcp_call(libai_mcp *handle,const char *operation,const char *arguments,const libai_cancellation *cancellation) {
    try {
        auto guard=lock(handle);std::string op=bridge_text(operation);
        json::Document d;
        if(op=="initialize")d=object(arguments,{"name","version","timeout_ms"});
        else if(op=="list_tools")d=object(arguments,{"timeout_ms"});
        else if(op=="tool")d=object(arguments,{"name"});
        else if(op=="call_tool")d=object(arguments,{"name","arguments_json","timeout_ms","idempotency_key"});
        else throw std::invalid_argument("MCP operation unknown");
        libai::mcp_options_t options;options.timeout=timeout(d);if(cancellation)options.cancellation=cancellation->value;
        if(d.HasMember("idempotency_key"))options.idempotency_key=text(d,"idempotency_key");
        if(op=="initialize")return bridge_owned(handle->client->initialize(text(d,"name"),text(d,"version"),options));
        if(op=="tool")return bridge_owned(libai::mcp_detail::wire(tool_json(handle->client->tool(text(d,"name")))));
        json::Document result;
        if(op=="list_tools"){
            result.SetArray();for(const auto &t:handle->client->list_tools(options)){
                auto value=tool_json(t);json::Value copy;copy.CopyFrom(value,result.GetAllocator());result.PushBack(copy,result.GetAllocator());
            }
        }else{
            auto value=handle->client->call_tool(text(d,"name"),text(d,"arguments_json"),options);
            result.SetObject();libai::mcp_detail::string(result,"json",value.json);result.AddMember("error",value.error,result.GetAllocator());
        }return bridge_owned(libai::mcp_detail::wire(result));
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP call failed");}return nullptr;
}
int libai_mcp_close(libai_mcp *handle) {
    try {auto guard=lock(handle);handle->client->close();return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP close failed");}return -1;
}
int libai_mcp_destroy(libai_mcp *handle) {
    if(!handle)return 0;
    try {auto guard=lock(handle);guard.unlock();delete handle;return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP destruction failed");}return -1;
}
int libai_agent_add_mcp_tool(libai_agent *agent,libai_mcp *handle,const char *remote,const char *local,const char *policy_json) {
    try {
        if(!agent)throw std::invalid_argument("agent is null");
        std::unique_lock<std::mutex> agent_guard(agent->mutex,std::try_to_lock);
        if(!agent_guard.owns_lock())throw std::logic_error("agent is running");auto guard=lock(handle);
        auto d=object(policy_json,{"parameters_schema","output_schema","execution_revision","requires_approval","side_effect_free","timeout_ms"});
        libai::mcp_binding_t policy;policy.parameters_schema=text(d,"parameters_schema");policy.execution_revision=text(d,"execution_revision");
        if(d.HasMember("output_schema"))policy.output_schema=text(d,"output_schema");
        policy.requires_approval=boolean(d,"requires_approval",true);policy.side_effect_free=boolean(d,"side_effect_free",false);policy.timeout=timeout(d);
        libai::add_mcp_tool(*agent->runtime,handle->client,bridge_text(remote),bridge_text(local),policy);return 0;
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("MCP binding failed");}return -1;
}
}
