package io.coolder.libai;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Java/JNI facade over one C++ agent and its persistent session. */
public final class Agent implements AutoCloseable {
    static { System.loadLibrary("ai_jni"); }

    /** Independently owned thread-safe stop token. Retained runs survive close(). */
    public static final class Cancellation implements AutoCloseable {
        private long handle;
        public Cancellation() { this(nativeTokenCreate()); }
        private Cancellation(long pointer) {
            if (pointer == 0) throw new IllegalStateException("cancellation allocation failed");
            handle = pointer;
        }
        public synchronized void cancel() { check(); nativeTokenCancel(handle); }
        public synchronized boolean cancelled() { check(); return nativeTokenCancelled(handle); }
        private void check() { if (handle == 0) throw new IllegalStateException("cancellation closed"); }
        synchronized long retain() { check(); return nativeTokenRetain(handle); }
        @Override public synchronized void close() {
            if (handle != 0) { nativeTokenDestroy(handle); handle = 0; }
        }
    }

    /** Model implementations receive a typed-transcript JSON request. */
    public interface Model { Response complete(String requestJson, Invocation invocation); }
    /** Tools validate their own JSON arguments and return text or an error. */
    public interface Tool { ToolResult execute(ToolCall call, Invocation invocation); }
    public interface TextSink { boolean onText(String delta); }
    public interface Authorization { boolean allow(String name, String arguments, boolean required); }
    public interface EventSink { void onEvent(String eventJson); }

    /** Borrowed callback boundary; retaining it after callback return is an error. */
    public static final class Invocation {
        private long handle;
        private final Thread thread = Thread.currentThread();
        private Invocation(long handle) { this.handle = handle; }
        private void check() { if (handle == 0 || thread != Thread.currentThread()) throw new IllegalStateException("invocation expired or wrong thread"); }
        public String idempotencyKey() { check(); return nativeIdempotencyKey(handle); }
        public boolean stopped() { check(); return nativeStopped(handle); }
        /** -1 unlimited, 0 expired. Never map 0 to an unlimited child timeout. */
        public long remainingMs() { check(); return nativeRemainingMs(handle); }
        /** Independent owner of the shared stop state; close after use. */
        public Cancellation retainCancellation() {
            check(); long pointer = nativeInvocationToken(handle);
            if (pointer == 0) throw new IllegalStateException("invocation has no cancellation token or retain failed");
            try { return new Cancellation(pointer); }
            catch (RuntimeException | Error failure) { nativeTokenDestroy(pointer); throw failure; }
        }
        public boolean emit(String text) { check(); return nativeEmit(handle, checked(text)); }
    }

    static Invocation invocation(long pointer) { return new Invocation(pointer); }
    static void expire(Invocation invocation) { invocation.handle = 0; }
    static void releaseToken(long pointer) { nativeTokenDestroy(pointer); }

    /** Complete JSON arguments are retained as text, with no JSON library dependency. */
    public static final class ToolCall {
        public final String id, name, arguments;
        public ToolCall(String id, String name, String arguments) {
            this.id = checked(id); this.name = checked(name); this.arguments = checked(arguments);
        }
    }
    public static final class Response {
        public String text = "", reasoning = "", error = "";
        public long inputTokens, outputTokens;
        public boolean retryable;
        public final List<ToolCall> calls = new ArrayList<>();
        public Response() { }
        public Response(String text) { this.text = checked(text); }
    }
    public static final class ToolResult {
        public final String text;
        public final boolean error;
        public ToolResult(String text) { this(text, false); }
        public ToolResult(String text, boolean error) { this.text = checked(text); this.error = error; }
    }
    /** Defaults match the portable C++ SDK; timeout is cooperative. */
    public static final class Options {
        public long maxModelCalls = 16, maxToolCalls = 64, maxOutputTokens = 32768;
        public long maxTokensPerCall = 4096, maxContextBytes = 1024 * 1024;
        public long maxToolResultBytes = 64 * 1024, maxModelRetries = 1, timeoutMs = 300000;
        private long[] values() {
            long[] values = {maxModelCalls, maxToolCalls, maxOutputTokens, maxTokensPerCall,
                maxContextBytes, maxToolResultBytes, maxModelRetries, timeoutMs};
            for (long value : values) if (value < 0) throw new IllegalArgumentException("negative run limit");
            return values;
        }
    }
    public static final class Hooks {
        public TextSink onText;
        public Authorization authorize;
        public EventSink onEvent;
    }

