package io.coolder.libai;

/** Root-scoped desktop coding facilities. Host code owns authorization and JSON parsing. */
public final class CodingService {
    static { System.loadLibrary("ai_jni"); }
    private final String root;
    /** Bind operations to an existing, trusted application directory. */
    public CodingService(String root) {
        if (root == null || root.indexOf('\0') >= 0) throw new IllegalArgumentException("invalid root");
        this.root = root;
    }
    /** Mobile/portable builds report false; full desktop builds report true. */
    public static boolean available() { return nativeAvailable(); }
    /** Return {ok,...} JSON; operation failures have ok=false and an error field. */
    public String call(String operation, String argumentsJson) {
        if (operation == null || argumentsJson == null || operation.indexOf('\0') >= 0 || argumentsJson.indexOf('\0') >= 0)
            throw new IllegalArgumentException("invalid coding arguments");
        return nativeCall(root, operation, argumentsJson);
    }
    private static native boolean nativeAvailable();
    private static native String nativeCall(String root, String operation, String argumentsJson);
}
