#include <libai/agent.h>
#include <cassert>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <libai/sdk/file_execution_store.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
using namespace libai;
namespace json=governance_detail::json;
struct Store:execution_store_t {
 execution_snapshot_t snapshot;bool leased=false;int sequence=0;bool slow_intent=false,slow_model=false;
 execution_snapshot_t open(const std::string&)override{assert(!leased);leased=true;return snapshot;}
 std::string commit(const std::string&,const std::string &revision,const std::string &wire)override{
  assert(leased&&revision==snapshot.revision);snapshot.state_json=wire;snapshot.revision=std::to_string(++sequence);
  if((slow_intent&&wire.find("\"state\":\"running\"")!=std::string::npos)||(slow_model&&wire.find("\"model_calls\":1")!=std::string::npos)){slow_intent=slow_model=false;std::this_thread::sleep_for(std::chrono::milliseconds(70));}
  return snapshot.revision;
 }
 void close(const std::string&)noexcept override{leased=false;}
 void edit(std::function<void(json::Document&)> fn){json::Document d;governance_detail::parse_json(snapshot.state_json,d);fn(d);json::StringBuffer b;json::Writer<json::StringBuffer>w(b);d.Accept(w);snapshot.state_json={b.GetString(),b.GetSize()};}
};
static run_options_t options(std::shared_ptr<Store>s){run_options_t o;o.execution_store=s;o.execution_scope="task";o.timeout=std::chrono::milliseconds(1000);return o;}
static std::shared_ptr<model_t> failing(int &calls){return std::make_shared<function_model_t>([&](const model_request_t&,const stream_sink_t&)->model_response_t{++calls;throw std::runtime_error("model interrupted");});}
static void unfinished(std::shared_ptr<Store>s,int &calls,run_options_t o){agent_t a(failing(calls));agent_session_t session;assert(a.run(session,"input",o).status==run_status_t::callback_error);}
static void process_deadline(){
#if defined(__unix__) || defined(__APPLE__)
 char path[]="/tmp/libai-deadline-XXXXXX";assert(mkdtemp(path));std::string root=path;
 pid_t child=fork();assert(child>=0);
 if(!child){
  auto store=std::make_shared<file_execution_store_t>(root,execution_file_codec_t::plaintext());
  agent_t agent(std::make_shared<function_model_t>([](const model_request_t&,const stream_sink_t&)->model_response_t{_exit(77);}));
  run_options_t o;o.execution_store=store;o.execution_scope="restart";o.timeout=std::chrono::milliseconds(1000);
  agent_session_t session;agent.run(session,"input",o);_exit(78);
 }
 int status=0;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==77);
 auto store=std::make_shared<file_execution_store_t>(root,execution_file_codec_t::plaintext());
 auto snapshot=store->open("restart");store->close("restart");json::Document d;governance_detail::parse_json(snapshot.state_json,d);
 auto end=d["time_budget"]["started_unix_ms"].GetInt64()+d["time_budget"]["timeout_ms"].GetInt64();
 auto wait=end-sdk_detail::execution_wall_ms()+20;if(wait>0)std::this_thread::sleep_for(std::chrono::milliseconds(wait));
 // Fresh process and owner, no fabricated timestamps and no parent Agent state.
 child=fork();assert(child>=0);
 if(!child){
  auto resumed=std::make_shared<file_execution_store_t>(root,execution_file_codec_t::plaintext());
  agent_t agent(std::make_shared<function_model_t>([](const model_request_t&,const stream_sink_t&)->model_response_t{_exit(79);}));
  run_options_t o;o.execution_store=resumed;o.execution_scope="restart";o.timeout=std::chrono::milliseconds(0);
  agent_session_t session;_exit(agent.run(session,"input",o).status==run_status_t::deadline_exceeded?0:80);
 }
 assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 assert(unlink((root+"/restart.state").c_str())==0);assert(unlink((root+"/restart.lock").c_str())==0);assert(rmdir(root.c_str())==0);
