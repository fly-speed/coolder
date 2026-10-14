package io.coolder.libai;

/** Owning bounded DAG over the shared C ABI, with no JSON library dependency. */
public final class Workflow implements AutoCloseable {
    static { System.loadLibrary("ai_jni"); }
    public interface Node { NodeResult execute(String stateJson, Context context); }
    public static final class NodeSpec {
        public final String name;
        public String[] writes = new String[0];
        public String executionRevision = "1";
        /** Trusted host assertion: captured state and callbacks support worker threads. */
        public boolean parallelSafe, sideEffectFree;
        public NodeSpec(String name) { this.name = checked(name); }
        private String json() {
            if (writes == null) throw new IllegalArgumentException("writes is null");
            StringBuilder keys = new StringBuilder("[");
            for (int i=0;i<writes.length;++i) { if(i>0)keys.append(',');keys.append(quote(writes[i])); }
            keys.append(']');
            return "{\"name\":"+quote(name)+",\"writes\":"+keys+",\"execution_revision\":"+quote(executionRevision)
                +",\"parallel_safe\":"+parallelSafe+",\"side_effect_free\":"+sideEffectFree+"}";
        }
    }
    public static final class NodeResult {
        public String patchJson="{}", route="", status="completed", error="";
        public NodeResult() { }
        public NodeResult(String patchJson) { this.patchJson=checked(patchJson); }
        private String json() { return "{\"patch_json\":"+quote(patchJson)+",\"route\":"+quote(route)
            +",\"status\":"+quote(status)+",\"error\":"+quote(error)+"}"; }
    }
    public static final class Options {
        public String scope="";
        public Execution.Store executionStore;
        public Agent.Cancellation cancellation;
        public long maxParallel=1,maxNodeCalls=64,maxStateBytes=2*1024*1024,timeoutMs=300000;
        private String json() { return "{\"scope\":"+quote(scope)+",\"max_parallel\":"+maxParallel
            +",\"max_node_calls\":"+maxNodeCalls+",\"max_state_bytes\":"+maxStateBytes+",\"timeout_ms\":"+timeoutMs+"}"; }
    }
    /** Borrowed methods expire on callback return and are restricted to that thread. */
    public static final class Context {
        private final Agent.Invocation invocation;
        /** Copyable child Agent journal namespace, not a business operation idempotency key. */
        public final String executionScope;
        private Context(Agent.Invocation invocation,String metadata) {
            this.invocation=invocation;
            String prefix="{\"execution_scope\":\"";
            if(!metadata.startsWith(prefix)||!metadata.endsWith("\"}"))throw new IllegalStateException("invalid workflow metadata");
            executionScope=metadata.substring(prefix.length(),metadata.length()-2);
            if(executionScope.length()>102||!executionScope.matches("[A-Za-z0-9_-]*"))throw new IllegalStateException("invalid workflow scope");
        }
        public boolean stopped() { return invocation.stopped(); }
        public long remainingMs() { return invocation.remainingMs(); }
        public Agent.Cancellation retainCancellation() { return invocation.retainCancellation(); }
        public ChildOptions agentOptions() {
            if(stopped())throw new IllegalStateException("workflow stopped before child Agent");
            Agent.Cancellation token=retainCancellation();
            try {
                long remaining=remainingMs();
                if(remaining==0||stopped())throw new IllegalStateException("workflow deadline expired before child Agent");
                Agent.Options options=new Agent.Options();options.timeoutMs=Math.max(remaining,0);
                return new ChildOptions(options,token);
            }catch(RuntimeException|Error error){token.close();throw error;}
        }
    }
    /** Use only inside try-with-resources; token remains owned until close. */
    public static final class ChildOptions implements AutoCloseable {
        public final Agent.Options options;
        public final Agent.Cancellation cancellation;
        private ChildOptions(Agent.Options options,Agent.Cancellation cancellation){this.options=options;this.cancellation=cancellation;}
        @Override public void close(){cancellation.close();}
    }
    private long handle;
    private boolean busy;
    public Workflow(){handle=nativeCreate();}
    private synchronized long begin(){
        if(handle==0)throw new IllegalStateException("workflow is closed");
        if(busy)throw new IllegalStateException("workflow is busy");
        busy=true;return handle;
    }
    private synchronized void end(){busy=false;}
    private static String checked(String value){
        if(value==null||value.indexOf('\0')>=0)throw new IllegalArgumentException("null/NUL workflow text");
        for(int i=0;i<value.length();++i){char c=value.charAt(i);
            if(Character.isHighSurrogate(c)){if(++i>=value.length()||!Character.isLowSurrogate(value.charAt(i)))throw new IllegalArgumentException("invalid workflow Unicode");}
            else if(Character.isLowSurrogate(c))throw new IllegalArgumentException("invalid workflow Unicode");
        }return value;
    }
    private static String quote(String value){checked(value);StringBuilder s=new StringBuilder("\"");
        for(int i=0;i<value.length();++i){char c=value.charAt(i);if(c=='"'||c=='\\')s.append('\\').append(c);else if(c<32)s.append(String.format("\\u%04x",(int)c));else s.append(c);}return s.append('"').toString();
    }
    public Workflow addNode(NodeSpec spec,Node node){
        if(spec==null||node==null)throw new IllegalArgumentException("null workflow spec/node");
        String descriptor=spec.json();long pointer=begin();try{nativeAddNode(pointer,descriptor,node);}finally{end();}return this;
    }
    public Workflow addEdge(String from,String to,String label){checked(from);checked(to);checked(label);long pointer=begin();try{nativeAddEdge(pointer,from,to,label);}finally{end();}return this;}
    public String run(String initialStateJson){return run(initialStateJson,new Options());}
    /** Returned JSON contains status/state_json/nodes/node_calls and shared task_contract. */
    public String run(String initialStateJson,Options options){
        checked(initialStateJson);if(options==null)throw new IllegalArgumentException("options is null");
        String wire=options.json();Execution.Store store=options.executionStore;Agent.Cancellation cancellation=options.cancellation;
        long pointer=begin(),token=0;
        try{if(cancellation!=null)token=cancellation.retain();return nativeRun(pointer,initialStateJson,wire,store,token);}
        finally{if(token!=0)Agent.releaseToken(token);end();}
    }
    /** Trusted whole-node receipt; never expose as an unrestricted model tool. */
    public void confirmNode(Execution.Store store,String scope,String initial,String revision,String node,
                            String outcome,NodeResult output,String reference){
        if(store==null||output==null)throw new IllegalArgumentException("null store/output");
        checked(scope);checked(initial);checked(revision);checked(node);checked(outcome);checked(reference);String wire=output.json();
        long pointer=begin();try{nativeConfirm(pointer,store,scope,initial,revision,node,outcome,wire,reference);}finally{end();}
    }
    @Override public void close(){
        synchronized(this){if(handle==0)return;}
        long pointer=begin();try{nativeDestroy(pointer);synchronized(this){handle=0;}}finally{end();}
    }
    // Cached JNI method ID avoids worker-thread class-loader lookup. Expire on all paths.
    private static String dispatch(Node node,String state,String metadata,long pointer){
        Agent.Invocation invocation=Agent.invocation(pointer);
        try{NodeResult result=node.execute(state,new Context(invocation,metadata));
            if(result==null)throw new IllegalStateException("workflow node returned null");return result.json();
        }finally{Agent.expire(invocation);}
    }
    private static native long nativeCreate();
    private static native void nativeAddNode(long pointer,String spec,Node node);
    private static native void nativeAddEdge(long pointer,String from,String to,String label);
    private static native String nativeRun(long pointer,String initial,String options,Execution.Store store,long token);
    private static native void nativeConfirm(long pointer,Execution.Store store,String scope,String initial,
                                            String revision,String node,String outcome,String output,String reference);
    private static native void nativeDestroy(long pointer);
}
