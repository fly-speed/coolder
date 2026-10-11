import io.coolder.libai.Agent;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
public final class AgentTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        int[] turns = {0}, tools = {0};
        Agent.Invocation[] saved = {null};
        try (Agent agent = new Agent((request, invocation) -> {
            saved[0] = invocation;
            Agent.Response response = new Agent.Response("你好🌏");
            if (turns[0]++ == 0) response.calls.add(new Agent.ToolCall("编号", "echo", "{}"));
            else { check(request.contains("你好🌏")); check(invocation.emit("你好🌏")); }
            return response;
        })) {
            agent.addTool("echo", "Echo", "{}", true, (call, invocation) -> {
                check(call.id.equals("编号")); tools[0]++; return new Agent.ToolResult("你好🌏");
            });
            Agent.Hooks hooks = new Agent.Hooks();
            hooks.authorize = (name, json, required) -> required;
            hooks.onText = text -> { check(text.equals("你好🌏")); return true; };
            String result = agent.run("开始", new Agent.Options(), hooks);
            check(result.contains("completed") && result.contains("你好🌏") && tools[0] == 1);
            try { saved[0].stopped(); throw new AssertionError(); } catch (IllegalStateException expected) { }
            check(agent.messages().contains("编号") && agent.messages().contains("\"role\":\"tool\""));
            try (Agent.Cancellation token = new Agent.Cancellation()) {
                token.cancel(); check(token.cancelled());
                int before = turns[0];
                check(agent.run("skip", new Agent.Options(), new Agent.Hooks(), token).contains("cancelled"));
                check(turns[0] == before);
            }
            agent.reset();
            check(agent.messages().equals("[]"));
            Agent.Options options = new Agent.Options(); options.maxModelCalls = 0;
            check(agent.run("budget", options, new Agent.Hooks()).contains("budget_exceeded"));
        }
        try (Agent agent = new Agent((request, invocation) -> { throw new RuntimeException("模型失败🌏"); })) {
            check(agent.run("start").contains("模型失败🌏"));
        }
        CountDownLatch started = new CountDownLatch(1);
        try (Agent agent = new Agent((request, invocation) -> {
            started.countDown();
            while (!invocation.stopped()) Thread.yield();
            return new Agent.Response();
        })) {
            String[] result = {null};
            Thread thread = new Thread(() -> result[0] = agent.run("wait")); thread.start();
            check(started.await(5, TimeUnit.SECONDS));
            try { agent.close(); throw new AssertionError(); } catch (IllegalStateException expected) { }
            agent.cancel(); thread.join(5000);
            check(!thread.isAlive() && result[0].contains("cancelled"));
        }
        String url = System.getenv("LIBAI_TEST_PROVIDER_URL");
        if (url != null) {
            try (Agent agent = Agent.fromProvider("{\"protocol\":\"openai_chat\",\"base_url\":\"" + url + "\",\"model\":\"fixture\"}", "temporary", "")) {
                check(agent.run("你好😀").contains("native😀"));
                check(agent.messages().contains("native😀"));
            }
        }
        System.out.println("Java JNI tests passed");
    }
}
