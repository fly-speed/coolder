#include <libai/workflow.h>
#include <libai/sdk/file_execution_store.h>
#include <cassert>
#include <atomic>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#endif
using namespace libai;
struct Store:execution_store_t {
    execution_snapshot_t state;bool leased=false;std::size_t sequence=0;int fail_after=-1;
    execution_snapshot_t open(const std::string &)override{assert(!leased);leased=true;return state;}
    std::string commit(const std::string &,const std::string &expected,const std::string &next)override{
        assert(leased&&expected==state.revision);if(fail_after==0)throw std::runtime_error("injected store failure");if(fail_after>0)--fail_after;
        state.state_json=next;return state.revision=std::to_string(++sequence);
    }
    void close(const std::string &)noexcept override{leased=false;}
};
template<class F>void rejects(F f){bool failed=false;try{f();}catch(...){failed=true;}assert(failed);}
workflow_node_result_t output(std::string patch="{}",std::string route=""){workflow_node_result_t r;r.patch_json=std::move(patch);r.route=std::move(route);return r;}
workflow_node_t node(std::string name,std::vector<std::string> writes,workflow_handler_t f,bool safe=false){workflow_node_t n;n.name=std::move(name);n.writes=std::move(writes);n.execute=f;n.parallel_safe=true;n.side_effect_free=safe;return n;}
workflow_options_t options(std::shared_ptr<execution_store_t> store){workflow_options_t o;o.execution_store=std::move(store);o.scope="job";o.max_parallel=2;return o;}
void branch_cache_and_freeze(){
    auto store=std::make_shared<Store>();int choose=0,yes=0,no=0,join=0;workflow_t graph;
    graph.add_node(node("choose",{},[&](const std::string &,const workflow_context_t&){++choose;return output("{}","yes");}));
    graph.add_node(node("yes",{"answer"},[&](const std::string &,const workflow_context_t &c){++yes;assert(c.execution_scope=="wf3_job_yes");return output(R"({"answer":1})");}));
    graph.add_node(node("no",{},[&](const std::string &,const workflow_context_t&){++no;return output();}));
    graph.add_node(node("join",{},[&](const std::string &s,const workflow_context_t&){++join;assert(s.find("answer")!=std::string::npos);return output();}));
    graph.add_edge("choose","yes","yes").add_edge("choose","no","no").add_edge("yes","join").add_edge("no","join");
    auto o=options(store);auto r=graph.run("{}",o);assert(r.status==run_status_t::completed && r.nodes["no"]=="skipped");
    assert(graph.run("{}",o).status==run_status_t::completed && choose==1&&yes==1&&no==0&&join==1);
    assert(graph.run(R"({"changed":true})",o).status==run_status_t::recovery_required && yes==1);
    workflow_t changed;auto n=node("choose",{},[&](const std::string &,const workflow_context_t&){assert(false);return output();});n.execution_revision="2";changed.add_node(n);
    assert(changed.run("{}",o).status==run_status_t::recovery_required);
}
void partial_parallel_and_receipt(){
    auto store=std::make_shared<Store>();std::atomic<int>a{0},b{0};workflow_t graph;
    graph.add_node(node("a",{"a"},[&](const std::string &,const workflow_context_t&){++a;return output(R"({"a":1})");}));
    graph.add_node(node("b",{"b"},[&](const std::string &,const workflow_context_t&){++b;workflow_node_result_t r;r.error="lost response after effect";return r;}));
    auto o=options(store);auto r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required && r.nodes["a"]=="produced" && r.nodes["b"]=="running" && a==1&&b==1);
    r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required && a==1&&b==1);
    auto revision=r.journal_revision;rejects([&]{graph.confirm_node(store,"job","{}","stale","b","applied",output(R"({"b":2})"),"receipt/1");});
    rejects([&]{graph.confirm_node(store,"job","{}",revision,"b","applied",output(R"({"wrong":2})"),"receipt/1");});
    graph.confirm_node(store,"job","{}",revision,"b","applied",output(R"({"b":2})"),"receipt/1");
    r=graph.run("{}",o);assert(r.status==run_status_t::completed && a==1&&b==1 && r.state_json.find("b")!=std::string::npos);
}
void safe_retry_budget_and_none(){
    auto store=std::make_shared<Store>();int calls=0;workflow_t read;
    read.add_node(node("read",{"value"},[&](const std::string &,const workflow_context_t&){if(++calls==1){workflow_node_result_t r;r.error="read failed";return r;}return output(R"({"value":1})");},true));
    auto o=options(store);assert(read.run("{}",o).status==run_status_t::callback_error);auto r=read.run("{}",o);assert(r.status==run_status_t::completed && calls==2 && r.node_calls==2);
    auto limited=std::make_shared<Store>();int attempts=0;workflow_t failing;failing.add_node(node("read",{},[&](const std::string &,const workflow_context_t&){++attempts;workflow_node_result_t r;r.error="fail";return r;},true));
    o=options(limited);o.max_node_calls=2;assert(failing.run("{}",o).status==run_status_t::callback_error);o.max_node_calls=1;assert(failing.run("{}",o).status==run_status_t::budget_exceeded);o.max_node_calls=64;assert(failing.run("{}",o).status==run_status_t::budget_exceeded && attempts==1);
    auto none=std::make_shared<Store>();int writes=0;workflow_t writer;writer.add_node(node("write",{},[&](const std::string &,const workflow_context_t&){if(++writes==1){workflow_node_result_t r;r.error="operation was not sent";return r;}return output();}));
    o=options(none);r=writer.run("{}",o);assert(r.status==run_status_t::recovery_required);writer.confirm_node(none,"job","{}",r.journal_revision,"write","none",output(),"host-none/1");assert(writer.run("{}",o).status==run_status_t::completed && writes==2);
}
void storage_and_corruption(){
    auto store=std::make_shared<Store>();int effects=0;workflow_t graph;
    graph.add_node(node("write",{},[&](const std::string &,const workflow_context_t&){++effects;store->fail_after=0;return output();}));
    auto o=options(store);auto r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required && !r.progress_available && effects==1 && !store->leased);
    store->fail_after=-1;r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required && effects==1);
    store->state.state_json=R"({"version":1,"kind":"libai.workflow"})";r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required && !r.progress_available && effects==1 && !store->leased);
}
void persistent_deadline_and_attempt_corruption(){
    auto store=std::make_shared<Store>();int calls=0;workflow_t graph;graph.add_node(node("read",{},[&](const std::string &,const workflow_context_t&){++calls;workflow_node_result_t r;r.error="temporary";return r;},true));
    auto o=options(store);o.timeout=std::chrono::milliseconds(1000);assert(graph.run("{}",o).status==run_status_t::callback_error);
    workflow_storage::json::Document d;governance_detail::parse_json(store->state.state_json,d);
    d["started"].SetInt64(workflow_storage::now()-2000);store->state.state_json=workflow_storage::wire(d);
    o.timeout=std::chrono::milliseconds(0);assert(graph.run("{}",o).status==run_status_t::deadline_exceeded&&calls==1);
    governance_detail::parse_json(store->state.state_json,d);d["calls"].SetUint64(0);store->state.state_json=workflow_storage::wire(d);
    auto r=graph.run("{}",o);assert(r.status==run_status_t::recovery_required&&!r.progress_available&&calls==1);
}
#if defined(__unix__) || defined(__APPLE__)
void real_process_exit(){
    char path[]="/tmp/libai-workflow-crash-XXXXXX";assert(mkdtemp(path));std::string root=path;
    pid_t child=fork();assert(child>=0);
    if(!child){auto store=std::make_shared<file_execution_store_t>(root,execution_file_codec_t::plaintext());workflow_t g;
        g.add_node(node("write",{},[&](const std::string &,const workflow_context_t&){std::ofstream effect(root+"/effect");effect<<"once";effect.close();_exit(71);return output();}));g.run("{}",options(store));_exit(72);}
    int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==71);
    auto store=std::make_shared<file_execution_store_t>(root,execution_file_codec_t::plaintext());int repeats=0;workflow_t g;
    g.add_node(node("write",{},[&](const std::string &,const workflow_context_t&){++repeats;return output();}));auto r=g.run("{}",options(store));assert(r.status==run_status_t::recovery_required&&repeats==0);
    std::ifstream effect(root+"/effect");std::string value;effect>>value;assert(value=="once");g.confirm_node(store,"job","{}",r.journal_revision,"write","applied",output(),"verified-effect-file/1");assert(g.run("{}",options(store)).status==run_status_t::completed&&repeats==0);
    store.reset();unlink((root+"/effect").c_str());unlink((root+"/wfg3_job.state").c_str());unlink((root+"/wfg3_job.lock").c_str());assert(rmdir(root.c_str())==0);
}
#endif
int main(){branch_cache_and_freeze();partial_parallel_and_receipt();safe_retry_budget_and_none();storage_and_corruption();persistent_deadline_and_attempt_corruption();
#if defined(__unix__) || defined(__APPLE__)
real_process_exit();
#endif
}
