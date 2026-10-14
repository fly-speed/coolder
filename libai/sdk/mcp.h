#pragma once
#include "agent_types.h"
#include "vendor/rapidjson/writer.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <cstdint>
namespace libai {
struct mcp_options_t {
    std::shared_ptr<cancellation_t> cancellation;
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::time_point::max();
    std::chrono::milliseconds timeout{30000};
    std::string idempotency_key;
};
struct mcp_request_context_t {
    tool_context_t control;
    std::uint64_t request_id=0;
    std::string protocol_version;
};
// Host implements message framing, response routing, authentication and connection lifetime.
// HTTP adapters must send protocol_version in MCP-Protocol-Version after negotiation.
class mcp_transport_t {
public:
    virtual ~mcp_transport_t(){}
    virtual std::string request(const std::string &,const mcp_request_context_t &)=0;
    virtual void notify(const std::string &,const mcp_request_context_t &)=0;
    virtual void close() noexcept=0;
};
struct mcp_tool_t {
    std::string name,description,input_schema,output_schema;
    bool requires_tasks=false;
};
struct mcp_result_t {std::string json;bool error=false;};
namespace mcp_detail {
namespace json=governance_detail::json;
inline std::string wire(const json::Value &value) {
    json::StringBuffer b;json::Writer<json::StringBuffer> w(b);value.Accept(w);return std::string(b.GetString(),b.GetSize());
}
inline std::string text(const json::Value &v,const char *key) {
    if(!v.IsObject() || !v.HasMember(key) || !v[key].IsString())throw std::invalid_argument("MCP string field missing or invalid");
    return std::string(v[key].GetString(),v[key].GetStringLength());
}
inline void string(json::Document &d,const char *key,const std::string &s) {
    json::Value v;v.SetString(s.data(),static_cast<json::SizeType>(s.size()),d.GetAllocator());
    d.AddMember(json::StringRef(key),v,d.GetAllocator());
}
inline bool identifier(const std::string &s) {
    if(s.empty() || s.size()>128)return false;
    for(unsigned char c:s)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
    return true;
}
class guard {
    std::atomic<bool> &busy_;
public:
    explicit guard(std::atomic<bool> &b):busy_(b){bool expected=false;if(!b.compare_exchange_strong(expected,true))throw std::logic_error("MCP client busy");}
    ~guard(){busy_.store(false);}
    guard(const guard&)=delete;guard &operator=(const guard&)=delete;
};
}
// Tools-only MCP 2025-11-25 client. No automatic reconnect/retry or server-request capabilities.
class mcp_client_t {
    using json=mcp_detail::json::Document;
    std::shared_ptr<mcp_transport_t> transport_;
    std::atomic<bool> busy_{false};
    enum class state {fresh,ready,failed,closed};state state_=state::fresh;
    std::uint64_t id_=0;
    bool catalog_valid_=false;
    std::map<std::string,mcp_tool_t> catalog_;
    std::string server_info_;
    static const char *version(){return "2025-11-25";}
    mcp_request_context_t context(const mcp_options_t &o) const {
        if(o.timeout.count()<=0 || o.timeout>std::chrono::hours(24))throw std::invalid_argument("MCP timeout out of range");
        mcp_request_context_t c;c.control.cancellation=o.cancellation;c.control.idempotency_key=o.idempotency_key;
        c.control.deadline=std::min(o.deadline,std::chrono::steady_clock::now()+o.timeout);
        c.protocol_version=state_==state::ready?version():"";
        if(c.control.stopped())throw std::runtime_error("MCP request cancelled or deadline exceeded before send");
        return c;
    }
    void ready() const {if(state_!=state::ready)throw std::logic_error("MCP client not initialized");}
    [[noreturn]] void protocol_error(const char *message) {
        state_=state::failed;catalog_valid_=false;transport_->close();throw std::runtime_error(message);
    }
    void cancel(const mcp_request_context_t &c) noexcept {
        try {
            json params;params.SetObject();params.AddMember("requestId",c.request_id,params.GetAllocator());
            mcp_detail::string(params,"reason","libai request deadline/cancellation");
            json n;n.SetObject();mcp_detail::string(n,"jsonrpc","2.0");mcp_detail::string(n,"method","notifications/cancelled");
            n.AddMember("params",params,n.GetAllocator());
            auto control=c;control.control.cancellation.reset();control.control.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
            transport_->notify(mcp_detail::wire(n),control);
        }catch(...){}
    }
    json rpc(const char *method,const std::string &parameters,const mcp_options_t &o) {
        auto c=context(o);
        if(id_>=9007199254740991ULL)throw std::runtime_error("MCP request IDs exhausted");
        c.request_id=++id_;
        json params;governance_detail::parse_json(parameters,params);
        if(!params.IsObject())throw std::invalid_argument("MCP params must be object");
        json request;request.SetObject();mcp_detail::string(request,"jsonrpc","2.0");request.AddMember("id",c.request_id,request.GetAllocator());
        mcp_detail::string(request,"method",method);mcp_detail::json::Value copy;copy.CopyFrom(params,request.GetAllocator());request.AddMember("params",copy,request.GetAllocator());
        std::string response;
        try {response=transport_->request(mcp_detail::wire(request),c);}
        catch(...) {if(c.control.stopped() && std::string(method)!="initialize")cancel(c);state_=state::failed;catalog_valid_=false;transport_->close();throw;}
        if(c.control.stopped())protocol_error("MCP late response rejected");
        json envelope;
        try {governance_detail::parse_json(response,envelope);}catch(...){protocol_error("MCP response JSON invalid or exceeds limits");}
        if(!envelope.IsObject() || !envelope.HasMember("jsonrpc") || !envelope["jsonrpc"].IsString() ||
            mcp_detail::text(envelope,"jsonrpc")!="2.0" || !envelope.HasMember("id") || !envelope["id"].IsUint64() ||
            envelope["id"].GetUint64()!=c.request_id || envelope.HasMember("result")==envelope.HasMember("error"))
            protocol_error("MCP response envelope or ID mismatch");
        if(envelope.HasMember("error")) {
            const auto &e=envelope["error"];
            if(!e.IsObject() || !e.HasMember("code") || !e["code"].IsInt64() || !e.HasMember("message") || !e["message"].IsString())
                protocol_error("MCP RPC error malformed");
            throw std::runtime_error("MCP RPC error code="+std::to_string(e["code"].GetInt64()));
        }
        if(!envelope["result"].IsObject())protocol_error("MCP result must be object");
        json result;result.CopyFrom(envelope["result"],result.GetAllocator());return result;
    }
public:
    explicit mcp_client_t(std::shared_ptr<mcp_transport_t> transport):transport_(std::move(transport)) {
        if(!transport_)throw std::invalid_argument("MCP transport is null");
    }
    ~mcp_client_t(){transport_->close();}
    mcp_client_t(const mcp_client_t&)=delete;mcp_client_t &operator=(const mcp_client_t&)=delete;
    void close(){mcp_detail::guard lock(busy_);state_=state::closed;catalog_valid_=false;transport_->close();}
    std::string initialize(const std::string &name,const std::string &client_version,const mcp_options_t &o=mcp_options_t()) {
        mcp_detail::guard lock(busy_);
        if(state_!=state::fresh)throw std::logic_error("MCP initialize may only occur once");
        if(name.empty() || name.size()>128 || client_version.empty() || client_version.size()>128)throw std::invalid_argument("MCP client identity invalid");
        json params;params.SetObject();mcp_detail::string(params,"protocolVersion",version());
        mcp_detail::json::Value caps(mcp_detail::json::kObjectType);params.AddMember("capabilities",caps,params.GetAllocator());
        json info;info.SetObject();mcp_detail::string(info,"name",name);mcp_detail::string(info,"version",client_version);
        mcp_detail::json::Value copied;copied.CopyFrom(info,params.GetAllocator());params.AddMember("clientInfo",copied,params.GetAllocator());
        try {
            auto result=rpc("initialize",mcp_detail::wire(params),o);
            if(mcp_detail::text(result,"protocolVersion")!=version() || !result.HasMember("capabilities") || !result["capabilities"].IsObject() ||
                !result["capabilities"].HasMember("tools") || !result["capabilities"]["tools"].IsObject() ||
                !result.HasMember("serverInfo") || !result["serverInfo"].IsObject())throw std::runtime_error("MCP version/tools capability missing");
            mcp_detail::text(result["serverInfo"],"name");mcp_detail::text(result["serverInfo"],"version");
            json n;n.SetObject();mcp_detail::string(n,"jsonrpc","2.0");mcp_detail::string(n,"method","notifications/initialized");
            auto c=context(o);c.protocol_version=version();transport_->notify(mcp_detail::wire(n),c);
            if(c.control.stopped())throw std::runtime_error("MCP initialized notification expired");
            server_info_=mcp_detail::wire(result["serverInfo"]);state_=state::ready;return server_info_;
        } catch(...) {state_=state::failed;transport_->close();throw;}
    }
    std::vector<mcp_tool_t> list_tools(const mcp_options_t &o=mcp_options_t()) {
        mcp_detail::guard lock(busy_);ready();catalog_valid_=false;
        std::map<std::string,mcp_tool_t> collected;std::set<std::string> cursors;std::string cursor;
        auto bounded=o;bounded.deadline=context(o).control.deadline;
        for(unsigned page=0;page<16;++page) {
            json params;params.SetObject();if(!cursor.empty())mcp_detail::string(params,"cursor",cursor);
            auto result=rpc("tools/list",mcp_detail::wire(params),bounded);
            if(!result.HasMember("tools") || !result["tools"].IsArray())throw std::runtime_error("MCP tools list malformed");
            for(const auto &value:result["tools"].GetArray()) {
                mcp_tool_t tool;tool.name=mcp_detail::text(value,"name");
                if(!mcp_detail::identifier(tool.name) || collected.count(tool.name) || collected.size()>=256)throw std::runtime_error("MCP tool name duplicate/invalid or catalog limit");
                if(value.HasMember("description"))tool.description=mcp_detail::text(value,"description");
                if(!value.HasMember("inputSchema") || !value["inputSchema"].IsObject())throw std::runtime_error("MCP inputSchema missing");
                tool.input_schema=mcp_detail::wire(value["inputSchema"]);
                if(value.HasMember("outputSchema")) {if(!value["outputSchema"].IsObject())throw std::runtime_error("MCP outputSchema malformed");tool.output_schema=mcp_detail::wire(value["outputSchema"]);}
                if(value.HasMember("execution")) {
                    const auto &execution=value["execution"];if(!execution.IsObject())throw std::runtime_error("MCP execution malformed");
                    if(execution.HasMember("taskSupport")) {auto support=mcp_detail::text(execution,"taskSupport");
                        if(support!="forbidden" && support!="optional" && support!="required")throw std::runtime_error("MCP taskSupport invalid");tool.requires_tasks=support=="required";}
                }
                collected.emplace(tool.name,std::move(tool));
            }
            if(!result.HasMember("nextCursor")) {
                catalog_.swap(collected);catalog_valid_=true;std::vector<mcp_tool_t> tools;
                for(const auto &entry:catalog_)tools.push_back(entry.second);return tools;
            }
            cursor=mcp_detail::text(result,"nextCursor");if(cursor.empty() || !cursors.insert(cursor).second)throw std::runtime_error("MCP cursor invalid or cyclic");
        }
        throw std::runtime_error("MCP pagination limit exceeded");
    }
    mcp_tool_t tool(const std::string &name) {
        mcp_detail::guard lock(busy_);ready();if(!catalog_valid_ || !catalog_.count(name))throw std::logic_error("MCP tool catalog not available");return catalog_.at(name);
    }
    mcp_result_t call_tool(const std::string &name,const std::string &arguments,const mcp_options_t &o=mcp_options_t(),const mcp_tool_t *expected=nullptr) {
        mcp_detail::guard lock(busy_);ready();
        if(!catalog_valid_ || !catalog_.count(name))throw std::logic_error("MCP tool not discovered");
        if(expected && (expected->name!=name || expected->input_schema!=catalog_.at(name).input_schema ||
            expected->output_schema!=catalog_.at(name).output_schema || expected->requires_tasks!=catalog_.at(name).requires_tasks))
            throw std::runtime_error("MCP discovered tool definition changed; host must rebind");
        if(catalog_.at(name).requires_tasks)throw std::logic_error("MCP asynchronous tasks not supported");
        json params;governance_detail::parse_json(arguments,params);if(!params.IsObject())throw std::invalid_argument("MCP arguments must be object");
        json body;body.SetObject();mcp_detail::string(body,"name",name);mcp_detail::json::Value args;args.CopyFrom(params,body.GetAllocator());body.AddMember("arguments",args,body.GetAllocator());
        if(!o.idempotency_key.empty()) {
            if(o.idempotency_key.size()>256)throw std::invalid_argument("MCP idempotency key too long");
            json meta;meta.SetObject();mcp_detail::string(meta,"io.coolder.libai/idempotency_key",o.idempotency_key);
            mcp_detail::json::Value m;m.CopyFrom(meta,body.GetAllocator());body.AddMember("_meta",m,body.GetAllocator());
        }
        auto result=rpc("tools/call",mcp_detail::wire(body),o);
        if(!result.HasMember("content") || !result["content"].IsArray())protocol_error("MCP content must be array");
        for(const auto &block:result["content"].GetArray()) {
            const auto type=mcp_detail::text(block,"type");
            if(type=="text")mcp_detail::text(block,"text");
            else if(type=="image" || type=="audio"){mcp_detail::text(block,"data");mcp_detail::text(block,"mimeType");}
            else if(type=="resource_link")mcp_detail::text(block,"uri");
            else if(type=="resource"){if(!block.HasMember("resource") || !block["resource"].IsObject())protocol_error("MCP resource malformed");mcp_detail::text(block["resource"],"uri");}
            else protocol_error("MCP content type unsupported");
        }
        if(result.HasMember("isError") && !result["isError"].IsBool())protocol_error("MCP isError invalid");
        if(result.HasMember("structuredContent") && !result["structuredContent"].IsObject())protocol_error("MCP structuredContent must be object");
        mcp_result_t output;output.error=result.HasMember("isError") && result["isError"].GetBool();
        // Keep model content, not private protocol metadata or arbitrary envelope extensions.
        for(auto &block:result["content"].GetArray()) {
            if(block.HasMember("_meta"))block.RemoveMember("_meta");
            if(block.HasMember("resource") && block["resource"].IsObject() && block["resource"].HasMember("_meta"))
                block["resource"].RemoveMember("_meta");
        }
        json model;model.SetObject();
        for(const char *field:{"content","structuredContent","isError"})if(result.HasMember(field)) {
            mcp_detail::json::Value v;v.CopyFrom(result[field],model.GetAllocator());model.AddMember(mcp_detail::json::StringRef(field),v,model.GetAllocator());
        }
        output.json=mcp_detail::wire(model);return output;
    }
};
}
