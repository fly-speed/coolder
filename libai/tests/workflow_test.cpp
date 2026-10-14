#include <libai/workflow.h>
#include <libai/agent.h>
#include <cassert>
#include <thread>
#include <condition_variable>
using namespace libai;
using json=workflow_detail::json::Document;
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}assert(failed);}
workflow_node_t node(const std::string &id,std::vector<std::string> writes,workflow_handler_t callback,bool parallel=false){workflow_node_t n;n.name=id;n.writes=std::move(writes);n.execute=callback;n.parallel_safe=parallel;return n;}
workflow_node_result_t output(const std::string &patch="{}",const std::string &route=""){workflow_node_result_t r;r.patch_json=patch;r.route=route;return r;}
void fork_join(){
    workflow_t graph;std::mutex mutex;std::condition_variable changed;int started=0;std::thread::id caller=std::this_thread::get_id();
    auto wait=[&]{std::unique_lock<std::mutex> lock(mutex);++started;changed.notify_all();assert(changed.wait_for(lock,std::chrono::seconds(2),[&]{return started==2;}));};
    graph.add_node(node("root",{"seed"},[](const std::string &,const workflow_context_t&){return output(R"({"seed":3})");}));
    graph.add_node(node("left",{"a"},[&](const std::string &s,const workflow_context_t &c){assert(std::this_thread::get_id()!=caller);assert(c.execution_scope=="wf4_task_left" && c.control.idempotency_key.empty());assert(s.find("seed")!=std::string::npos && s.find("b")==std::string::npos);wait();return output(R"({"a":6})");},true));
    graph.add_node(node("right",{"b"},[&](const std::string &s,const workflow_context_t &){assert(s.find("a\"")==std::string::npos);wait();return output(R"({"b":9})");},true));
    graph.add_node(node("join",{"sum"},[](const std::string &s,const workflow_context_t&){auto d=workflow_detail::object(s,2048);assert(d["a"].GetInt()==6 && d["b"].GetInt()==9);return output(R"({"sum":15})");}));
    graph.add_edge("root","left").add_edge("root","right").add_edge("left","join").add_edge("right","join");
    workflow_options_t options;options.max_parallel=2;options.scope="task";auto result=graph.run("{}",options);
    assert(result.status==run_status_t::completed && result.node_calls==4 && result.nodes["join"]=="completed");
    assert(result.task_contract.loop_finished && !result.task_contract.accepted() && result.task_contract.build.state=="unknown");
    auto state=workflow_detail::object(result.state_json,2048);assert(state["sum"].GetInt()==15);
}
void branch(){
    workflow_t graph;int no=0,joined=0;
    graph.add_node(node("choose",{},[](const std::string &,const workflow_context_t&){return output("{}","yes");}));
    graph.add_node(node("yes",{"answer"},[](const std::string &,const workflow_context_t&){return output(R"({"answer":true})");}));
    graph.add_node(node("no",{},[&](const std::string &,const workflow_context_t&){++no;return output();}));
    graph.add_node(node("join",{},[&](const std::string &s,const workflow_context_t&){++joined;assert(s.find("answer")!=std::string::npos);return output();}));
    graph.add_edge("choose","yes","yes").add_edge("choose","no","no").add_edge("yes","join").add_edge("no","join");
    auto r=graph.run("{}");assert(r.status==run_status_t::completed && no==0 && joined==1 && r.nodes["no"]=="skipped" && r.node_calls==3);
}
void reject_before_effects(){
    int effects=0;auto f=[&](const std::string &,const workflow_context_t&){++effects;return output(R"({"x":1})");};
    workflow_t conflict;conflict.add_node(node("a",{"x"},f,true)).add_node(node("b",{"x"},f,true));workflow_options_t o;o.max_parallel=2;
    assert(conflict.run("{}",o).status==run_status_t::protocol_error && effects==0);
    workflow_t unsafe;unsafe.add_node(node("a",{"a"},f)).add_node(node("b",{"b"},f));assert(unsafe.run("{}",o).status==run_status_t::protocol_error && effects==0);
    workflow_t cycle;cycle.add_node(node("a",{},f)).add_node(node("b",{},f)).add_edge("a","b").add_edge("b","a");rejects([&]{cycle.run("{}");});assert(effects==0);
    workflow_t missing;missing.add_node(node("a",{},f)).add_edge("a","absent");rejects([&]{missing.run("{}");});
    rejects([&]{conflict.run(R"({"x":1,"x":2})");});
    o.scope=std::string(40,'a')+"!";rejects([&]{conflict.run("{}",o);});
    o=workflow_options_t();o.max_node_calls=0;assert(conflict.run("{}",o).status==run_status_t::budget_exceeded && effects==0);
}
void failed_wave_is_atomic(){
    workflow_t graph;
    graph.add_node(node("a",{"a"},[](const std::string &,const workflow_context_t&){return output(R"({"a":1})");},true));
    graph.add_node(node("b",{"b"},[](const std::string &,const workflow_context_t&){return output(R"({"undeclared":2})");},true));
    workflow_options_t options;options.max_parallel=2;auto r=graph.run(R"({"original":3})",options);
    assert(r.status==run_status_t::callback_error && r.nodes["a"]=="produced" && r.nodes["b"]=="failed" && r.state_json.find("a\"")==std::string::npos);
    workflow_t bad_route;bad_route.add_node(node("choose",{},[](const std::string &,const workflow_context_t&){return output("{}","not-a-label");}));
    bad_route.add_node(node("next",{},[](const std::string &,const workflow_context_t&){assert(false);return output();})).add_edge("choose","next","yes");
    r=bad_route.run("{}");assert(r.status==run_status_t::callback_error && r.nodes["next"]=="pending");
    std::string values="[";for(int i=0;i<34000;++i){if(i)values+=",";values+="0";}values+="]";
    workflow_t large;
    large.add_node(node("a",{"a"},[&](const std::string &,const workflow_context_t&){return output("{\"a\":"+values+"}");},true));
    large.add_node(node("b",{"b"},[&](const std::string &,const workflow_context_t&){return output("{\"b\":"+values+"}");},true));
    r=large.run("{}",options);assert(r.status==run_status_t::protocol_error && r.state_json=="{}" && r.nodes["a"]=="produced");
}
void cancellation_and_reentry(){
    workflow_t graph;graph.add_node(node("a",{},[&](const std::string &,const workflow_context_t &c){rejects([&]{graph.run("{}");});rejects([&]{graph.add_edge("a","b");});
        while(!c.stopped())std::this_thread::sleep_for(std::chrono::milliseconds(1));return output();}));
    workflow_options_t options;options.timeout=std::chrono::milliseconds(15);assert(graph.run("{}",options).status==run_status_t::deadline_exceeded);
    options.cancellation=std::make_shared<cancellation_t>();options.cancellation->cancel();auto r=graph.run("{}",options);assert(r.status==run_status_t::cancelled && r.node_calls==0);
}
struct Store:execution_store_t {
    execution_snapshot_t snapshot;int revision=0;bool leased=false;
    execution_snapshot_t open(const std::string &) override{assert(!leased);leased=true;return snapshot;}
    std::string commit(const std::string &,const std::string &expected,const std::string &state) override{
        assert(leased&&expected==snapshot.revision);snapshot.state_json=state;return snapshot.revision=std::to_string(++revision);
    }
    void close(const std::string &)noexcept override{leased=false;}
};
void durable_agent_composition(){
    workflow_t graph;auto store=std::make_shared<Store>();int models=0,effects=0;
    graph.add_node(node("agent",{"answer"},[&](const std::string &state,const workflow_context_t &ctx){
        auto model=std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){model_response_t r;
            if(++models==1){tool_call_t call;call.id="call1";call.name="add";call.arguments="{}";r.calls.push_back(call);}else r.text=R"({"answer":3})";return r;
        });
        agent_t agent(model);tool_spec_t tool;tool.name="add";tool.parameters=R"({"type":"object"})";
        agent.add_tool(tool,[&](const tool_call_t &,const tool_context_t &c){++effects;assert(c.idempotency_key=="wf3_job_agent.0");return tool_result_t::success("3");});
        agent.configure_output(R"({"type":"object","required":["answer"],"properties":{"answer":{"type":"integer"}}})");
        agent_session_t session;run_options_t options;options.execution_store=store;options.execution_scope=ctx.execution_scope;options.cancellation=ctx.control.cancellation;
        auto run=agent.run(session,state,options);workflow_node_result_t result;result.status=run.status;result.patch_json=run.text.empty()?"{}":run.text;return result;
    }));
    workflow_options_t options;options.scope="job";auto first=graph.run("{}",options);assert(first.status==run_status_t::completed);
    auto again=graph.run("{}",options);assert(again.status==run_status_t::completed && models==2 && effects==1);
    auto changed=graph.run(R"({"new_input":true})",options);assert(changed.status==run_status_t::recovery_required && models==2 && effects==1);
}
int main(){fork_join();branch();reject_before_effects();failed_wave_is_atomic();cancellation_and_reentry();durable_agent_composition();}