    private final Model model;
    private final Map<String, Tool> tools = new HashMap<>();
    private long handle;
    private boolean busy;
    private Hooks hooks;
    private Execution.Store executionStore;

    public Agent(Model model) { this(model, ""); }
    public Agent(Model model, String instructions) {
        if (model == null) throw new IllegalArgumentException("model is null");
        this.model = model;
        handle = nativeCreate(checked(instructions), null, null);
    }
    private Agent(String config, String apiKey, String instructions) {
        this.model = null;
        handle = nativeCreate(checked(instructions), checked(config), checked(apiKey));
    }
    /** Built-in desktop transport; normal tool and streaming hooks apply. */
    public static Agent fromProvider(String configJson, String apiKey, String instructions) {
        return new Agent(configJson, apiKey, instructions);
    }
    /** Copy the idle transcript as a JSON array. */
    public synchronized String messages() { idle(); return nativeMessages(handle); }
    private void idle() {
        if (handle == 0) throw new IllegalStateException("agent is closed");
        if (busy) throw new IllegalStateException("agent is running");
    }
    private static String checked(String text) {
        if (text == null || text.indexOf('\0') >= 0) throw new IllegalArgumentException("text contains NUL or is null");
        return text;
    }
    /** Configure host-declared capabilities before running the agent. */
    public synchronized Agent configureModel(String profileJson) {
        idle(); nativeConfigureModel(handle, checked(profileJson)); return this;
    }
    /** Host-validate final JSON; empty schema disables validation. Feedback range 0..8. */
    public synchronized Agent configureOutput(String schemaJson, long repairs) {
        idle();
        if (repairs < 0 || repairs > 8) throw new IllegalArgumentException("repairs must be from 0 to 8");
        nativeConfigureOutput(handle, checked(schemaJson), repairs); return this;
    }
    private native void nativeConfigureOutput(long pointer, String schemaJson, long repairs);
    private native void nativeConfigureModel(long pointer, String profileJson);
    /** Configure a stable durable task; null disables persistence. Store is retained. */
    public synchronized Agent setExecutionStore(String scope, Execution.Store store) {
        idle(); nativeSetExecutionStore(handle, checked(scope), store != null);
        executionStore = store; return this;
    }
    public synchronized Agent addTool(String name, String description, String schema,
                                     boolean requiresApproval, Tool handler) {
        return addTool(name, description, schema, requiresApproval, "1", handler);
    }
    /** Change executionRevision when handler semantics change. Pending recovery checks it. */
    public synchronized Agent addTool(String name, String description, String schema,
                                     boolean requiresApproval, String executionRevision, Tool handler) {
        return addTool(name, description, schema, requiresApproval, executionRevision, false, handler);
    }
    /** Trusted host assertion: permit unconfirmed retries only without external side effects. */
    public synchronized Agent addTool(String name, String description, String schema,
            boolean requiresApproval, String executionRevision, boolean sideEffectFree, Tool handler) {
        idle();
        if (handler == null) throw new IllegalArgumentException("handler is null");
        nativeAddToolGoverned(handle, checked(name), checked(description), checked(schema), requiresApproval, checked(executionRevision), sideEffectFree);
        tools.put(name, handler);
        return this;
    }
    /** Bind discovered MCP capabilities while idle using explicit host Schema/revision. */
    public synchronized Agent addMcpTool(McpClient client, String remoteName, String localName, String policyJson) {
        idle();
        if (client == null) throw new IllegalArgumentException("MCP client is null");
        long connection = client.begin();
        try { nativeAddMcpTool(handle, connection, checked(remoteName), checked(localName), checked(policyJson)); }
        finally { client.end(); }
        return this;
    }
    private native void nativeAddMcpTool(long pointer, long client, String remote, String local, String policy);
    /** Host-owned scope/filter/revision; Agent retains the native retriever. */
    public synchronized Agent addRetrievalTool(Rag retriever,String name,Rag.Binding binding) {
        idle();if(retriever==null||binding==null)throw new IllegalArgumentException("null retriever/binding");
        String policy=binding.json();long component=retriever.beginRetriever();
        try{nativeAddRetrievalTool(handle,component,checked(name),policy);}finally{retriever.end();}
        return this;
    }
    private native void nativeAddRetrievalTool(long pointer,long retriever,String name,String binding);
    public String run(String text) { return run(text, new Options(), new Hooks()); }
    /** Returns status/text/error and usage as JSON; sessions persist between calls. */
    public String run(String text, Options options, Hooks hooks) {
        return run(text, options, hooks, null);
    }
    public String run(String text, Options options, Hooks hooks, Cancellation cancellation) {
        checked(text);
        if (hooks == null || options == null) throw new IllegalArgumentException("options/hooks are null");
        Hooks snapshot = new Hooks();
        snapshot.onText = hooks.onText; snapshot.authorize = hooks.authorize; snapshot.onEvent = hooks.onEvent;
        long[] limits = options.values();
        final long pointer;
        synchronized (this) {
            idle(); busy = true; this.hooks = snapshot; pointer = handle;
        }
        long lease = 0;
        try {
            if (cancellation != null) lease = cancellation.retain();
            return nativeRun(pointer, text, limits, snapshot.onText != null,
                             snapshot.authorize != null, snapshot.onEvent != null, lease);
        } finally {
            if (lease != 0) nativeTokenDestroy(lease);
            synchronized (this) { busy = false; this.hooks = null; }
        }
    }
    /** May be called by another thread while run is waiting for a model/tool. */
    public synchronized void cancel() { if (handle != 0) nativeCancel(handle); }
    public synchronized void reset() { idle(); nativeReset(handle); }
    @Override public synchronized void close() {
        if (handle == 0) return;
        idle(); nativeDestroy(handle); handle = 0; tools.clear(); executionStore = null;
    }

