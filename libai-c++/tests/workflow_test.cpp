#include <libai_cpp/workflow.h>
#include <libai_cpp/agent.h>
#include <cassert>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#endif
using namespace libai::cpp;
template<class F>void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}assert(failed);}
WorkflowNodeResult output(std::string patch="{}",std::string route=""){WorkflowNodeResult r;r.patch_json=std::move(patch);r.route=std::move(route);return r;}
WorkflowNode node(std::string id,std::vector<std::string>writes,WorkflowHandler handler,bool safe=false){WorkflowNode n;n.name=std::move(id);n.writes=std::move(writes);n.execute=std::move(handler);n.parallel_safe=true;n.side_effect_free=safe;return n;}
struct Store:ExecutionStore {
    ExecutionSnapshot snapshot;bool leased=false;int sequence=0;
    ExecutionSnapshot open(const std::string&)override{assert(!leased);leased=true;return snapshot;}
    std::string commit(const std::string&,const std::string&revision,const std::string&state)override{assert(leased&&revision==snapshot.revision);snapshot.state_json=state;return snapshot.revision=std::to_string(++sequence);}
    void close(const std::string&)noexcept override{leased=false;}
};
void parallel_branch_and_conflict(){
    Workflow graph;std::mutex mutex;std::condition_variable changed;int started=0,no=0,joined=0;
    auto barrier=[&]{std::unique_lock<std::mutex> lock(mutex);++started;changed.notify_all();assert(changed.wait_for(lock,std::chrono::seconds(2),[&]{return started==2;}));};
    graph.add_node(node("choose",{},[](const std::string&,const WorkflowContext&){return output("{}","yes");}));
    graph.add_node(node("a",{"a"},[&](const std::string&s,const WorkflowContext&c){assert(s=="{}"&&c.execution_scope=="wf3_job_a"&&c.control.idempotency_key.empty());barrier();return output(R"({"a":1})");}));
    graph.add_node(node("b",{"b"},[&](const std::string&s,const WorkflowContext&){assert(s=="{}");barrier();return output(R"({"b":2})");}));
    graph.add_node(node("no",{},[&](const std::string&,const WorkflowContext&){++no;return output();}));
    graph.add_node(node("join",{},[&](const std::string&s,const WorkflowContext&){++joined;assert(s.find("a")!=std::string::npos&&s.find("b")!=std::string::npos);return output();}));
    graph.add_edge("choose","a","yes").add_edge("choose","b","yes").add_edge("choose","no","no").add_edge("a","join").add_edge("b","join").add_edge("no","join");
    WorkflowOptions options;options.max_parallel=2;options.scope="job";auto r=graph.run("{}",options);
    assert(r.status==RunStatus::completed&&r.node_calls==4&&r.nodes["no"]=="skipped"&&no==0&&joined==1&&!r.task_contract.accepted());
    Workflow conflict;int effects=0;auto f=[&](const std::string&,const WorkflowContext&){++effects;return output();};
    conflict.add_node(node("a",{"x"},f)).add_node(node("b",{"x"},f));assert(conflict.run("{}",options).status==RunStatus::protocol_error&&effects==0);
}
void persistence_receipts_and_cancel(){
    auto store=std::make_shared<Store>();Workflow graph;int a=0,b=0;
    graph.add_node(node("a",{"a"},[&](const std::string&,const WorkflowContext&){++a;return output(R"({"a":1})");}));
    graph.add_node(node("b",{"b"},[&](const std::string&,const WorkflowContext&){++b;WorkflowNodeResult r;r.error="lost effect acknowledgement";return r;}));
    WorkflowOptions options;options.execution_store=store;options.scope="job";options.max_parallel=1;
    auto r=graph.run("{}",options);assert(r.status==RunStatus::recovery_required&&a==1&&b==1);
    r=graph.run("{}",options);assert(r.status==RunStatus::recovery_required&&a==1&&b==1);
    rejects([&]{graph.confirm_node(store,"job","{}","stale","b","applied",output(R"({"b":2})"),"receipt/1");});
    graph.confirm_node(store,"job","{}",r.journal_revision,"b","applied",output(R"({"b":2})"),"receipt/1");
    r=graph.run("{}",options);assert(r.status==RunStatus::completed&&a==1&&b==1&&r.progress_available);
    assert(graph.run("{}",options).status==RunStatus::completed&&a==1&&b==1);
    assert(graph.run(R"({"changed":true})",options).status==RunStatus::recovery_required);
    Workflow stopped;stopped.add_node(node("wait",{},[&](const std::string&,const WorkflowContext&c){rejects([&]{stopped.run("{}");});while(!c.stopped())std::this_thread::sleep_for(std::chrono::milliseconds(1));return output();}));
    WorkflowOptions limit;limit.timeout=std::chrono::milliseconds(15);assert(stopped.run("{}",limit).status==RunStatus::deadline_exceeded);
    limit.cancellation=std::make_shared<Cancellation>();limit.cancellation->cancel();assert(stopped.run("{}",limit).status==RunStatus::cancelled);
}
void independent_agent_composition(){
    Workflow graph;auto store=std::make_shared<Store>();int models=0,effects=0;
    graph.add_node(node("agent",{"answer"},[&](const std::string&input,const WorkflowContext&context){
        Agent agent([&](const ModelRequest&,const StreamSink&){ModelResponse r;if(++models==1){ToolCall call;call.id="call1";call.name="effect";call.arguments="{}";r.calls.push_back(call);}else r.text=R"({"answer":3})";return r;});
        ToolSpec spec;spec.name="effect";spec.parameters=R"({"type":"object"})";
        agent.add_tool(spec,[&](const ToolCall&,const ToolContext&c){++effects;assert(c.idempotency_key=="wf3_job_agent.0");return ToolResult::success("3");});
        RunOptions options;options.execution_store=store;options.execution_scope=context.execution_scope;options.cancellation=context.control.cancellation;
        auto run=agent.run(input,options);WorkflowNodeResult r;r.status=run.status;r.patch_json=run.text.empty()?"{}":run.text;return r;
    }));
    WorkflowOptions options;options.scope="job";assert(graph.run("{}",options).status==RunStatus::completed);assert(graph.run("{}",options).status==RunStatus::completed&&models==2&&effects==1);
    assert(graph.run(R"({"changed":true})",options).status==RunStatus::recovery_required&&effects==1);
}
#if defined(__unix__) || defined(__APPLE__)
void process_exit(){
    char root[]="/tmp/libai-cpp-workflow-XXXXXX";assert(mkdtemp(root));std::string directory=root;
    auto worker=[&](bool terminate,int &effects){Workflow g;g.add_node(node("write",{},[&](const std::string&,const WorkflowContext&){++effects;if(terminate){std::ofstream file(directory+"/effect");file<<"once";file.close();_exit(71);}return output();}));
        WorkflowOptions options;options.scope="job";options.execution_store=create_file_execution_store(directory,ExecutionFileCodec::plaintext());return g.run("{}",options);};
    auto pid=fork();assert(pid>=0);if(!pid){int effects=0;worker(true,effects);_exit(72);}int status=0;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==71);
    int effects=0;auto result=worker(false,effects);assert(result.status==RunStatus::recovery_required&&effects==0);
    Workflow g;g.add_node(node("write",{},[](const std::string&,const WorkflowContext&){assert(false);return output();}));auto store=create_file_execution_store(directory,ExecutionFileCodec::plaintext());
    g.confirm_node(store,"job","{}",result.journal_revision,"write","applied",output(),"effect-file/1");WorkflowOptions options;options.scope="job";options.execution_store=store;assert(g.run("{}",options).status==RunStatus::completed);
    options.execution_store.reset();store.reset();unlink((directory+"/effect").c_str());unlink((directory+"/wfg3_job.state").c_str());unlink((directory+"/wfg3_job.lock").c_str());assert(rmdir(directory.c_str())==0);
}
#endif
int main(){parallel_branch_and_conflict();persistence_receipts_and_cancel();independent_agent_composition();
#if defined(__unix__) || defined(__APPLE__)
process_exit();
#endif
}