#endif
}
int main(){
 process_deadline();
 auto s=std::make_shared<Store>();int calls=0;auto o=options(s);unfinished(s,calls,o);assert(calls==1);
 s->edit([](json::Document &d){auto &t=d["time_budget"];auto now=sdk_detail::execution_wall_ms();t["started_unix_ms"].SetInt64(now-2000);t["last_seen_unix_ms"].SetInt64(now-2000);});
 o.timeout=std::chrono::milliseconds(5000);agent_t a(failing(calls));agent_session_t session;assert(a.run(session,"input",o).status==run_status_t::deadline_exceeded&&calls==1);o.timeout=std::chrono::milliseconds(0);assert(a.run(session,"input",o).status==run_status_t::deadline_exceeded&&calls==1);
 // Tightening is relative to original start, never now + requested duration.
 s=std::make_shared<Store>();calls=0;o=options(s);unfinished(s,calls,o);s->edit([](json::Document&d){auto now=sdk_detail::execution_wall_ms();d["time_budget"]["started_unix_ms"].SetInt64(now-100);d["time_budget"]["last_seen_unix_ms"].SetInt64(now-100);});o.timeout=std::chrono::milliseconds(50);agent_t tightened(failing(calls));assert(tightened.run(session,"input",o).status==run_status_t::deadline_exceeded&&calls==1);o.timeout=std::chrono::milliseconds(1000);assert(tightened.run(session,"input",o).status==run_status_t::deadline_exceeded&&calls==1);
 // Corrupt/backwards clocks fail closed before model callbacks.
 for(int mode=0;mode<3;++mode){s=std::make_shared<Store>();calls=0;o=options(s);unfinished(s,calls,o);s->edit([mode](json::Document&d){if(mode==0)d["time_budget"]["last_seen_unix_ms"].SetInt64(sdk_detail::execution_wall_ms()+10000);else if(mode==1)d["time_budget"]["timeout_ms"].SetInt64(-1);else d["time_budget"]["origin"].SetString("model_assertion",d.GetAllocator());});agent_t b(failing(calls));assert(b.run(session,"input",o).status==run_status_t::recovery_required&&calls==1);}
 // Legacy unfinished state cannot silently acquire a fresh timeout.
 s=std::make_shared<Store>();calls=0;o=options(s);unfinished(s,calls,o);s->edit([](json::Document&d){d.RemoveMember("time_budget");});agent_t legacy(failing(calls));assert(legacy.run(session,"input",o).status==run_status_t::recovery_required&&calls==1);bool rejected=false;try{adopt_execution_deadline(s,"task","input","","stale",std::chrono::milliseconds(1000),"host-audit");}catch(...){rejected=true;}assert(rejected);adopt_execution_deadline(s,"task","input","",s->snapshot.revision,std::chrono::milliseconds(1000),"host-original-budget-audit");assert(legacy.run(session,"input",o).status==run_status_t::callback_error&&calls==2);
 // Zero is consistently unlimited for SDK and language bridges.
 auto final_model=std::make_shared<function_model_t>([&](const model_request_t &request,const stream_sink_t&){++calls;assert(request.deadline==std::chrono::steady_clock::time_point::max());model_response_t r;r.text="done";return r;});agent_t unlimited(final_model);run_options_t zero;zero.timeout=std::chrono::milliseconds(0);assert(unlimited.run(session,"input",zero).status==run_status_t::completed);
 // Completed results remain reusable after expiration, with no new model work.
 s=std::make_shared<Store>();o=options(s);auto done=std::make_shared<function_model_t>([&](const model_request_t&,const stream_sink_t&){++calls;model_response_t r;r.text="done";return r;});agent_t completed(done);assert(completed.run(session,"input",o).status==run_status_t::completed);int before=calls;s->edit([](json::Document&d){auto now=sdk_detail::execution_wall_ms();d["time_budget"]["started_unix_ms"].SetInt64(now-2000);d["time_budget"]["last_seen_unix_ms"].SetInt64(now-2000);});assert(completed.run(session,"input",o).status==run_status_t::completed&&calls==before);
 // Storage latency cannot launch a model or mutating tool after deadline.
 s=std::make_shared<Store>();o=options(s);o.timeout=std::chrono::milliseconds(40);s->slow_model=true;before=calls;agent_t slow(done);assert(slow.run(session,"input",o).status==run_status_t::deadline_exceeded&&calls==before);
 s=std::make_shared<Store>();o=options(s);o.timeout=std::chrono::milliseconds(40);s->slow_intent=true;int effects=0;auto tool_model=std::make_shared<function_model_t>([](const model_request_t&,const stream_sink_t&){model_response_t r;tool_call_t c;c.id="a";c.name="write";r.calls={c};return r;});agent_t writer(tool_model);tool_spec_t spec;spec.name="write";writer.add_tool(spec,[&](const tool_call_t&,const tool_context_t&){++effects;return tool_result_t::success("effect");});assert(writer.run(session,"input",o).status==run_status_t::deadline_exceeded&&effects==0);assert(s->snapshot.state_json.find("deadline/cancelled before handler dispatch")!=std::string::npos);
}
