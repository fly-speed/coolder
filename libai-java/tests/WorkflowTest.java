import io.coolder.libai.*;
import java.lang.ref.WeakReference;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;

public final class WorkflowTest {
    static void check(boolean b){if(!b)throw new AssertionError();}
    static void status(String json,String state){check(json.contains("\"status\":\""+state+"\""));}
    static void rejects(Runnable f){try{f.run();}catch(RuntimeException expected){return;}throw new AssertionError("accepted invalid action");}
    static Workflow.NodeSpec spec(String name,String...writes){Workflow.NodeSpec spec=new Workflow.NodeSpec(name);spec.writes=writes;return spec;}
    static class Store implements Execution.Store {
        String revision="",state="";boolean leased,failResult;int sequence;Thread caller;
        Store(){caller=Thread.currentThread();}
        synchronized void thread(){if(caller!=null)check(Thread.currentThread()==caller);}
        public synchronized Execution.Snapshot open(String scope){thread();if(leased)throw new IllegalStateException("leased");leased=true;return new Execution.Snapshot(revision,state);}
        public synchronized String commit(String scope,String expected,String value){thread();check(leased&&expected.equals(revision));if(failResult&&value.contains("\"phase\":\"produced\"")){failResult=false;throw new IllegalStateException("lost result commit");}state=value;return revision=Integer.toString(++sequence);}
        public synchronized void close(String scope){thread();leased=false;}
    }
    static void parallelBranch(){
        AtomicInteger started=new AtomicInteger(),joins=new AtomicInteger(),skipped=new AtomicInteger();
        CountDownLatch gate=new CountDownLatch(2);List<String> snapshots=Collections.synchronizedList(new ArrayList<String>());
        List<Workflow.Context> retained=Collections.synchronizedList(new ArrayList<Workflow.Context>());
        try(Workflow g=new Workflow()){
            g.addNode(spec("choose"),(s,c)->{Workflow.NodeResult r=new Workflow.NodeResult();r.route="yes";return r;});
            for(String key:new String[]{"a","b"}){Workflow.NodeSpec n=spec(key,key);n.parallelSafe=true;
                g.addNode(n,(s,c)->{check(c.executionScope.equals("wf3_job_"+key));snapshots.add(s);retained.add(c);started.incrementAndGet();gate.countDown();
                    try{check(gate.await(3,TimeUnit.SECONDS));}catch(InterruptedException e){throw new RuntimeException(e);}return new Workflow.NodeResult("{\""+key+"\":\"🌏\"}");});g.addEdge("choose",key,"yes");}
            g.addNode(spec("no"),(s,c)->{skipped.incrementAndGet();return new Workflow.NodeResult();});g.addEdge("choose","no","no");
            Thread caller=Thread.currentThread();g.addNode(spec("join"),(s,c)->{check(Thread.currentThread()==caller&&s.contains("a")&&s.contains("b"));joins.incrementAndGet();return new Workflow.NodeResult();});
            for(String key:new String[]{"a","b","no"})g.addEdge(key,"join","");
            Workflow.Options options=new Workflow.Options();options.scope="job";options.maxParallel=2;
            String result=g.run("{}",options);status(result,"completed");check(result.contains("\"accepted\":false")&&result.contains("\"no\":\"skipped\"")&&result.contains("\"node_calls\":4"));
            check(started.get()==2&&joins.get()==1&&skipped.get()==0&&snapshots.equals(Arrays.asList("{}","{}")));
            for(Workflow.Context context:retained){rejects(()->context.stopped());rejects(()->context.remainingMs());rejects(()->context.retainCancellation());}
        }
    }
    static WeakReference<Workflow.Node> register(Workflow graph,String name){
        Workflow.Node node=new Workflow.Node(){public Workflow.NodeResult execute(String s,Workflow.Context c){return new Workflow.NodeResult();}};
        WeakReference<Workflow.Node> ref=new WeakReference<>(node);graph.addNode(spec(name),node);return ref;
    }
    static WeakReference<Workflow.Node> failedRegistration(Workflow graph){
        Workflow.Node node=new Workflow.Node(){public Workflow.NodeResult execute(String s,Workflow.Context c){return new Workflow.NodeResult();}};
        WeakReference<Workflow.Node> ref=new WeakReference<>(node);
        try{graph.addNode(spec("a"),node);throw new AssertionError();}catch(IllegalStateException expected){}return ref;
    }
    static void collect(WeakReference<?> reference)throws Exception{for(int i=0;i<80&&reference.get()!=null;++i){System.gc();Thread.sleep(10);}check(reference.get()==null);}
    static void ownershipAndErrors()throws Exception{
        Workflow g=new Workflow();WeakReference<Workflow.Node> ref=register(g,"a");System.gc();check(ref.get()!=null);
        WeakReference<Workflow.Node> failed=failedRegistration(g);collect(failed);
        Workflow.Node node=(s,c)->new Workflow.NodeResult();rejects(()->g.addNode(spec("a"),node));g.close();g.close();collect(ref);rejects(()->g.run("{}"));
        for(int mode=0;mode<5;++mode){final int kind=mode;
            try(Workflow graph=new Workflow()){
                graph.addNode(spec("a"),(s,c)->{
                    if(kind==0)return null;
                    if(kind==1)throw new AssertionError("callback failure");
                    if(kind==2)return new Workflow.NodeResult("{bad}");
                    if(kind==3){Workflow.NodeResult r=new Workflow.NodeResult();r.status="bad";return r;}
                    throw new AssertionError(){@Override public String toString(){throw new IllegalStateException("broken exception string");}};
                });status(graph.run("{}"),"callback_error");
            }
        }
        AtomicInteger effects=new AtomicInteger();try(Workflow graph=new Workflow()){
            for(String key:new String[]{"a","b"}){Workflow.NodeSpec n=spec(key,"x");n.parallelSafe=true;graph.addNode(n,(s,c)->{effects.incrementAndGet();return new Workflow.NodeResult();});}
            Workflow.Options o=new Workflow.Options();o.maxParallel=2;status(graph.run("{}",o),"protocol_error");check(effects.get()==0);
            o.maxParallel=0;rejects(()->graph.run("{}",o));rejects(()->graph.addNode(spec("bad","x","x"),node));rejects(()->graph.addNode(spec("bad","\ud800"),node));
        }
    }
    static void durable(){
        Store store=new Store();AtomicInteger effects=new AtomicInteger();try(Workflow g=new Workflow()){
            g.addNode(spec("write","answer"),(s,c)->{effects.incrementAndGet();throw new IllegalStateException("lost acknowledgement");});
            Workflow.Options options=new Workflow.Options();options.scope="job";options.executionStore=store;
            status(g.run("{}",options),"recovery_required");status(g.run("{}",options),"recovery_required");check(effects.get()==1&&!store.leased);
            String revision=store.revision;rejects(()->g.confirmNode(store,"job","{}","stale","write","applied",new Workflow.NodeResult(),"receipt"));
            rejects(()->g.confirmNode(store,"job","{}",revision,"write","applied",new Workflow.NodeResult("{\"outside\":1}"),"receipt"));
            g.confirmNode(store,"job","{}",revision,"write","applied",new Workflow.NodeResult("{\"answer\":42}"),"verified-receipt");
            status(g.run("{}",options),"completed");status(g.run("{}",options),"completed");check(effects.get()==1&&!store.leased);
            check(g.run("{\"changed\":true}",options).contains("\"progress_available\":false"));
        }
        Store failing=new Store();failing.failResult=true;try(Workflow g=new Workflow()){
            g.addNode(spec("write"),(s,c)->{effects.incrementAndGet();return new Workflow.NodeResult();});Workflow.Options options=new Workflow.Options();options.scope="job";options.executionStore=failing;
            String result=g.run("{}",options);status(result,"recovery_required");check(result.contains("\"progress_available\":false"));status(g.run("{}",options),"recovery_required");check(effects.get()==2&&!failing.leased);
        }
    }
    static void concurrencyAndCancel()throws Exception{
        CountDownLatch entered=new CountDownLatch(1),finish=new CountDownLatch(1);AtomicReference<String> result=new AtomicReference<>();AtomicReference<Throwable> error=new AtomicReference<>();AtomicReference<Agent.Cancellation> retained=new AtomicReference<>();AtomicReference<Workflow.Context> context=new AtomicReference<>();
        try(Workflow g=new Workflow();Agent.Cancellation token=new Agent.Cancellation()){
            g.addNode(spec("wait"),(s,c)->{context.set(c);retained.set(c.retainCancellation());rejects(()->g.run("{}"));rejects(()->g.close());rejects(()->g.addEdge("a","b",""));entered.countDown();try{check(finish.await(3,TimeUnit.SECONDS));}catch(InterruptedException e){throw new RuntimeException(e);}check(c.stopped());rejects(()->c.agentOptions());return new Workflow.NodeResult();});
            Workflow.Options o=new Workflow.Options();o.cancellation=token;
            Thread thread=new Thread(()->{try{result.set(g.run("{}",o));}catch(Throwable e){error.set(e);}});thread.start();
            try{check(entered.await(3,TimeUnit.SECONDS));rejects(()->g.close());rejects(()->g.run("{}"));rejects(()->context.get().stopped());token.cancel();token.close();}
            finally{finish.countDown();thread.join(5000);}
            check(!thread.isAlive()&&error.get()==null);status(result.get(),"cancelled");check(retained.get().cancelled());retained.get().close();rejects(()->context.get().stopped());
        }
        try(Workflow g=new Workflow()){
            g.addNode(spec("wait"),(s,c)->{while(!c.stopped())Thread.yield();rejects(()->c.agentOptions());return new Workflow.NodeResult();});Workflow.Options o=new Workflow.Options();o.timeoutMs=5;status(g.run("{}",o),"deadline_exceeded");
        }
    }
    static void parallelChildAgents(){
        AtomicInteger models=new AtomicInteger(),effects=new AtomicInteger();Store[] stores={new Store(),new Store()};for(Store s:stores)s.caller=null;
        try(Workflow g=new Workflow()){
            for(int k=0;k<2;++k){final int key=k;Workflow.NodeSpec n=spec("agent"+key,"answer"+key);n.parallelSafe=true;
                g.addNode(n,(state,c)->{
                    check(c.executionScope.equals("wf3_job_agent"+key));try(Agent agent=new Agent((request,i)->{models.incrementAndGet();Agent.Response r=new Agent.Response();if(!request.contains("\"role\":\"tool\""))r.calls.add(new Agent.ToolCall("one","write","{}"));else r.text="done";return r;})){
                        agent.addTool("write","","{}",false,(call,i)->{check(i.idempotencyKey().equals(c.executionScope+".0"));effects.incrementAndGet();return new Agent.ToolResult("3");});
                        agent.setExecutionStore(c.executionScope,stores[key]);try(Workflow.ChildOptions options=c.agentOptions()){
                            String result=agent.run(state,options.options,new Agent.Hooks(),options.cancellation);
                            if(result.contains("\"status\":\"completed\""))return new Workflow.NodeResult("{\"answer"+key+"\":3}");
                            Workflow.NodeResult r=new Workflow.NodeResult();r.status="recovery_required";return r;
                        }
                    }
                });
            }
            Workflow.Options o=new Workflow.Options();o.scope="job";o.maxParallel=2;status(g.run("{}",o),"completed");status(g.run("{}",o),"completed");check(models.get()==4&&effects.get()==2);status(g.run("{\"changed\":true}",o),"recovery_required");check(effects.get()==2);
        }
    }
    static void worker(String root){
        try(FileExecutionStore store=FileExecutionStore.plaintext(root);Workflow g=new Workflow()){
            g.addNode(spec("write"),(s,c)->{try(FileOutputStream output=new FileOutputStream(Paths.get(root,"effect").toFile(),true)){output.write("once\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));output.getFD().sync();}catch(IOException e){throw new RuntimeException(e);}Runtime.getRuntime().halt(71);throw new AssertionError();});
            Workflow.Options o=new Workflow.Options();o.scope="job";o.executionStore=store;g.run("{}",o);
        }throw new AssertionError("worker failed to halt");
    }
    static void processExit()throws Exception{
        Path root=Files.createTempDirectory("libai-java-workflow-",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        try{
            Process p=new ProcessBuilder(Paths.get(System.getProperty("java.home"),"bin","java").toString(),"-Xcheck:jni","-Djava.library.path="+System.getProperty("java.library.path"),"-cp",System.getProperty("java.class.path"),"WorkflowTest",root.toString()).inheritIO().start();check(p.waitFor(10,TimeUnit.SECONDS)&&p.exitValue()==71);
            AtomicInteger effects=new AtomicInteger();try(FileExecutionStore store=FileExecutionStore.plaintext(root.toString());Workflow g=new Workflow()){
                g.addNode(spec("write"),(s,c)->{effects.incrementAndGet();return new Workflow.NodeResult();});Workflow.Options o=new Workflow.Options();o.scope="job";o.executionStore=store;status(g.run("{}",o),"recovery_required");
                Execution.Snapshot snapshot=store.open("wfg3_job");store.close("wfg3_job");g.confirmNode(store,"job","{}",snapshot.revision,"write","applied",new Workflow.NodeResult(),"effect-file");status(g.run("{}",o),"completed");check(effects.get()==0&&new String(Files.readAllBytes(root.resolve("effect")),java.nio.charset.StandardCharsets.UTF_8).equals("once\n"));
            }
        }finally{try(java.util.stream.Stream<Path> files=Files.walk(root)){files.sorted(Comparator.reverseOrder()).forEach(path->{try{Files.delete(path);}catch(IOException e){throw new RuntimeException(e);}});}}
    }
    public static void main(String[] args)throws Exception{if(args.length==1){worker(args[0]);return;}parallelBranch();ownershipAndErrors();durable();concurrencyAndCancel();parallelChildAgents();processExit();}
}
