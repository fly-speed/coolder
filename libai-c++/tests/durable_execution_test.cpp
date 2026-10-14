#include <libai_cpp/agent.h>
#include <cassert>
#include <future>
#include <stdexcept>

using namespace libai::cpp;
class Store : public ExecutionStore {
public:
    std::string revision, state;
    bool leased = false, fail_result = true;
    ExecutionSnapshot open(const std::string &) override
    {
        if (leased) throw std::runtime_error("leased");
        ExecutionSnapshot snapshot;
        snapshot.revision = revision; snapshot.state_json = state;
        leased = true;
        return snapshot;
    }
    std::string commit(const std::string &, const std::string &expected,
        const std::string &next) override
    {
        assert(leased);
        if (expected != revision) throw std::runtime_error("CAS conflict");
        if (fail_result && next.find("\"state\":\"done\"") != std::string::npos) {
            fail_result = false;
            throw std::runtime_error("result commit failed");
        }
        state = next;
        revision = std::to_string(std::stoul(revision.empty() ? "0" : revision) + 1);
        return revision;
    }
    void close(const std::string &) noexcept override { leased = false; }
};

int main()
{
    auto store = std::make_shared<Store>();
    int requests = 0;
    std::vector<std::string> effects;
    auto model = [&](const ModelRequest &request, const StreamSink &) {
        ++requests;
        ModelResponse response;
        if (request.messages.back().role == Message::role_t::tool) response.text = "done";
        else {
            ToolCall a; a.id = "a"; a.name = "write";
            ToolCall b = a; b.id = "b";
            response.calls = {a, b};
        }
        return response;
    };
    RunOptions options; options.execution_store = store; options.execution_scope = "task";
    auto run = [&](const std::string &revision, const RunOptions &settings) {
        Agent agent(model, "instructions");
        ToolSpec spec; spec.name = "write"; spec.execution_revision = revision;
        agent.add_tool(spec, [&](const ToolCall &, const ToolContext &context) {
            effects.push_back(context.idempotency_key);
            return ToolResult::success("written");
        });
        bool invalid=false;try{agent.event_execution();}catch(const std::logic_error &){invalid=true;}assert(invalid);
        RunOptions hooked=settings;bool checked_thread=false;
        hooked.on_event=[&](const Event &event){
            const auto execution=agent.event_execution();assert(execution.execution_scope=="task");
            if(!checked_thread){checked_thread=true;
                assert(std::async(std::launch::async,[&]{try{agent.event_execution();return false;}catch(const std::logic_error &){return true;}}).get());
            }
            if(event.kind==Event::kind_t::tool_intent_committed||event.kind==Event::kind_t::tool_result_committed||event.kind==Event::kind_t::tool_result_reused){
                assert(!execution.idempotency_key.empty()&&execution.journal_revision==store->revision&&execution.attempts==1);
                assert(execution.effect_state==(event.kind==Event::kind_t::tool_intent_committed?"running":"done"));
            }
        };
        auto result=agent.run("input", hooked);
        invalid=false;try{agent.event_execution();}catch(const std::logic_error &){invalid=true;}assert(invalid);
        return result;
    };
    assert(run("v1", options).status == RunStatus::recovery_required);
    assert(!store->leased && requests == 1 && effects == std::vector<std::string>{"task.0"});
    assert(store->state.find("{\"time_budget\":{")==0);
    store->state="{"+store->state.substr(store->state.find("},")+2);
    const auto legacy=store->state;
    for(const auto &revision:{std::string("stale"),store->revision}) {
        bool rejected=false;
        try { adopt_execution_deadline(store,"task","input","instructions",revision,
            std::chrono::milliseconds(revision=="stale"?1000:-1),"audit"); }
        catch(const std::exception &){rejected=true;}
        assert(rejected&&!store->leased&&store->state==legacy);
    }
    adopt_execution_deadline(store,"task","input","instructions",store->revision,
        std::chrono::milliseconds(300000),"host-budget-audit");
    assert(store->state.find("host_adoption")!=std::string::npos);
    bool duplicate=false;try { adopt_execution_deadline(store,"task","input","instructions",
        store->revision,std::chrono::milliseconds(300000),"audit"); }catch(const std::exception &){duplicate=true;}
    assert(duplicate&&!store->leased);
    assert(run("v1", options).status == RunStatus::recovery_required);
    assert(requests == 1 && effects.size() == 1);
    bool rejected = false;
    try {
        confirm_execution_effect(store, "task", "input", "instructions", "stale",
            "task.0", ToolResult::success("written"), "applied", "receipt/1");
    } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected && !store->leased);
    confirm_execution_effect(store, "task", "input", "instructions", store->revision,
        "task.0", ToolResult::success("written"), "applied", "receipt/1");
    // Changed handler semantics must not replay the frozen remaining batch.
    assert(run("v2", options).status == RunStatus::recovery_required);
    assert(requests == 1 && effects.size() == 1 && !store->leased);
    RunOptions reduced = options; reduced.max_tool_calls = 0;
    assert(run("v1", reduced).status == RunStatus::budget_exceeded);
    assert(effects.size() == 1 && requests == 1);
    assert(run("v1", options).status == RunStatus::completed);
    assert(effects == (std::vector<std::string>{"task.0", "task.1"}) && requests == 2);
    assert(run("v1", options).text == "done");
    assert(effects.size() == 2 && requests == 2 && !store->leased);
    // Another owner's failed lease acquisition must not release its lease.
    store->leased = true;
    assert(run("v1", options).status == RunStatus::recovery_required);
    assert(store->leased && requests == 2);
    auto reads = std::make_shared<Store>();
    RunOptions read_options; read_options.execution_store = reads; read_options.execution_scope = "read";
    int read_count = 0;
    auto run_read = [&]() {
        Agent agent([](const ModelRequest &request, const StreamSink &) {
            assert(request.tools.at(0).side_effect_free);
            ModelResponse result;
            if(request.messages.back().role == Message::role_t::tool) result.text = "handled";
            else { ToolCall call;call.id="r";call.name="read";result.calls={call}; }
            return result;
        });
        ToolSpec read;read.name="read";read.side_effect_free=true;
        agent.add_tool(read,[&](const ToolCall &,const ToolContext &){++read_count;return ToolResult::failure("not found");});
        return agent.run("input",read_options);
    };
    assert(run_read().status==RunStatus::recovery_required && read_count==1);
    auto replay=run_read();assert(replay.status==RunStatus::completed && replay.tool_calls==2 && read_count==2);
    return 0;
}