    // JNI calls these methods on the run thread. Callback handles never escape.
    private Response complete(String request, long pointer) {
        Invocation invocation = new Invocation(pointer);
        try {
            Response response = model.complete(request, invocation);
            if (response == null) throw new IllegalArgumentException("model returned null");
            checked(response.text); checked(response.reasoning); checked(response.error);
            if (response.inputTokens < 0 || response.outputTokens < 0)
                throw new IllegalArgumentException("negative token usage");
            return response;
        } finally { invocation.handle = 0; }
    }
    private ToolResult execute(String id, String name, String arguments, long pointer) {
        Invocation invocation = new Invocation(pointer);
        try {
            ToolResult output = tools.get(name).execute(new ToolCall(id, name, arguments), invocation);
            if (output == null) throw new IllegalArgumentException("tool returned null");
            return output;
        } finally { invocation.handle = 0; }
    }
    private boolean text(String delta) { return hooks.onText.onText(delta); }
    private boolean authorize(String name, String arguments, boolean required) {
        return hooks.authorize.allow(name, arguments, required);
    }
    private void event(String event) { hooks.onEvent.onEvent(event); }

    private native long nativeCreate(String instructions, String config, String apiKey);
    private native String nativeMessages(long pointer);
    private static native long nativeTokenCreate();
    private static native long nativeRemainingMs(long pointer);
    private static native long nativeInvocationToken(long pointer);
    private static native long nativeTokenRetain(long pointer);
    private static native void nativeTokenDestroy(long pointer);
    private static native void nativeTokenCancel(long pointer);
    private static native boolean nativeTokenCancelled(long pointer);
    private native void nativeDestroy(long pointer);
    private native void nativeAddToolGoverned(long pointer, String name, String description, String schema, boolean approval, String revision, boolean sideEffectFree);
    private native String nativeRun(long pointer, String text, long[] options, boolean stream, boolean authorize, boolean events, long cancellation);
    private native void nativeCancel(long pointer);
    private native void nativeReset(long pointer);
    private native void nativeSetExecutionStore(long pointer, String scope, boolean enabled);
    private static native String nativeIdempotencyKey(long invocation);
    private static native boolean nativeStopped(long invocation);
    private static native boolean nativeEmit(long invocation, String delta);
}
