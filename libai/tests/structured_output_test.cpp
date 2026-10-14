#include <libai/agent.h>
#include <cassert>
using namespace libai;
const std::string schema=R"({"type":"object","required":["n"],"properties":{"n":{"type":"integer"}},"additionalProperties":false})";
struct Store:execution_store_t {
    execution_snapshot_t snapshot;bool leased=false,fail=false;unsigned revision=0;
    execution_snapshot_t open(const std::string &) override {if(leased)throw std::runtime_error("busy");leased=true;return snapshot;}
    std::string commit(const std::string &,const std::string &expected,const std::string &state) override {
        if(!leased || fail || snapshot.revision!=expected)throw std::runtime_error("commit failed");
        snapshot.state_json=state;snapshot.revision=std::to_string(++revision);return snapshot.revision;
    }
    void close(const std::string &) noexcept override {leased=false;}
};
static tool_call_t charge(const std::string &id="charge") {tool_call_t c;c.id=id;c.name="charge";return c;}
static void register_charge(agent_t &a,int &effects) {tool_spec_t t;t.name="charge";a.add_tool(t,[&](const tool_call_t &,const tool_context_t &){++effects;return tool_result_t::success("receipt");});}
int main() {
    int calls=0,effects=0;std::string visible;
    agent_t a(std::make_shared<function_model_t>([&](const model_request_t &q,const stream_sink_t &sink){
        assert(q.output_schema==schema);model_response_t r;
        if(calls++==0){assert(!q.output_repair);r.calls.push_back(charge());assert(sink("planning text"));}
        else if(calls==2){r.text=R"({"n":"bad"})";assert(sink(r.text));}
        else {assert(q.output_repair && q.tools.empty());r.text=R"({"n":2})";assert(sink(r.text));}
        r.output_tokens=1;return r;
    }));a.configure_output(schema,1);register_charge(a,effects);
    bool invalid=false;try{a.configure_output(R"({"const":1})",1);}catch(const std::exception&){invalid=true;}assert(invalid);
    agent_session_t session;run_options_t options;options.on_text=[&](const std::string &s){visible+=s;return true;};
    auto result=a.run(session,"work",options);
    assert(result.status==run_status_t::completed && result.output_validated && result.output_repairs==1);
    assert(calls==3 && effects==1 && result.model_calls==3 && result.tool_calls==1 && result.output_tokens==3);
    assert(visible==R"({"n":2})" && !result.task_contract.accepted());
    // Exhaustion is durable, not reset by constructing another run/Agent.
    auto failed=std::make_shared<Store>();run_options_t durable;durable.execution_store=failed;durable.execution_scope="failed";
    int bad_calls=0;
    agent_t bad(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++bad_calls;model_response_t r;r.text="not JSON";return r;}));bad.configure_output(schema,0);
    agent_session_t bad_session;assert(bad.run(bad_session,"bad",durable).status==run_status_t::protocol_error);
    assert(bad.run(bad_session,"bad",durable).status==run_status_t::protocol_error && bad_calls==1);
    bad.configure_output(schema,1);assert(bad.run(bad_session,"bad",durable).status==run_status_t::recovery_required && bad_calls==1);
    // Output repair cannot re-plan effects, even if the transport ignores the empty catalog.
    int forbidden_effects=0,repair_calls=0;auto forbidden=std::make_shared<Store>();durable.execution_store=forbidden;durable.execution_scope="forbidden";
    agent_t evil(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){model_response_t r;if(repair_calls++==0)r.text="{}";else r.calls.push_back(charge());return r;}));
    evil.configure_output(schema,1);register_charge(evil,forbidden_effects);agent_session_t evil_session;
    assert(evil.run(evil_session,"work",durable).status==run_status_t::protocol_error && forbidden_effects==0);
    assert(evil.run(evil_session,"work",durable).status==run_status_t::protocol_error && repair_calls==2);
    // A failed transport in correction retains the original feedback/counters and no tools.
    auto store=std::make_shared<Store>();durable.execution_store=store;durable.execution_scope="resume";int transport_calls=0;
    agent_t first(std::make_shared<function_model_t>([&](const model_request_t &q,const stream_sink_t &){model_response_t r;if(transport_calls++==0)r.text="{}";else {assert(q.output_repair && q.tools.empty());r.error="offline";}return r;}));
    first.configure_output(schema,1);agent_session_t original;
    assert(first.run(original,"work",durable).status==run_status_t::model_error && transport_calls==2);
    agent_t resumed(std::make_shared<function_model_t>([&](const model_request_t &q,const stream_sink_t &){++transport_calls;assert(q.output_repair && q.tools.empty());model_response_t r;r.text=R"({"n":4})";return r;}));
    resumed.configure_output(schema,1);agent_session_t fresh;result=resumed.run(fresh,"work",durable);
    assert(result.status==run_status_t::completed && result.output_repairs==1 && result.output_validated && result.model_calls==3);
    assert(resumed.run(fresh,"work",durable).output_validated && transport_calls==3);
    auto valid_snapshot=store->snapshot;
    // A structurally consistent but invalid cached final JSON must be rejected.
    governance_detail::json::Document doc;governance_detail::parse_json(store->snapshot.state_json,doc);
    doc["text"].SetString(R"({"n":"bad"})",doc.GetAllocator());doc["messages"].GetArray().End()[-1]["text"].SetString(R"({"n":"bad"})",doc.GetAllocator());
    governance_detail::json::StringBuffer buffer;governance_detail::json::Writer<governance_detail::json::StringBuffer> writer(buffer);doc.Accept(writer);
    store->snapshot.state_json=buffer.GetString();assert(resumed.run(fresh,"work",durable).status==run_status_t::recovery_required && transport_calls==3);
    store->snapshot=valid_snapshot;
    // Host side-effect confirmation preserves the frozen output contract.
    auto receipts=std::make_shared<Store>();durable.execution_store=receipts;durable.execution_scope="receipt";int paid=0,model_calls=0;
    agent_t payer(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++model_calls;model_response_t r;r.calls.push_back(charge());return r;}));payer.configure_output(schema,1);
    tool_spec_t tool;tool.name="charge";payer.add_tool(tool,[&](const tool_call_t &,const tool_context_t &){++paid;receipts->fail=true;return tool_result_t::success("receipt");});agent_session_t pay_session;
    assert(payer.run(pay_session,"pay",durable).status==run_status_t::recovery_required && paid==1);receipts->fail=false;
    confirm_execution_effect(receipts,"receipt","pay","",receipts->snapshot.revision,"receipt.0",tool_result_t::success("receipt"),"applied","host receipt");
    agent_t finish(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++model_calls;model_response_t r;r.text=R"({"n":5})";return r;}));finish.configure_output(schema,1);register_charge(finish,paid);agent_session_t finish_session;
    assert(finish.run(finish_session,"pay",durable).output_validated && paid==1 && model_calls==2);
    // Original global budgets still count correction requests.
    int budget_calls=0;agent_t budget(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++budget_calls;model_response_t r;r.text="{}";return r;}));budget.configure_output(schema,1);
    run_options_t one;one.max_model_calls=1;agent_session_t limited;assert(budget.run(limited,"work",one).status==run_status_t::budget_exceeded && budget_calls==1);
    for(const std::string &text:{"```json\n{\"n\":1}\n```", "{\"n\":1}{\"n\":2}", "{\"n\":1,\"n\":2}"}) {
        agent_t strict(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){model_response_t r;r.text=text;return r;}));strict.configure_output(schema,0);agent_session_t s;assert(strict.run(s,"strict").status==run_status_t::protocol_error);
    }
}
