package io.coolder.libai;
/** Portable schema and governance operations; no desktop backend is required. */
public final class Governance {
    static { System.loadLibrary("ai_jni"); }
    private Governance() { }
    public static String call(String operation, String argumentsJson) {
        if (operation==null || argumentsJson==null || operation.indexOf('\0')>=0 || argumentsJson.indexOf('\0')>=0)
            throw new IllegalArgumentException("invalid governance arguments");
        return nativeCall(operation,argumentsJson);
    }
    private static native String nativeCall(String operation,String argumentsJson);
}
