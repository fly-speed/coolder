package io.coolder.libai;

/** Owning POSIX JSON file store. Explicit plaintext has no encryption/authentication.
 * Root must be an existing uid-owned absolute directory with mode 0700.
 * Close after all configured Agent users finish; closing with a lease is rejected. */
public final class FileExecutionStore implements Execution.Store, AutoCloseable {
    static { System.loadLibrary("ai_jni"); }
    private long handle;
    private FileExecutionStore(String root) { handle = nativeCreate(checked(root)); }
    /** Explicit unencrypted factory; never a default encryption choice. */
    public static FileExecutionStore plaintext(String root) { return new FileExecutionStore(root); }
    public static boolean available() { return nativeAvailable(); }
    private static String checked(String text) {
        if (text == null || text.indexOf('\0') >= 0) throw new IllegalArgumentException("invalid execution text");
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i >= text.length() || !Character.isLowSurrogate(text.charAt(i)))
                    throw new IllegalArgumentException("invalid execution Unicode");
            } else if (Character.isLowSurrogate(c)) throw new IllegalArgumentException("invalid execution Unicode");
        }
        return text;
    }
    private void check() { if (handle == 0) throw new IllegalStateException("file execution store closed"); }
    @Override public synchronized Execution.Snapshot open(String scope) {
        check(); checked(scope);
        String[] snapshot = nativeOpen(handle, scope);
        try { return new Execution.Snapshot(snapshot[0], snapshot[1]); }
        catch (RuntimeException | Error failure) {
            try { nativeClose(handle, scope); } catch (RuntimeException | Error ignored) { }
            throw failure;
        }
    }
    @Override public synchronized String commit(String scope, String revision, String stateJson) {
        check(); return nativeCommit(handle, checked(scope), checked(revision), checked(stateJson));
    }
    @Override public synchronized void close(String scope) { check(); nativeClose(handle, checked(scope)); }
    /** Release the native owner only while idle. Idempotent after successful close. */
    @Override public synchronized void close() {
        if (handle != 0) { nativeDestroy(handle); handle = 0; }
    }
    private static native boolean nativeAvailable();
    private static native long nativeCreate(String root);
    private static native String[] nativeOpen(long pointer, String scope);
    private static native String nativeCommit(long pointer, String scope, String revision, String stateJson);
    private static native void nativeClose(long pointer, String scope);
    private static native void nativeDestroy(long pointer);
}
