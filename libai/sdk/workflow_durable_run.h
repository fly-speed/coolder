#pragma once
#include "workflow.h"
#include <tuple>
namespace libai {
inline std::string workflow_t::definition()const {
    using namespace workflow_storage;json::Document d;d.SetObject();json::Value nodes(json::kArrayType),edges(json::kArrayType);
    for(const auto &i:nodes_){json::Document n;n.SetObject();field(n,"name",i.first);field(n,"revision",i.second.execution_revision);n.AddMember("parallel_safe",i.second.parallel_safe,n.GetAllocator());n.AddMember("side_effect_free",i.second.side_effect_free,n.GetAllocator());
        std::set<std::string> ordered(i.second.writes.begin(),i.second.writes.end());json::Value writes(json::kArrayType);for(const auto &s:ordered){json::Value v;v.SetString(s.data(),s.size(),n.GetAllocator());writes.PushBack(v,n.GetAllocator());}n.AddMember("writes",writes,n.GetAllocator());json::Value value;value.CopyFrom(n,d.GetAllocator());nodes.PushBack(value,d.GetAllocator());}
    std::set<std::tuple<std::string,std::string,std::string>> ordered;for(const auto &e:edges_)ordered.emplace(e.from,e.to,e.label);
    for(const auto &e:ordered){json::Document value;value.SetObject();field(value,"from",std::get<0>(e));field(value,"to",std::get<1>(e));field(value,"label",std::get<2>(e));json::Value copy;copy.CopyFrom(value,d.GetAllocator());edges.PushBack(copy,d.GetAllocator());}
    d.AddMember("nodes",nodes,d.GetAllocator());d.AddMember("edges",edges,d.GetAllocator());return wire(d);
}
inline void workflow_t::check_output(const std::string &id,const workflow_node_result_t &output,std::size_t limit)const {
    if(output.status!=run_status_t::completed||!output.error.empty())throw std::runtime_error("workflow child did not complete");
    auto patch=workflow_detail::object(output.patch_json,limit);std::set<std::string> writes(nodes_.at(id).writes.begin(),nodes_.at(id).writes.end());
    for(auto i=patch.MemberBegin();i!=patch.MemberEnd();++i)if(!writes.count(std::string(i->name.GetString(),i->name.GetStringLength())))throw std::runtime_error("undeclared workflow write");
    std::set<std::string> labels;for(const auto &e:edges_)if(e.from==id&&!e.label.empty())labels.insert(e.label);
    if((labels.empty()&&!output.route.empty())||(!labels.empty()&&!labels.count(output.route)))throw std::runtime_error("workflow route invalid");
}
inline void workflow_t::check_journal(workflow_storage::journal &j)const {
    using namespace workflow_storage;
        if(j.entries.size()!=nodes_.size()||now()<j.last_seen||j.started>INT64_MAX-j.timeout_ms)throw std::runtime_error("workflow journal identity/time invalid");
        workflow_detail::object(j.state,2*1024*1024);
        std::set<std::string> wave(j.wave.begin(),j.wave.end());if(wave.size()!=j.wave.size()||(!j.wave.empty()&&j.wave_input!=j.state)|| (j.wave.empty()&&!j.wave_input.empty()))throw std::runtime_error("workflow frozen wave invalid");
        if(!std::is_sorted(j.wave.begin(),j.wave.end()))throw std::runtime_error("workflow frozen wave order invalid");
        std::set<std::string> writers;
        for(const auto &id:j.wave){if(!nodes_.count(id))throw std::runtime_error("workflow frozen wave node missing");
            if(j.wave.size()>1){if(!nodes_.at(id).parallel_safe)throw std::runtime_error("workflow frozen parallel declaration invalid");
                for(const auto &key:nodes_.at(id).writes)if(!writers.insert(key).second)throw std::runtime_error("workflow frozen wave write conflict");
            }
        }
        std::size_t admitted=0;
        for(const auto &n:j.entries){if(!nodes_.count(n.first))throw std::runtime_error("workflow journal node missing");
            const auto &phase=n.second.phase;
            if(phase!="pending"&&phase!="skipped"&&phase!="ready"&&phase!="running"&&phase!="produced"&&phase!="completed")throw std::runtime_error("workflow journal phase invalid");
            bool active=phase=="ready"||phase=="running"||phase=="produced";if(active!=bool(wave.count(n.first)))throw std::runtime_error("workflow journal wave membership invalid");
            admitted+=n.second.attempts;
            if((phase=="pending"||phase=="skipped")&&n.second.attempts)throw std::runtime_error("workflow inactive node has attempts");
            if((phase=="running"||phase=="produced"||phase=="completed")&&!n.second.attempts)throw std::runtime_error("workflow started node lacks attempts");
            if(phase=="produced"){workflow_node_result_t value;value.patch_json=n.second.patch;value.route=n.second.route;check_output(n.first,value,2*1024*1024);}
            else if(n.second.patch!="{}")throw std::runtime_error("workflow journal unexpected patch");
            if(phase!="completed"&&phase!="produced"&&!n.second.route.empty())throw std::runtime_error("workflow journal unexpected route");
            if(phase=="completed"){workflow_node_result_t value;value.route=n.second.route;check_output(n.first,value,2*1024*1024);}
            bool source=true,eligible=false;
            for(const auto &e:edges_)if(e.to==n.first){source=false;const auto &p=j.entries.at(e.from);if(phase!="pending"&&p.phase!="completed"&&p.phase!="skipped")throw std::runtime_error("workflow journal dependency unfinished");if(p.phase=="completed"&&(e.label.empty()||p.route==e.label))eligible=true;}
            if(phase=="skipped"&&(source||eligible))throw std::runtime_error("workflow journal skipped active node");
            if(phase!="pending"&&phase!="skipped"&&!source&&!eligible)throw std::runtime_error("workflow journal inactive executed node");
        }
        if(j.calls!=admitted)throw std::runtime_error("workflow journal attempt count invalid");
        if(j.finished&&j.status==run_status_t::completed){if(!j.wave.empty())throw std::runtime_error("workflow finished with active wave");for(const auto &n:j.entries)if(n.second.phase!="completed"&&n.second.phase!="skipped")throw std::runtime_error("workflow finished with pending nodes");}
}
inline workflow_result_t workflow_t::durable_run(const std::string &initial,const workflow_options_t &options) {
    using namespace workflow_storage;workflow_result_t result;
    auto view=[&](journal &j,run_status_t status,const std::string &error=""){
        static const char *names[]={"completed","cancelled","deadline_exceeded","budget_exceeded","model_error","protocol_error","callback_error","recovery_required"};
        result.node_calls=j.calls;result.state_json=j.state;result.status=status;result.error=error;result.journal_revision=j.revision;
        for(const auto &n:j.entries)result.nodes[n.first]=n.second.phase=="ready"?"pending":n.second.phase;
        result.task_contract.loop_finished=true;result.task_contract.loop_outcome=names[static_cast<unsigned>(status)];return result;
    };
    try {
        auto input=workflow_detail::wire(workflow_detail::object(initial,2*1024*1024));journal j(options.execution_store,options.scope);
        auto descriptor=definition();
        if(!j.loaded){j.definition=descriptor;j.initial=input;j.state=input;j.budget=options.max_node_calls;j.state_limit=options.max_state_bytes;j.started=now();j.last_seen=j.started;j.timeout_ms=options.timeout.count();for(const auto &n:nodes_)j.entries.emplace(n.first,entry());j.save();}
        if(j.definition!=descriptor||j.initial!=input){result.progress_available=false;result.state_json="";result.status=run_status_t::recovery_required;result.error="workflow definition or initial state changed";result.task_contract.loop_finished=true;result.task_contract.loop_outcome="recovery_required";return result;}
        check_journal(j);
        j.budget=std::min(j.budget,options.max_node_calls);j.state_limit=std::min(j.state_limit,options.max_state_bytes);
        if(options.timeout.count()&&(!j.timeout_ms||options.timeout.count()<j.timeout_ms))j.timeout_ms=options.timeout.count();
        j.save();
        if(j.state.size()>j.state_limit)return view(j,run_status_t::budget_exceeded,"workflow restored state exceeds current limit");
        if(j.finished)return view(j,j.status,j.error);
        tool_context_t control;control.cancellation=options.cancellation;control.deadline=std::chrono::steady_clock::time_point::max();
        if(j.timeout_ms){auto remaining=j.started+j.timeout_ms-now();if(remaining<=0)return view(j,run_status_t::deadline_exceeded,"workflow persistent deadline exceeded");control.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(remaining);}
        auto terminal=[&](run_status_t status,const std::string &error=""){j.finished=true;j.status=status;j.error=error;j.save();return view(j,status,error);};
        for(;;){
            if(control.stopped())return view(j,options.cancellation&&options.cancellation->cancelled()?run_status_t::cancelled:run_status_t::deadline_exceeded,"workflow stopped; stored progress retained");
            if(j.wave.empty()){
                bool changed;std::vector<std::string> ready;
                do{changed=false;ready.clear();for(auto &n:j.entries){if(n.second.phase!="pending")continue;bool source=true,waiting=false,active=false;
                    for(const auto &e:edges_)if(e.to==n.first){source=false;const auto &parent=j.entries.at(e.from);if(parent.phase!="completed"&&parent.phase!="skipped")waiting=true;if(parent.phase=="completed"&&(e.label.empty()||parent.route==e.label))active=true;}
                    if(waiting)continue;if(source||active)ready.push_back(n.first);else{n.second.phase="skipped";changed=true;}
                }}while(changed);
                if(ready.empty())return terminal(run_status_t::completed);
                ready.resize(std::min(ready.size(),options.max_parallel));std::set<std::string>writers;
                if(ready.size()>1)for(const auto &id:ready){if(!nodes_.at(id).parallel_safe)return terminal(run_status_t::protocol_error,"parallel node lacks safety declaration");for(const auto &key:nodes_.at(id).writes)if(!writers.insert(key).second)return terminal(run_status_t::protocol_error,"parallel state write conflict before callbacks");}
                j.wave=ready;j.wave_input=j.state;for(const auto &id:ready)j.entries[id].phase="ready";j.save();
            }
            bool retry=false;
            for(const auto &id:j.wave)if(j.entries.at(id).phase=="running"){
                if(!nodes_.at(id).side_effect_free)return view(j,run_status_t::recovery_required,"workflow node effect unconfirmed; host receipt required");
                j.entries[id].phase="ready";retry=true;
            }
            if(retry)j.save();
            std::vector<std::string> ready;for(const auto &id:j.wave)if(j.entries.at(id).phase=="ready")ready.push_back(id);
            ready.resize(std::min(ready.size(),options.max_parallel));
            if(!ready.empty()){
                if(j.calls+ready.size()>j.budget)return terminal(run_status_t::budget_exceeded,"workflow persistent node budget exceeded");
                if(ready.size()>1)for(const auto &id:ready)if(!nodes_.at(id).parallel_safe)return terminal(run_status_t::protocol_error,"restored parallel node lacks safety declaration");
                std::vector<std::future<workflow_node_result_t>> futures;futures.reserve(ready.size());
                for(const auto &id:ready){j.entries[id].phase="running";++j.entries[id].attempts;}j.calls+=ready.size();j.save(); // All intent precedes any callback.
                auto execute=[&](const std::string &id){workflow_context_t context;context.control=control;context.execution_scope="wf"+std::to_string(options.scope.size())+"_"+options.scope+"_"+id;
                    try{if(context.stopped()){workflow_node_result_t r;r.status=run_status_t::cancelled;r.error="stopped before callback";return r;}return nodes_.at(id).execute(j.wave_input,context);}catch(...){workflow_node_result_t r;r.error="workflow callback threw";return r;}
                };
                bool failed=false;run_status_t failure=run_status_t::callback_error;
                try{for(const auto &id:ready)futures.push_back(std::async(ready.size()>1?std::launch::async:std::launch::deferred,execute,id));}
                catch(...){for(auto &f:futures)try{f.get();}catch(...){}return view(j,run_status_t::recovery_required,"workflow launch failed; recorded intent requires reconciliation");}
                for(std::size_t i=0;i<futures.size();++i){auto output=futures[i].get();const auto &id=ready[i];
                    try{check_output(id,output,j.state_limit);j.entries[id].phase="produced";j.entries[id].patch=workflow_detail::wire(workflow_detail::object(output.patch_json,j.state_limit));j.entries[id].route=output.route;}
                    catch(...){failed=true;if(!nodes_.at(id).side_effect_free)failure=run_status_t::recovery_required;else{j.entries[id].phase="ready";if(failure!=run_status_t::recovery_required)failure=static_cast<unsigned>(output.status)<=static_cast<unsigned>(run_status_t::recovery_required)&&output.status!=run_status_t::completed?output.status:run_status_t::callback_error;}}
                    j.save();
                }
                if(failed)return view(j,failure,"workflow node failed; known results retained, unknown effects require reconciliation");
                if(control.stopped())return view(j,options.cancellation&&options.cancellation->cancelled()?run_status_t::cancelled:run_status_t::deadline_exceeded,"workflow stopped after callbacks; results retained");
            }
            bool complete=true;for(const auto &id:j.wave)if(j.entries.at(id).phase!="produced")complete=false;if(!complete)continue;
            auto next=workflow_detail::object(j.state,2*1024*1024);
            for(const auto &id:j.wave){auto patch=workflow_detail::object(j.entries.at(id).patch,2*1024*1024);for(auto i=patch.MemberBegin();i!=patch.MemberEnd();++i){std::string key(i->name.GetString(),i->name.GetStringLength());next.RemoveMember(key.c_str());json::Value k,v;k.CopyFrom(i->name,next.GetAllocator());v.CopyFrom(i->value,next.GetAllocator());next.AddMember(k,v,next.GetAllocator());}}
            auto merged=workflow_detail::wire(next);if(merged.size()>j.state_limit)return terminal(run_status_t::budget_exceeded,"workflow merged state exceeds limit; effects already recorded");
            try{workflow_detail::object(merged,j.state_limit);}catch(...){return terminal(run_status_t::protocol_error,"workflow merged JSON structural limit exceeded");}
            j.state=merged;for(const auto &id:j.wave){j.entries[id].phase="completed";j.entries[id].patch="{}";}j.wave.clear();j.wave_input.clear();j.save();
        }
    }catch(...){result.progress_available=false;result.journal_revision.clear();result.state_json="";result.nodes.clear();result.status=run_status_t::recovery_required;result.error="workflow storage/journal failure; no automatic replay";result.task_contract.loop_finished=true;result.task_contract.loop_outcome="recovery_required";return result;}
}
inline void workflow_t::confirm_node(std::shared_ptr<execution_store_t> store,const std::string &scope,const std::string &initial,
    const std::string &expected,const std::string &id,const std::string &outcome,const workflow_node_result_t &output,const std::string &reference) {
    guard lock(busy_);validate();if(scope.empty()||scope.size()>64||!nodes_.count(id)||reference.empty()||reference.size()>4096||reference.find('\0')!=std::string::npos||(outcome!="none"&&outcome!="applied"))throw std::invalid_argument("workflow receipt invalid");
    for(char c:scope)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))throw std::invalid_argument("workflow receipt scope invalid");
    workflow_storage::journal j(store,scope);auto input=workflow_detail::wire(workflow_detail::object(initial,2*1024*1024));
    if(!j.loaded||j.finished||j.revision!=expected||j.definition!=definition()||j.initial!=input||!j.entries.count(id)||j.entries.at(id).phase!="running"||std::find(j.wave.begin(),j.wave.end(),id)==j.wave.end())throw std::runtime_error("workflow receipt does not match current intent");
    check_journal(j);
    if(outcome=="applied"){check_output(id,output,j.state_limit);j.entries[id].phase="produced";j.entries[id].patch=workflow_detail::wire(workflow_detail::object(output.patch_json,j.state_limit));j.entries[id].route=output.route;}
    else{j.entries[id].phase="ready";j.entries[id].route.clear();j.entries[id].patch="{}";}
    j.entries[id].receipt=outcome+":"+reference;j.save();
}
}
