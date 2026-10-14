#include <libai/agent.h>
#include <cassert>
using namespace libai;
struct Store:execution_store_t {
    execution_snapshot_t snapshot;bool leased=false,fail_commit=false;unsigned revision=0;
    execution_snapshot_t open(const std::string &) override {if(leased)throw std::runtime_error("busy");leased=true;return snapshot;}
    std::string commit(const std::string &,const std::string &expected,const std::string &state) override {
        if(!leased || expected!=snapshot.revision || fail_commit)throw std::runtime_error("commit failed");
        snapshot.state_json=state;snapshot.revision=std::to_string(++revision);return snapshot.revision;
    }
    void close(const std::string &) noexcept override {leased=false;}
};
static std::shared_ptr<model_t> model(int &calls) {
    return std::make_shared<function_model_t>([&](const model_request_t &request,const stream_sink_t &){
        ++calls;model_response_t r;
        if(request.messages.back().role==message_t::role_t::user) {
            tool_call_t c;c.id="call";c.name="charge";r.calls.push_back(c);
        }else r.text="done";
        return r;
    });
}
int main() {
    auto observed=std::make_shared<Store>();run_options_t observed_options;
    observed_options.execution_store=observed;observed_options.execution_scope="observed";
    std::vector<agent_event_t> events;int observed_models=0,observed_effects=0;
    agent_t observed_agent(model(observed_models));tool_spec_t observed_spec;observed_spec.name="charge";
    observed_agent.add_tool(observed_spec,[&](const tool_call_t &,const tool_context_t &c){
        ++observed_effects;assert(c.idempotency_key=="observed.0");
        assert(events.back().kind==agent_event_t::kind_t::tool_intent_committed);
        assert(events.back().effect_state=="running"&&events.back().attempts==1);
        return tool_result_t::success("receipt");
    });
    observed_options.on_event=[&](const agent_event_t &event){
        assert(event.execution_scope=="observed");events.push_back(event);
        if(event.kind==agent_event_t::kind_t::tool_intent_committed||event.kind==agent_event_t::kind_t::tool_result_committed||event.kind==agent_event_t::kind_t::tool_result_reused){
            assert(event.idempotency_key=="observed.0"&&event.journal_revision==observed->snapshot.revision);
            governance_detail::json::Document state;governance_detail::parse_json(observed->snapshot.state_json,state);
            assert(event.effect_state==state["effects"][0]["state"].GetString());
            // Presentation failures cannot change already committed journal facts.
            if(event.kind==agent_event_t::kind_t::tool_result_committed)throw std::runtime_error("observer failed");
        }
    };
    agent_session_t observed_session;assert(observed_agent.run(observed_session,"pay",observed_options).status==run_status_t::completed);
    assert(observed_models==2&&observed_effects==1);
    events.clear();assert(observed_agent.run(observed_session,"pay",observed_options).status==run_status_t::completed);
    assert(events.size()==2&&events[0].kind==agent_event_t::kind_t::tool_result_reused&&events[1].kind==agent_event_t::kind_t::finished);
    assert(observed_models==2&&observed_effects==1);
    for(bool denied:{false,true}) {
        auto limited_store=std::make_shared<Store>();run_options_t limits;
        limits.execution_store=limited_store;limits.execution_scope=denied?"denied":"truncated";limits.max_tool_result_bytes=2;
        int local_models=0,local_tools=0;agent_t local(model(local_models));tool_spec_t spec;spec.name="charge";spec.requires_approval=denied;
        local.add_tool(spec,[&](const tool_call_t &,const tool_context_t &){++local_tools;return tool_result_t::success("long result");});
        std::vector<agent_event_t> local_events;limits.on_event=[&](const agent_event_t &event){local_events.push_back(event);};
        agent_session_t current;assert(local.run(current,"pay",limits).status==run_status_t::completed);
        bool committed=false;unsigned intents=0;
        for(const auto &event:local_events){
            if(event.kind==agent_event_t::kind_t::tool_intent_committed)++intents;
            if(event.kind==agent_event_t::kind_t::tool_result_committed){committed=true;assert(event.error&&event.attempts==(denied?0u:1u));}
        }
        assert(committed&&intents==(denied?0u:1u)&&local_tools==(denied?0:1));
        local_events.clear();assert(local.run(current,"pay",limits).status==run_status_t::completed);
        assert(local_events[0].kind==agent_event_t::kind_t::tool_result_reused&&local_events[0].error);
    }
    {
        auto cancelled_store=std::make_shared<Store>();run_options_t limits;limits.execution_store=cancelled_store;
        limits.execution_scope="cancelled";limits.cancellation=std::make_shared<cancellation_t>();
        int local_models=0,local_tools=0;agent_t local(model(local_models));tool_spec_t spec;spec.name="charge";
        local.add_tool(spec,[&](const tool_call_t &,const tool_context_t &){++local_tools;return tool_result_t::success("unexpected");});
        limits.on_event=[&](const agent_event_t &event){if(event.kind==agent_event_t::kind_t::tool_intent_committed)limits.cancellation->cancel();};
        agent_session_t current;assert(local.run(current,"pay",limits).status==run_status_t::cancelled&&local_tools==0);
    }
    auto store=std::make_shared<Store>();run_options_t o;o.execution_store=store;o.execution_scope="task";
    int calls=0,effects=0;agent_t agent(model(calls));tool_spec_t t;t.name="charge";
    agent.add_tool(t,[&](const tool_call_t &,const tool_context_t &c){assert(c.idempotency_key=="task.0");++effects;store->fail_commit=true;return tool_result_t::success("receipt");});
    agent_session_t s;auto first=agent.run(s,"pay",o);
    assert(first.status==run_status_t::recovery_required && effects==1 && calls==1 && !store->leased);
    store->fail_commit=false;agent_t resumed(model(calls));resumed.add_tool(t,[&](const tool_call_t &,const tool_context_t &){++effects;return tool_result_t::success("duplicate");});
    agent_session_t fresh;auto blocked=resumed.run(fresh,"pay",o);
    assert(blocked.status==run_status_t::recovery_required && effects==1 && calls==1);
    bool stale=false;try{confirm_execution_effect(store,"task","pay","","stale","task.0",tool_result_t::success("receipt"),"applied","host-receipt");}catch(const std::exception&){stale=true;}assert(stale);
    confirm_execution_effect(store,"task","pay","",store->snapshot.revision,"task.0",tool_result_t::success("receipt"),"applied","host-receipt");
    auto completed=resumed.run(fresh,"pay",o);assert(completed.status==run_status_t::completed && effects==1 && calls==2);
    auto cached=resumed.run(fresh,"pay",o);assert(cached.status==run_status_t::completed && cached.text=="done" && effects==1 && calls==2);
    auto wrong=resumed.run(fresh,"different task",o);assert(wrong.status==run_status_t::recovery_required && effects==1);
    store->snapshot.state_json="broken";assert(resumed.run(fresh,"pay",o).status==run_status_t::recovery_required && effects==1);
    auto partial=std::make_shared<Store>();run_options_t batch;o.execution_store=partial;o.execution_scope="batch";
    int generated=0,first_effect=0,second_effect=0;
    agent_t two(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){
        ++generated;model_response_t r;tool_call_t c;c.id="one";c.name="first";r.calls.push_back(c);c.id="two";c.name="second";r.calls.push_back(c);return r;
    }));
    tool_spec_t one;one.name="first";tool_spec_t two_spec;two_spec.name="second";
    two.add_tool(one,[&](const tool_call_t &,const tool_context_t &){++first_effect;return tool_result_t::success("one");});
    two.add_tool(two_spec,[&](const tool_call_t &,const tool_context_t &){++second_effect;return tool_result_t::success("two");});
    o.on_event=[&](const agent_event_t &event){if(event.kind==agent_event_t::kind_t::tool_end && event.tool_name=="first")partial->fail_commit=true;};
    agent_session_t partial_session;assert(two.run(partial_session,"batch",o).status==run_status_t::recovery_required);
    assert(first_effect==1 && second_effect==0);partial->fail_commit=false;o.on_event={};
    // The first receipt commit failed, so the authoritative first intent is still unknown.
    confirm_execution_effect(partial,"batch","batch","",partial->snapshot.revision,"batch.0",tool_result_t::success("one"),"applied","first receipt");
    agent_t finish_batch(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++generated;model_response_t r;r.text="batch done";return r;}));
    finish_batch.add_tool(one,[&](const tool_call_t &,const tool_context_t &){++first_effect;return tool_result_t::success("duplicate");});
    finish_batch.add_tool(two_spec,[&](const tool_call_t &,const tool_context_t &c){assert(c.idempotency_key=="batch.1");++second_effect;return tool_result_t::success("two");});
    agent_session_t resumed_batch;
    auto limited=o;limited.max_tool_calls=0;
    assert(finish_batch.run(resumed_batch,"batch",limited).status==run_status_t::budget_exceeded && second_effect==0);
    assert(finish_batch.run(resumed_batch,"batch",o).status==run_status_t::completed && first_effect==1 && second_effect==1 && generated==2);
    // Before-effect commit failure must never execute the handler or contact the model.
    auto failed=std::make_shared<Store>();failed->fail_commit=true;o.execution_store=failed;o.execution_scope="failed";
    agent_session_t empty;assert(resumed.run(empty,"pay",o).status==run_status_t::recovery_required && effects==1 && calls==2);
    auto costs=std::make_shared<Store>();run_options_t cost_options;cost_options.execution_store=costs;cost_options.execution_scope="costs";cost_options.max_output_tokens=10;
    int cost_calls=0;
    agent_t paid(std::make_shared<function_model_t>([&](const model_request_t &,const stream_sink_t &){++cost_calls;model_response_t r;r.error="known model error";r.output_tokens=4;return r;}));
    agent_session_t cost_session;assert(paid.run(cost_session,"cost",cost_options).status==run_status_t::model_error);
    agent_t retry(std::make_shared<function_model_t>([&](const model_request_t &q,const stream_sink_t &){++cost_calls;assert(q.max_output_tokens==6);model_response_t r;r.text="done";return r;}));
    assert(retry.run(cost_session,"cost",cost_options).status==run_status_t::completed && cost_calls==2);

}
