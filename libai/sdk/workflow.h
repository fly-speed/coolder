#pragma once
#include "agent_types.h"
#include "vendor/rapidjson/writer.h"
#include "workflow_journal.h"
#include <future>
namespace libai {
struct workflow_node_result_t {
    std::string patch_json="{}",route,error;
    run_status_t status=run_status_t::completed;
};
struct workflow_context_t {
    tool_context_t control;
    // Namespace for a node's durable Agent, not a key for all its business operations.
    std::string execution_scope;
    bool stopped()const{return control.stopped();}
};
using workflow_handler_t=std::function<workflow_node_result_t(const std::string&,const workflow_context_t&)>;
struct workflow_node_t {
    std::string name;
    std::string execution_revision="1";
    bool side_effect_free=false;
    // Top-level state keys. Parallel writers must be disjoint before execution.
    std::vector<std::string> writes;
    // Explicit host assertion: callback and captured state support worker threads.
    bool parallel_safe=false;
    workflow_handler_t execute;
};
struct workflow_options_t {
    // Separate owner/lease from stores used by node Agents.
    std::shared_ptr<execution_store_t> execution_store;
    std::size_t max_parallel=1,max_node_calls=64,max_state_bytes=2*1024*1024;
    std::chrono::milliseconds timeout{300000};
    std::shared_ptr<cancellation_t> cancellation;
    // Stable task identity for node Agent journals, never generated on retry.
    std::string scope;
};
struct workflow_result_t {
    bool progress_available=true;
    std::string journal_revision;
    task_contract_t task_contract;
    run_status_t status=run_status_t::completed;
    std::string state_json="{}",error;
    std::map<std::string,std::string> nodes;
    std::size_t node_calls=0;
};
namespace workflow_detail {
namespace json=governance_detail::json;
inline std::string wire(const json::Value &value){json::StringBuffer b;json::Writer<json::StringBuffer> writer(b);value.Accept(writer);return {b.GetString(),b.GetSize()};}
inline bool name(const std::string &s){if(s.empty()||s.size()>32)return false;for(char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return false;return true;}
inline json::Document object(const std::string &text,std::size_t maximum){json::Document d;governance_detail::parse_json_bounded(text,d,maximum);if(!d.IsObject())throw std::invalid_argument("workflow state/patch must be an object");return d;}
}
// Bounded DAG execution. Graph mutation and simultaneous/reentrant run are rejected.
// External effects belong in durable Agents. This graph does not auto-retry nodes.
class workflow_t {
    struct edge {std::string from,to,label;};
    std::map<std::string,workflow_node_t> nodes_;
    std::vector<edge> edges_;
    std::atomic<bool> busy_{false};
    workflow_result_t durable_run(const std::string &,const workflow_options_t &);
    std::string definition()const;
    void check_journal(workflow_storage::journal &)const;
    void check_output(const std::string &,const workflow_node_result_t &,std::size_t)const;
    class guard {
        std::atomic<bool> &busy_;
    public:
        explicit guard(std::atomic<bool> &b):busy_(b){bool old=false;if(!b.compare_exchange_strong(old,true))throw std::logic_error("workflow busy");}
        ~guard(){busy_.store(false);}
    };
    void validate()const {
        if(nodes_.empty())throw std::invalid_argument("workflow is empty");
        std::map<std::string,std::size_t> degree;for(const auto &n:nodes_)degree[n.first]=0;
        for(const auto &e:edges_){if(!nodes_.count(e.from)||!nodes_.count(e.to))throw std::invalid_argument("workflow edge references missing node");++degree[e.to];}
        std::vector<std::string> roots;for(const auto &n:degree)if(n.second==0)roots.push_back(n.first);
        std::size_t visited=0;while(!roots.empty()){auto name=roots.back();roots.pop_back();++visited;
            for(const auto &e:edges_)if(e.from==name && --degree[e.to]==0)roots.push_back(e.to);
        }if(visited!=nodes_.size())throw std::invalid_argument("workflow cycle detected");
    }
public:
    workflow_t()=default;
    workflow_t(const workflow_t&)=delete;workflow_t &operator=(const workflow_t&)=delete;
    workflow_t &add_node(const workflow_node_t &node){
        guard lock(busy_);if(!workflow_detail::name(node.name)||!node.execute||nodes_.count(node.name)||nodes_.size()>=64||node.execution_revision.empty()||node.execution_revision.size()>128||node.execution_revision.find('\0')!=std::string::npos)
            throw std::invalid_argument("workflow node needs unique name/handler, maximum 64 nodes");
        std::set<std::string> writes;for(const auto &key:node.writes){
            if(key.empty()||key.size()>256||key.find('\0')!=std::string::npos||!writes.insert(key).second)throw std::invalid_argument("invalid/duplicate workflow write key");
            workflow_detail::json::Document probe;probe.SetObject();workflow_detail::json::Value value;
            value.SetString(key.data(),static_cast<workflow_detail::json::SizeType>(key.size()),probe.GetAllocator());probe.AddMember(value,0,probe.GetAllocator());
            workflow_detail::object(workflow_detail::wire(probe),2048);
        }
        if(writes.size()>64)throw std::invalid_argument("workflow write declarations exceed 64 keys");
        nodes_.emplace(node.name,node);return *this;
    }
    // Empty label is unconditional; nonempty labels form the source node's route choices.
    workflow_t &add_edge(const std::string &from,const std::string &to,const std::string &label="") {
        guard lock(busy_);if(!workflow_detail::name(from)||!workflow_detail::name(to)||from==to||(!label.empty()&&!workflow_detail::name(label))||edges_.size()>=256)
            throw std::invalid_argument("invalid workflow edge");
        for(const auto &e:edges_)if(e.from==from&&e.to==to&&e.label==label)throw std::invalid_argument("duplicate workflow edge");
        edges_.push_back({from,to,label});return *this;
    }
    workflow_result_t run(const std::string &initial,const workflow_options_t &options=workflow_options_t()) {
        guard lock(busy_);validate();
        if(options.max_parallel<1||options.max_parallel>64||options.max_node_calls>64||options.max_state_bytes<1||options.max_state_bytes>2*1024*1024||options.timeout.count()<0||options.timeout>std::chrono::hours(24))
            throw std::invalid_argument("invalid workflow limits");
        if(options.scope.size()>64)throw std::invalid_argument("invalid workflow scope");
        for(char c:options.scope)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))throw std::invalid_argument("invalid workflow scope");
        if(options.execution_store)return durable_run(initial,options);
        using namespace workflow_detail;auto state=object(initial,options.max_state_bytes);workflow_result_t result;
        std::map<std::string,std::string> routes;for(const auto &n:nodes_)result.nodes[n.first]="pending";
        tool_context_t context;context.cancellation=options.cancellation;
        context.deadline=options.timeout.count()?std::chrono::steady_clock::now()+options.timeout:std::chrono::steady_clock::time_point::max();
        auto finish=[&](run_status_t status,const std::string &error=""){
            static const char *outcomes[]={"completed","cancelled","deadline_exceeded","budget_exceeded","model_error","protocol_error","callback_error","recovery_required"};
            result.status=status;result.error=error;result.state_json=wire(state);
            result.task_contract.loop_finished=true;result.task_contract.loop_outcome=outcomes[static_cast<unsigned>(status)];return result;
        };
        auto stopped=[&](){return finish(options.cancellation&&options.cancellation->cancelled()?run_status_t::cancelled:run_status_t::deadline_exceeded,"workflow stopped");};
        for(;;){
            if(context.stopped())return stopped();
            std::vector<std::string> ready;
            bool changed;
            do {changed=false;ready.clear();for(const auto &n:nodes_){if(result.nodes[n.first]!="pending")continue;
                bool source=true,waiting=false,active=false;
                for(const auto &e:edges_)if(e.to==n.first){source=false;auto status=result.nodes[e.from];
                    if(status=="pending"||status=="running")waiting=true;
                    if(status=="completed"&&(e.label.empty()||e.label==routes[e.from]))active=true;
                }
                if(waiting)continue;if(source||active)ready.push_back(n.first);
                else{result.nodes[n.first]="skipped";changed=true;}
            }}while(changed);
            if(ready.empty())return finish(run_status_t::completed);
            ready.resize(std::min(ready.size(),options.max_parallel));
            if(result.node_calls+ready.size()>options.max_node_calls)return finish(run_status_t::budget_exceeded,"workflow node budget exceeded");
            std::set<std::string> writers;
            if(ready.size()>1)for(const auto &id:ready){
                if(!nodes_.at(id).parallel_safe)return finish(run_status_t::protocol_error,"parallel callback lacks host safety declaration");
                for(const auto &key:nodes_.at(id).writes)if(!writers.insert(key).second)return finish(run_status_t::protocol_error,"parallel state write conflict before callbacks");
            }
            const auto snapshot=wire(state);std::vector<std::future<workflow_node_result_t>> futures;
            futures.reserve(ready.size());
            auto execute=[&](const std::string &id){workflow_context_t control;control.control=context;
                if(!options.scope.empty())control.execution_scope="wf"+std::to_string(options.scope.size())+"_"+options.scope+"_"+id;
                try{if(control.stopped()){workflow_node_result_t r;r.error="workflow stopped before callback";return r;}return nodes_.at(id).execute(snapshot,control);}catch(...){workflow_node_result_t r;r.error="workflow callback threw";return r;}
            };
            // Launch failure still waits for every previously started callback.
            bool launch_failed=false;
            for(const auto &id:ready){try{
                auto policy=ready.size()>1?std::launch::async:std::launch::deferred;
                futures.push_back(std::async(policy,execute,id));result.nodes[id]="running";++result.node_calls;
            }catch(...){launch_failed=true;break;}}
            std::vector<workflow_node_result_t> outputs;outputs.reserve(ready.size());for(auto &f:futures){try{outputs.push_back(f.get());}catch(...){workflow_node_result_t r;r.error="workflow worker failed";outputs.push_back(r);}}
            if(launch_failed){for(std::size_t i=0;i<outputs.size();++i)result.nodes[ready[i]]="failed";return finish(run_status_t::callback_error,"workflow worker launch failed; started effects require host reconciliation");}
            if(context.stopped()){for(const auto &id:ready)result.nodes[id]="stopped";return stopped();}
            std::vector<json::Document> patches;bool failed=false;run_status_t failure_status=run_status_t::callback_error;
            for(std::size_t i=0;i<ready.size();++i){const auto &id=ready[i];const auto &output=outputs[i];
                try{
                    if(static_cast<unsigned>(output.status)>static_cast<unsigned>(run_status_t::recovery_required))throw std::runtime_error("invalid workflow child status");
                    if(output.status!=run_status_t::completed){
                        if(output.status==run_status_t::recovery_required)failure_status=output.status;
                        else if(failure_status!=run_status_t::recovery_required)failure_status=output.status;
                        throw std::runtime_error("workflow child did not complete");
                    }
                    if(!output.error.empty())throw std::runtime_error("workflow callback reported error");
                    auto patch=object(output.patch_json,options.max_state_bytes);
                    std::set<std::string> allowed(nodes_.at(id).writes.begin(),nodes_.at(id).writes.end());
                    for(auto field=patch.MemberBegin();field!=patch.MemberEnd();++field)
                        if(!allowed.count(std::string(field->name.GetString(),field->name.GetStringLength())))throw std::runtime_error("undeclared workflow state write");
                    std::set<std::string> labels;for(const auto &e:edges_)if(e.from==id&&!e.label.empty())labels.insert(e.label);
                    if((labels.empty()&&!output.route.empty())||(!labels.empty()&&!labels.count(output.route)))throw std::runtime_error("invalid workflow route");
                    patches.push_back(std::move(patch));result.nodes[id]="produced";
                }catch(...){failed=true;result.nodes[id]=output.status==run_status_t::recovery_required?"recovery_required":"failed";patches.emplace_back();}
            }
            if(failed)return finish(failure_status,"workflow child/output/route failed; wave state was not committed and effects require host reconciliation");
            if(context.stopped()){for(const auto &id:ready)result.nodes[id]="stopped";return stopped();}
            json::Document next;next.CopyFrom(state,next.GetAllocator());
            for(std::size_t i=0;i<ready.size();++i)for(auto field=patches[i].MemberBegin();field!=patches[i].MemberEnd();++field){
                std::string key(field->name.GetString(),field->name.GetStringLength());next.RemoveMember(key.c_str());
                json::Value name,value;name.CopyFrom(field->name,next.GetAllocator());value.CopyFrom(field->value,next.GetAllocator());next.AddMember(name,value,next.GetAllocator());
            }
            auto merged=wire(next);
            if(merged.size()>options.max_state_bytes)return finish(run_status_t::budget_exceeded,"workflow merged state exceeds limit; effects require host reconciliation");
            try{object(merged,options.max_state_bytes);}catch(...){return finish(run_status_t::protocol_error,"workflow merged JSON exceeds structural limits; effects require host reconciliation");}
            state.Swap(next);for(std::size_t i=0;i<ready.size();++i){routes[ready[i]]=outputs[i].route;result.nodes[ready[i]]="completed";}
        }
    }
    // Trusted host receipt; never expose to the model as an unrestricted tool.
    void confirm_node(std::shared_ptr<execution_store_t>,const std::string &scope,
        const std::string &initial,const std::string &expected_revision,const std::string &node,
        const std::string &outcome,const workflow_node_result_t &output,const std::string &reference);
};
}
#include "workflow_durable_run.h"
