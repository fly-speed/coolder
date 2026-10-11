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
        private long handle = nativeTokenCreate();
        public synchronized void cancel() { check(); nativeTokenCancel(handle); }
        public synchronized boolean cancelled() { check(); return nativeTokenCancelled(handle); }
        private void check() { if (handle == 0) throw new IllegalStateException("cancellation closed"); }
        private synchronized long retain() { check(); return nativeTokenRetain(handle); }
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
        public boolean stopped() { check(); return nativeStopped(handle); }
        public boolean emit(String text) { check(); return nativeEmit(handle, checked(text)); }
    }

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
    public synchronized Agent addTool(String name, String description, String schema,
                                     boolean requiresApproval, Tool handler) {
        idle();
        if (handler == null) throw new IllegalArgumentException("handler is null");
        nativeAddTool(handle, checked(name), checked(description), checked(schema), requiresApproval);
        tools.put(name, handler);
        return this;
    }
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
        idle(); nativeDestroy(handle); handle = 0; tools.clear();
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
    private static native long nativeTokenRetain(long pointer);
    private static native void nativeTokenDestroy(long pointer);
    private static native void nativeTokenCancel(long pointer);
    private static native boolean nativeTokenCancelled(long pointer);
    private native void nativeDestroy(long pointer);
    private native void nativeAddTool(long pointer, String name, String description, String schema, boolean approval);
    private native String nativeRun(long pointer, String text, long[] options, boolean stream, boolean authorize, boolean events, long cancellation);
    private native void nativeCancel(long pointer);
    private native void nativeReset(long pointer);
    private static native boolean nativeStopped(long invocation);
    private static native boolean nativeEmit(long invocation, String delta);
}
