package io.coolder.libai;

/** MCP tools connection. Protocol, Schema and recovery remain in the native SDK. */
public final class McpClient implements AutoCloseable {
    static { System.loadLibrary("ai_jni"); }
    public interface Transport {
        String request(String wire, String contextJson, Agent.Invocation invocation);
        void notify(String wire, String contextJson, Agent.Invocation invocation);
        /** Must reliably release resources without throwing. */
        void close();
    }
    private long handle;
    private boolean busy;
    public McpClient(Transport transport) {
        if (transport == null) throw new IllegalArgumentException("transport is null");
        handle = nativeCreate(transport);
    }
    private McpClient(long handle) { this.handle = handle; }
    public static boolean stdioAvailable() { return nativeStdioAvailable(); }
    /** Absolute executable plus argv/environment in the C ABI JSON options format. */
    public static McpClient stdio(String optionsJson) { return new McpClient(nativeStdio(checked(optionsJson))); }
    synchronized long begin() {
        if (handle == 0) throw new IllegalStateException("MCP client has been disposed");
        if (busy) throw new IllegalStateException("MCP client is busy");
        busy = true; return handle;
    }
    synchronized void end() { busy = false; }
    private static String checked(String text) {
        if (text == null || text.indexOf('\0') >= 0) throw new IllegalArgumentException("text contains NUL or is null");
        return text;
    }
    private static String quote(String text) {
        checked(text);
        StringBuilder out = new StringBuilder("\"");
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c == '\\' || c == '"') out.append('\\').append(c);
            else if (c < 32) out.append(String.format("\\u%04x", (int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }
    private static void timeout(long milliseconds) {
        if (milliseconds < 1 || milliseconds > 86400000) throw new IllegalArgumentException("timeout must be 1..86400000ms");
    }
    /** Explicit host operation; direct calls do not acquire Agent governance/log guarantees. */
    public String call(String operation, String argumentsJson, Agent.Cancellation cancellation) {
        checked(operation); checked(argumentsJson); long pointer = begin(), token = 0;
        try {
            if (cancellation != null) token = cancellation.retain();
            return nativeCall(pointer, operation, argumentsJson, token);
        } finally { if (token != 0) Agent.releaseToken(token); end(); }
    }
    public String initialize(String name, String version) { return initialize(name, version, 30000, null); }
    public String initialize(String name, String version, long timeoutMs, Agent.Cancellation cancellation) {
        timeout(timeoutMs);
        return call("initialize", "{\"name\":" + quote(name) + ",\"version\":" + quote(version) + ",\"timeout_ms\":" + timeoutMs + "}", cancellation);
    }
    public String listTools() { return listTools(30000, null); }
    public String listTools(long timeoutMs, Agent.Cancellation cancellation) {
        timeout(timeoutMs);return call("list_tools", "{\"timeout_ms\":" + timeoutMs + "}", cancellation);
    }
    public String tool(String name) { return call("tool", "{\"name\":" + quote(name) + "}", null); }
    public String callTool(String name, String argumentsJson) { return callTool(name, argumentsJson, 30000, "", null); }
    public String callTool(String name, String argumentsJson, long timeoutMs, String idempotencyKey, Agent.Cancellation cancellation) {
        timeout(timeoutMs);
        return call("call_tool", "{\"name\":" + quote(name) + ",\"arguments_json\":" + quote(argumentsJson)
            + ",\"timeout_ms\":" + timeoutMs + ",\"idempotency_key\":" + quote(idempotencyKey) + "}", cancellation);
    }
    /** Explicitly close the underlying connection for every shared Agent binding. */
    public void closeConnection() { long pointer=begin();try { nativeClose(pointer); } finally { end(); } }
    /** Release only the outer handle. Bound Agents retain connection and Transport. */
    @Override public void close() {
        synchronized (this) { if (handle == 0) return; }
        long pointer = begin();
        try { nativeDestroy(pointer); synchronized(this) { handle=0; } } finally { end(); }
    }
    // Called through a cached JNI method ID, so no callback-thread classloader lookup is needed.
    private static String dispatch(Transport transport, String operation, String wire, String context, long pointer) {
        if (operation.equals("close")) { transport.close(); return null; }
        Agent.Invocation invocation = Agent.invocation(pointer);
        try {
            if (operation.equals("request")) return checked(transport.request(wire, context, invocation));
            if (operation.equals("notify")) { transport.notify(wire, context, invocation); return null; }
            throw new IllegalStateException("unknown MCP callback operation");
        } finally { Agent.expire(invocation); }
    }
    private static native long nativeCreate(Transport transport);
    private static native boolean nativeStdioAvailable();
    private static native long nativeStdio(String options);
    private static native String nativeCall(long pointer, String operation, String arguments, long token);
    private static native void nativeClose(long pointer);
    private static native void nativeDestroy(long pointer);
}
