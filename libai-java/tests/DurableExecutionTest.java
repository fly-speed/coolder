import io.coolder.libai.Agent;
import io.coolder.libai.Execution;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class DurableExecutionTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static final class Store implements Execution.Store {
        long revision;
        String state = "";
        boolean leased, failResult = true;
        public Execution.Snapshot open(String scope) {
            if (leased) throw new IllegalStateException("leased");
            leased = true;
            return new Execution.Snapshot(revision == 0 ? "" : Long.toString(revision), state);
        }
        public String commit(String scope, String expected, String next) {
            check(leased);
            if (!expected.equals(revision == 0 ? "" : Long.toString(revision))) throw new IllegalStateException("CAS");
            if (failResult && next.contains("\"state\":\"done\"")) {
                failResult = false; throw new IllegalStateException("injected commit failure");
            }
            state = next;
            return Long.toString(++revision);
        }
        public void close(String scope) { leased = false; }
    }
    static final Store store = new Store();
    static int requests;
    static final List<String> events = new ArrayList<>();
    static final List<String> effects = new ArrayList<>();
    static Agent.Invocation retained;
    static String run(String version) {
        try (Agent agent = new Agent((request, invocation) -> {
            requests++;
            if (request.contains("\"role\":\"tool\"")) return new Agent.Response("done");
            Agent.Response response = new Agent.Response();
            response.calls.add(new Agent.ToolCall("a", "write", "{}"));
            response.calls.add(new Agent.ToolCall("b", "write", "{}"));
            return response;
        })) {
            agent.addTool("write", "", "{}", false, version, (call, invocation) -> {
                retained = invocation; effects.add(invocation.idempotencyKey());
                return new Agent.ToolResult("written🌏");
            });
            agent.setExecutionStore("task", store);
            Agent.Hooks hooks=new Agent.Hooks();hooks.onEvent=events::add;
            return agent.run("input",new Agent.Options(),hooks);
        }
    }
    static void confirm(String revision) {
        Execution.confirm(store, "task", "input", "", revision, "task.0",
                new Agent.ToolResult("written🌏"), "applied", "receipt/1");
    }
    static int reads, readRequests;
    static String read(Store store, boolean policy, long budget) {
        try (Agent agent = new Agent((request, invocation) -> {
            readRequests++;
            if (request.contains("\"role\":\"tool\"")) return new Agent.Response("handled");
            Agent.Response response = new Agent.Response();
            response.calls.add(new Agent.ToolCall("r", "read", "{}")); return response;
        })) {
            agent.addTool("read", "", "{}", false, "1", policy, (call, invocation) -> {
                reads++; check(invocation.idempotencyKey().equals("read.0"));
                return new Agent.ToolResult("not found", true);
            });
            agent.setExecutionStore("read", store);
            Agent.Options options = new Agent.Options(); options.maxToolCalls = budget;
            return agent.run("input", options, new Agent.Hooks());
        }
    }
    public static void main(String[] args) {
        check(run("1").contains("recovery_required"));
        check(!store.leased && requests == 1 && effects.equals(Arrays.asList("task.0")));
        check(events.stream().anyMatch(e->e.contains("tool_intent_committed")&&e.contains("\"idempotency_key\":\"task.0\"")&&e.contains("\"effect_state\":\"running\"")));
        check(events.stream().noneMatch(e->e.contains("tool_result_committed")));
        check(store.state.startsWith("{\"time_budget\":{"));
        store.state="{"+store.state.substring(store.state.indexOf("},")+2);
        String legacy=store.state;
        for (long budget : new long[]{-1,Long.MAX_VALUE}) {
            try { Execution.adoptDeadline(store,"task","input","",Long.toString(store.revision),budget,"audit");
                throw new AssertionError(); } catch (IllegalStateException | IllegalArgumentException expected) { }
            check(!store.leased&&store.state.equals(legacy));
        }
        try { Execution.adoptDeadline(store,"task","input","","stale",1000,"audit");
            throw new AssertionError(); } catch (IllegalStateException expected) { }
        check(!store.leased&&store.state.equals(legacy));
        Execution.adoptDeadline(store,"task","input","",Long.toString(store.revision),300000,"host-budget-audit");
        check(store.state.contains("host_adoption"));
        try { Execution.adoptDeadline(store,"task","input","",Long.toString(store.revision),300000,"audit");
            throw new AssertionError(); } catch (IllegalStateException expected) { }
        try { retained.idempotencyKey(); throw new AssertionError(); } catch (IllegalStateException expected) { }
        check(run("1").contains("recovery_required") && requests == 1);
        try { confirm("stale"); throw new AssertionError(); } catch (IllegalStateException expected) { }
        check(!store.leased);
        confirm(Long.toString(store.revision));
        check(run("2").contains("recovery_required") && effects.size() == 1);
        check(run("1").contains("completed"));
        check(effects.equals(Arrays.asList("task.0", "task.1")) && requests == 2);
        check(events.stream().anyMatch(e->e.contains("tool_result_committed")&&e.contains("\"idempotency_key\":\"task.1\"")));
        events.clear();
        check(run("1").contains("done") && requests == 2 && effects.size() == 2);
        check(events.size()==3&&events.get(0).contains("tool_result_reused")&&events.get(1).contains("tool_result_reused")&&events.get(2).contains("finished"));
        check(events.get(0).contains("\"execution_scope\":\"task\"")&&events.get(0).contains("\"attempts\":1")&&events.get(0).contains("\"journal_revision\":\""+store.revision+"\""));
        store.leased = true;
        check(run("1").contains("recovery_required") && store.leased && requests == 2);
        store.leased = false;
        try (Agent agent = new Agent((request, invocation) -> new Agent.Response("done"))) {
            agent.setExecutionStore("task", store);
            try { agent.setExecutionStore("invalid/path", store); throw new AssertionError(); }
            catch (IllegalStateException expected) { }
            check(agent.run("input").contains("completed"));
            agent.setExecutionStore("", null);
            check(agent.run("other").contains("completed"));
        }
        boolean[] leased = {false};
        Execution.Store malformed = new Execution.Store() {
            public Execution.Snapshot open(String scope) { leased[0] = true; return new Execution.Snapshot("\0", ""); }
            public String commit(String s, String r, String next) { throw new AssertionError(); }
            public void close(String scope) { leased[0] = false; }
        };
        try (Agent agent = new Agent((r, i) -> { throw new AssertionError("model must not run"); })) {
            agent.setExecutionStore("bad", malformed);
            check(agent.run("input").contains("recovery_required"));
            check(!leased[0]);
        }
        Execution.Store throwsError = new Execution.Store() {
            public Execution.Snapshot open(String scope) { throw new AssertionError("injected"); }
            public String commit(String s, String r, String next) { throw new AssertionError(); }
            public void close(String scope) { throw new AssertionError("must not release another lease"); }
        };
        try (Agent agent = new Agent((r, i) -> { throw new AssertionError("model must not run"); })) {
            agent.setExecutionStore("error", throwsError);
            check(agent.run("input").contains("recovery_required"));
        }
        Store readStore = new Store();
        check(read(readStore, true, 64).contains("recovery_required") && reads == 1 && readRequests == 1);
        check(read(readStore, false, 64).contains("recovery_required") && reads == 1);
        check(read(readStore, true, 1).contains("budget_exceeded") && reads == 1);
        String resumed = read(readStore, true, 64);
        check(resumed.contains("completed") && resumed.contains("\"tool_calls\":2") && reads == 2 && readRequests == 2);
        check(read(readStore, true, 64).contains("handled") && reads == 2 && readRequests == 2);
        System.out.println("Java durable execution tests passed");
    }
}
