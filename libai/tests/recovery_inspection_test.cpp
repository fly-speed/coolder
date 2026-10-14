#include <libai/agent.h>
#include <libai/sdk/governance.h>
#include <cassert>
using namespace libai;
namespace json=governance_detail::json;
struct Store:execution_store_t {
    execution_snapshot_t snapshot;unsigned commits=0;bool fail=false;
    execution_snapshot_t open(const std::string &) override{return snapshot;}
    std::string commit(const std::string &,const std::string &revision,const std::string &state) override {
        assert(revision==snapshot.revision);if(fail)throw std::runtime_error("lost receipt");
        snapshot.state_json=state;return snapshot.revision=std::to_string(++commits);
    }
    void close(const std::string &) noexcept override{}
};
static std::string wire(const execution_snapshot_t &s,const std::string &input="pay"){
    json::StringBuffer b;json::Writer<json::StringBuffer>w(b);w.StartObject();
    auto field=[&](const char*k,const std::string&v){w.Key(k);w.String(v.data(),static_cast<json::SizeType>(v.size()));};
    field("scope","task");field("input",input);field("instructions","");field("revision",s.revision);field("state_json",s.state_json);
    w.EndObject();return std::string(b.GetString(),b.GetSize());
}
static json::Document inspect(const execution_snapshot_t &s,const std::string &input="pay"){
    json::Document d;governance_detail::parse_json(governance_call("executions.inspect",wire(s,input)),d);return d;
}
int main(){
    auto store=std::make_shared<Store>();int models=0,effects=0;
    agent_t agent(std::make_shared<function_model_t>([&](const model_request_t&q,const stream_sink_t&){
        ++models;model_response_t r;if(q.messages.back().role==message_t::role_t::user){tool_call_t c;c.id="one";c.name="charge";r.calls.push_back(c);}else r.text="done";return r;
    }));
    tool_spec_t spec;spec.name="charge";agent.add_tool(spec,[&](const tool_call_t&,const tool_context_t&){++effects;store->fail=true;return tool_result_t::success("receipt");});
    run_options_t options;options.execution_store=store;options.execution_scope="task";agent_session_t session;
    assert(agent.run(session,"pay",options).status==run_status_t::recovery_required);
    const auto original=store->snapshot.state_json;const auto count=store->commits;
    auto d=inspect(store->snapshot);assert(d["ok"].GetBool());assert(std::string(d["recovery"].GetString())=="host_reconciliation");
    assert(std::string(d["effects"][0]["key"].GetString())=="task.0");
    assert(std::string(d["effects"][0]["resume_policy"].GetString())=="host_receipt_required");
    assert(!inspect(store->snapshot,"different")["ok"].GetBool());
    assert(store->snapshot.state_json==original&&store->commits==count&&models==1&&effects==1);
    // Clock diagnostics remain readable even when normal execution must stop.
    json::Document state;governance_detail::parse_json(original,state);
    auto &time=state["time_budget"];auto future=static_cast<std::uint64_t>(sdk_detail::execution_wall_ms()+60000);
    time["started_unix_ms"].SetUint64(future);time["last_seen_unix_ms"].SetUint64(future);
    json::StringBuffer b;json::Writer<json::StringBuffer>w(b);state.Accept(w);execution_snapshot_t future_state=store->snapshot;future_state.state_json=b.GetString();
    auto clock=inspect(future_state);assert(clock["ok"].GetBool()&&clock["clock_moved_backwards"].GetBool());
    assert(std::string(clock["recovery"].GetString())=="host_reconciliation");
    state["effects"][0]["side_effect_free"].SetBool(true);
    b.Clear();json::Writer<json::StringBuffer>safe_writer(b);state.Accept(safe_writer);future_state.state_json=b.GetString();
    assert(std::string(inspect(future_state)["recovery"].GetString())=="clock_reconciliation");
    auto past=static_cast<std::uint64_t>(sdk_detail::execution_wall_ms()-10000);
    time["started_unix_ms"].SetUint64(past);time["last_seen_unix_ms"].SetUint64(past);time["timeout_ms"].SetUint64(1);
    b.Clear();json::Writer<json::StringBuffer>expired_writer(b);state.Accept(expired_writer);future_state.state_json=b.GetString();
    auto expired=inspect(future_state);assert(expired["deadline_expired"].GetBool());
    assert(std::string(expired["recovery"].GetString())=="deadline_exceeded");
    state.RemoveMember("time_budget");
    b.Clear();json::Writer<json::StringBuffer>legacy_writer(b);state.Accept(legacy_writer);auto legacy=store->snapshot;legacy.state_json=b.GetString();
    auto old=inspect(legacy);assert(old["ok"].GetBool()&&!old["time_budget_present"].GetBool());
    assert(std::string(old["recovery"].GetString())=="deadline_adoption");
    assert(std::string(old["effects"][0]["resume_policy"].GetString())=="validate_and_authorize_replay");
    store->fail=false;confirm_execution_effect(store,"task","pay","",store->snapshot.revision,"task.0",tool_result_t::success("receipt"),"applied","bank receipt");
    assert(agent.run(session,"pay",options).status==run_status_t::completed);
    auto final=inspect(store->snapshot);assert(final["ok"].GetBool());assert(std::string(final["recovery"].GetString())=="cached_result");
    assert(std::string(final["effects"][0]["resume_policy"].GetString())=="reuse_result");assert(effects==1&&models==2);
    execution_snapshot_t empty;auto fresh=inspect(empty);assert(fresh["ok"].GetBool()&&!fresh["has_checkpoint"].GetBool());
    empty.revision="unexpected";assert(!inspect(empty)["ok"].GetBool());
    empty.state_json="{}";assert(!inspect(empty)["ok"].GetBool());
    assert(governance_call("executions.inspect",R"({"scope":"task","scope":"task"})").find("\"ok\":false")!=std::string::npos);
}
