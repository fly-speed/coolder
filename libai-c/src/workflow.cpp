#include "bridge_internal.h"
#include <libai/workflow.h>
#include <cstring>
#include <set>

struct libai_workflow_result {
    libai::workflow_node_result_t value;
    bool set=false,failed=false;
};
struct libai_workflow {
    std::mutex mutex;
    libai::workflow_t runtime;
};
namespace {
namespace json=libai::workflow_detail::json;
constexpr std::size_t result_wire_limit=16*1024*1024;
json::Document object(const std::string &wire,std::initializer_list<const char*> allowed,
    std::size_t maximum=2*1024*1024) {
    auto d=libai::workflow_detail::object(wire,maximum);
    std::set<std::string> keys;for(auto key:allowed)keys.insert(key);
    for(auto i=d.MemberBegin();i!=d.MemberEnd();++i)
        if(!keys.count(std::string(i->name.GetString(),i->name.GetStringLength())))
            throw std::invalid_argument("workflow unknown field");
    return d;
}
std::string text(const json::Value &d,const char *key,const char *fallback=nullptr) {
    if(!d.HasMember(key)){if(fallback)return fallback;throw std::invalid_argument("workflow required string missing");}
    if(!d[key].IsString())throw std::invalid_argument("workflow string field invalid");
    std::string value(d[key].GetString(),d[key].GetStringLength());
    if(value.find('\0')!=std::string::npos)throw std::invalid_argument("workflow string contains NUL");
    return value;
}
bool boolean(const json::Value &d,const char *key) {
    if(!d.HasMember(key))return false;
    if(!d[key].IsBool())throw std::invalid_argument("workflow boolean field invalid");return d[key].GetBool();
}
std::size_t number(const json::Value &d,const char *key,std::size_t fallback,std::size_t maximum) {
    if(!d.HasMember(key))return fallback;
    if(!d[key].IsUint64()||d[key].GetUint64()>maximum)throw std::invalid_argument("workflow numeric limit invalid");
    return static_cast<std::size_t>(d[key].GetUint64());
}
void field(json::Value &d,const char *key,const std::string &value,json::Document::AllocatorType &a) {
    json::Value v;v.SetString(value.data(),static_cast<json::SizeType>(value.size()),a);
    d.AddMember(json::Value(key,a),v,a);
}
libai::workflow_node_result_t output(const std::string &wire) {
    auto d=object(wire,{"patch_json","route","status","error"},result_wire_limit);
    libai::workflow_node_result_t result;
    result.patch_json=text(d,"patch_json","{}");result.route=text(d,"route","");result.error=text(d,"error","");
    auto status=text(d,"status","completed");
    static const char *names[]={"completed","cancelled","deadline_exceeded","budget_exceeded","model_error","protocol_error","callback_error","recovery_required"};
    bool found=false;for(unsigned i=0;i<8;++i)if(status==names[i]){result.status=static_cast<libai::run_status_t>(i);found=true;break;}
    if(!found||result.route.size()>32||result.error.size()>4096)throw std::invalid_argument("workflow output fields invalid");
    libai::workflow_detail::object(result.patch_json,2*1024*1024);
    return result;
}
std::unique_lock<std::mutex> lock(libai_workflow *handle) {
    if(!handle)throw std::invalid_argument("workflow is null");
    std::unique_lock<std::mutex> guard(handle->mutex,std::try_to_lock);
    if(!guard.owns_lock())throw std::logic_error("workflow is busy");return guard;
}
struct NodeOwner {
    libai_workflow_node_fn callback;void *user;libai_workflow_release_fn release;bool active=false;
    NodeOwner(libai_workflow_node_fn c,void *u,libai_workflow_release_fn r):callback(c),user(u),release(r){}
    ~NodeOwner(){if(active&&release)try{release(user);}catch(...){}}
};
std::string result_json(const libai::workflow_result_t &source) {
    // Reuse status and TaskContract encoding; do not infer build/test evidence.
    libai::run_result_t summary;summary.status=source.status;summary.task_contract=source.task_contract;
    json::Document encoded;libai::governance_detail::parse_json(bridge_result_json(summary),encoded);
    json::Document d;d.SetObject();auto &a=d.GetAllocator();
    for(auto key:{"status","task_contract"}){
        json::Value value;value.CopyFrom(encoded[key],a);d.AddMember(json::Value(key,a),value,a);
    }
    field(d,"state_json",source.state_json,a);field(d,"error",source.error,a);field(d,"journal_revision",source.journal_revision,a);
    d.AddMember("progress_available",source.progress_available,a);
    d.AddMember("node_calls",static_cast<uint64_t>(source.node_calls),a);
    json::Value nodes(json::kObjectType);for(const auto &entry:source.nodes)field(nodes,entry.first.c_str(),entry.second,a);
    d.AddMember("nodes",nodes,a);return libai::workflow_detail::wire(d);
}
}
extern "C" {
libai_workflow *libai_workflow_create(void) {
    try{return new libai_workflow;}catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow creation failed");}return nullptr;
}
int libai_workflow_add_node(libai_workflow *handle,const char *descriptor,libai_workflow_node_fn callback,void *user,libai_workflow_release_fn release) {
    try {
        auto guard=lock(handle);if(!callback)throw std::invalid_argument("workflow callback is null");
        auto d=object(bridge_text(descriptor),{"name","writes","execution_revision","parallel_safe","side_effect_free"});
        libai::workflow_node_t node;node.name=text(d,"name");node.execution_revision=text(d,"execution_revision","1");
        node.parallel_safe=boolean(d,"parallel_safe");node.side_effect_free=boolean(d,"side_effect_free");
        if(d.HasMember("writes")){
            if(!d["writes"].IsArray()||d["writes"].Size()>64)throw std::invalid_argument("workflow writes invalid");
            for(const auto &v:d["writes"].GetArray()){
                if(!v.IsString())throw std::invalid_argument("workflow write must be a string");
                node.writes.emplace_back(v.GetString(),v.GetStringLength());
            }
        }
        auto owner=std::make_shared<NodeOwner>(callback,user,release);
        node.execute=[owner](const std::string &state,const libai::workflow_context_t &context){
            libai_invocation invocation;invocation.cancellation=context.control.cancellation;
            invocation.deadline=context.control.deadline;invocation.idempotency_key=context.control.idempotency_key;
            json::Document d;d.SetObject();field(d,"execution_scope",context.execution_scope,d.GetAllocator());
            auto wire=libai::workflow_detail::wire(d);libai_workflow_result result;
            owner->callback(owner->user,state.c_str(),wire.c_str(),&result,&invocation);
            if(result.failed||!result.set)throw std::runtime_error("workflow callback output missing or invalid");
            return result.value;
        };
        handle->runtime.add_node(node);owner->active=true;return 0;
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow registration failed");}return -1;
}
int libai_workflow_add_edge(libai_workflow *handle,const char *from,const char *to,const char *label) {
    try{auto guard=lock(handle);handle->runtime.add_edge(bridge_text(from),bridge_text(to),bridge_text(label));return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow edge failed");}return -1;
}
int libai_workflow_result_set(libai_workflow_result *result,const char *wire,size_t bytes) {
    if(!result)return -1;
    try{
        if(!wire||result->set||bytes>result_wire_limit)throw std::invalid_argument("workflow output missing, duplicate or too large");
        result->value=output(std::string(wire,bytes));result->set=true;return 0;
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow output failed");}
    result->failed=true;return -1;
}
char *libai_workflow_run(libai_workflow *handle,const char *initial,const char *options_json,
    libai_execution_fn store,void *store_user,const libai_cancellation *cancellation) {
    try {
        auto guard=lock(handle);auto d=object(options_json?bridge_text(options_json):"{}",{"scope","max_parallel","max_node_calls","max_state_bytes","timeout_ms"});
        libai::workflow_options_t options;options.scope=text(d,"scope","");
        options.max_parallel=number(d,"max_parallel",1,64);options.max_node_calls=number(d,"max_node_calls",64,64);
        options.max_state_bytes=number(d,"max_state_bytes",2*1024*1024,2*1024*1024);
        options.timeout=std::chrono::milliseconds(number(d,"timeout_ms",300000,86400000));
        if(cancellation)options.cancellation=cancellation->value;
        if(store)options.execution_store=bridge_execution_store(store,store_user);
        return bridge_owned(result_json(handle->runtime.run(bridge_text(initial),options)));
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow run failed");}return nullptr;
}
int libai_workflow_confirm_node(libai_workflow *handle,libai_execution_fn store,void *user,
    const char *scope,const char *initial,const char *revision,const char *node,const char *outcome,
    const char *output_json,const char *reference) {
    try {
        auto guard=lock(handle);auto result=output(output_json?bridge_text(output_json):"{}");
        handle->runtime.confirm_node(bridge_execution_store(store,user),bridge_text(scope),bridge_text(initial),
            bridge_text(revision),bridge_text(node),bridge_text(outcome),result,bridge_text(reference));return 0;
    }catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow confirmation failed");}return -1;
}
int libai_workflow_destroy(libai_workflow *handle) {
    if(!handle)return 0;
    try{auto guard=lock(handle);guard.unlock();delete handle;return 0;}
    catch(const std::exception &e){bridge_error(e.what());}catch(...){bridge_error("workflow destruction failed");}return -1;
}
}
