import io.coolder.libai.Agent;
import io.coolder.libai.Execution;
import io.coolder.libai.McpClient;
import java.lang.ref.WeakReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public final class McpTest {
    static void check(boolean b) { if (!b) throw new AssertionError(); }
    static void rejects(Runnable f) { try { f.run(); } catch (RuntimeException e) { return; } throw new AssertionError("accepted invalid action"); }
    static final String POLICY = "{\"parameters_schema\":\"{\\\"type\\\":\\\"object\\\",\\\"additionalProperties\\\":false}\",\"execution_revision\":\"host-fixture-1\"}";
    static final class State { int calls,closed,turns; boolean initialized,lost,reentry; McpClient client; Agent.Cancellation token; String key; Agent.Invocation retained; CountDownLatch entered,finish; }
    static final class Transport implements McpClient.Transport {
        final State s;
        Transport(State s) { this.s=s; }
        public String request(String wire, String context, Agent.Invocation invocation) {
            check(!invocation.stopped()); s.retained=invocation;
            if (s.reentry) { rejects(() -> s.client.listTools()); rejects(() -> s.client.close()); rejects(() -> s.client.closeConnection()); }
            String result;
            if (wire.contains("\"method\":\"initialize\"")) {
                result="{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"fixture🌏\",\"version\":\"1\"}}";
            } else if (wire.contains("\"method\":\"tools/list\"")) {
                if (s.entered != null) { s.entered.countDown();try { check(s.finish.await(5,TimeUnit.SECONDS)); } catch (InterruptedException e) { throw new IllegalStateException(e); } }
                check(s.initialized && context.contains("2025-11-25"));
                result="{\"tools\":[{\"name\":\"echo\",\"inputSchema\":{\"type\":\"object\"},\"annotations\":{\"readOnlyHint\":true}}]}";
            } else {
                s.calls++;s.key=invocation.idempotencyKey();
                if (s.token != null) { s.token.cancel(); check(invocation.stopped()); throw new IllegalStateException("cancelled🌏"); }
                if (s.lost) throw new IllegalStateException("lost response after effect🌏");
                result="{\"content\":[{\"type\":\"text\",\"text\":\"echo🌏\"}],\"_meta\":{\"private\":\"secret\"}}";
            }
            Matcher id=Pattern.compile("\"request_id\":(\\d+)").matcher(context);check(id.find());
            return "{\"jsonrpc\":\"2.0\",\"id\":"+id.group(1)+",\"result\":"+result+"}";
        }
        public void notify(String wire,String context,Agent.Invocation invocation) {
            if (wire.contains("notifications/initialized")) { check(context.contains("2025-11-25"));s.initialized=true; }
        }
        public void close() { s.closed++; }
    }
    static McpClient connect(State s) {
        McpClient c=new McpClient(new Transport(s));
        check(c.initialize("test","1").contains("fixture🌏"));check(c.listTools().contains("input_schema"));return c;
    }
    static Agent agent(State s, boolean invalid) {
        return new Agent((q,i) -> {
            if (++s.turns==1) { check(q.contains("\"requires_approval\":true"));Agent.Response r=new Agent.Response();
                r.calls.add(new Agent.ToolCall("call1","local_echo",invalid?"{\"bad\":true}":"{}"));return r; }
            return new Agent.Response("done");
        });
    }
    static String run(Agent a,String input) { Agent.Hooks hooks=new Agent.Hooks();hooks.authorize=(n,args,required)->{check(required);return true;};return a.run(input,new Agent.Options(),hooks); }
    static final class Store implements Execution.Store {
        long sequence;String revision="",state="";boolean leased;
        public Execution.Snapshot open(String scope) { check(!leased);leased=true;return new Execution.Snapshot(revision,state); }
        public String commit(String scope,String expected,String next) { check(leased && expected.equals(revision));state=next;return revision=Long.toString(++sequence); }
        public void close(String scope) { leased=false; }
    }
    static void basic() {
        State s=new State();try (McpClient c=connect(s)) {
            check(c.callTool("echo","{}").contains("echo🌏"));check(s.calls==1);rejects(()->s.retained.stopped());
            try (Agent denied=agent(new State(),false)) { denied.addMcpTool(c,"echo","local_echo",POLICY);check(denied.run("echo").contains("completed"));check(s.calls==1); }
            try (Agent bad=agent(new State(),true)) { bad.addMcpTool(c,"echo","local_echo",POLICY);Agent.Hooks h=new Agent.Hooks();int[] authorizations={0};h.authorize=(n,args,r)->{authorizations[0]++;return true;};check(bad.run("echo",new Agent.Options(),h).contains("completed"));check(s.calls==1 && authorizations[0]==0); }
            State turns=new State();try (Agent a=agent(turns,false)) {
                rejects(()->a.addMcpTool(c,"echo","local_echo","{}"));a.addMcpTool(c,"echo","local_echo",POLICY);
                a.setExecutionStore("java-mcp",new Store());check(run(a,"echo").contains("completed"));check("java-mcp.0".equals(s.key));
                check(run(a,"echo").contains("completed"));check(turns.turns==2 && s.calls==2);check(!a.messages().contains("secret"));
            }
            rejects(()->c.listTools(0,null));rejects(()->c.call("list_tools","{\"extra\":true}",null));check(c.listTools().contains("echo"));
            c.closeConnection();c.closeConnection();check(s.closed==1);rejects(()->c.listTools());
        }
    }
    static WeakReference<Transport> bindThenRelease(Agent a,State s) {
        Transport t=new Transport(s);WeakReference<Transport> reference=new WeakReference<>(t);
        McpClient c=new McpClient(t);c.initialize("test","1");c.listTools();a.addMcpTool(c,"echo","local_echo",POLICY);c.close();return reference;
    }
    static void lifetime() throws Exception {
        State s=new State();Agent a=agent(new State(),false);WeakReference<Transport> reference=bindThenRelease(a,s);
        System.gc();check(reference.get()!=null && s.closed==0);check(run(a,"echo").contains("completed"));a.close();check(s.closed==1);
        for (int i=0;i<100 && reference.get()!=null;++i) { System.gc();Thread.sleep(10); }
        check(reference.get()==null);
    }
    static void recovery() {
        Store store=new Store();State turns=new State(),lost=new State();lost.lost=true;
        try (McpClient c=connect(lost);Agent a=agent(turns,false)) {
            a.addMcpTool(c,"echo","local_echo",POLICY).setExecutionStore("java-unknown",store);
            check(run(a,"write").contains("recovery_required"));check(lost.calls==1);
        }
        State next=new State();try (McpClient c=connect(next);Agent a=agent(turns,false)) {
            a.addMcpTool(c,"echo","local_echo",POLICY).setExecutionStore("java-unknown",store);
            check(run(a,"write").contains("recovery_required"));check(next.calls==0 && turns.turns==1);
        }
    }
    static void callbackBoundaries() {
        State s=new State();try (McpClient c=connect(s)) {
            s.client=c;s.reentry=true;check(c.listTools().contains("echo"));s.client=null;s.reentry=false;
            try (Agent.Cancellation token=new Agent.Cancellation()) {
                s.token=token;rejects(()->c.callTool("echo","{}",30000,"",token));check(token.cancelled());s.token=null;
            }
        }
        check(s.closed==1);
    }
    static void stdio(String script) {
        check(McpClient.stdioAvailable());String config="{\"executable\":\"/usr/bin/python3\",\"arguments\":[\""+script+"\"]}";
        try (McpClient c=McpClient.stdio(config)) {
            c.initialize("java-app","1");c.listTools();check(c.callTool("echo","{}").contains("from stdio"));
            try (Agent a=agent(new State(),false)) { a.addMcpTool(c,"echo","local_echo",POLICY);check(run(a,"echo").contains("completed")); }
        }
    }
    static void concurrency() throws Exception {
        State s=new State();try (McpClient c=connect(s)) {
            s.entered=new CountDownLatch(1);s.finish=new CountDownLatch(1);AtomicReference<Throwable> failure=new AtomicReference<>();
            Thread worker=new Thread(()->{try { check(c.listTools().contains("echo")); } catch (Throwable e) { failure.set(e); }});
            worker.start();
            try { check(s.entered.await(5,TimeUnit.SECONDS));rejects(()->c.listTools());rejects(()->c.closeConnection());rejects(()->c.close()); }
            finally { s.finish.countDown();worker.join(5000); }
            check(!worker.isAlive() && failure.get()==null);s.entered=null;s.finish=null;
        }
    }
    public static void main(String[] args) throws Exception { basic();lifetime();recovery();callbackBoundaries();concurrency();if(args.length==1)stdio(args[0]); }
}
