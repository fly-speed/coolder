package io.coolder.libai;

/** Host-owned durable execution. Storage and receipt confirmation are never model tools. */
public final class Execution {
    static { System.loadLibrary("ai_jni"); }
    private Execution() { }
    public static final class Snapshot {
        public final String revision, stateJson;
        public Snapshot(String revision, String stateJson) { this.revision = revision; this.stateJson = stateJson; }
    }
    /** Hold an exclusive scope lease between open and close. Host owns durable CAS,
     * authorization and encryption. Failed open cleans up only its own acquisition. */
    public interface Store {
        Snapshot open(String scope);
        String commit(String scope, String expectedRevision, String stateJson);
        /** Release reliably without throwing. Native close cannot report failure. */
        void close(String scope);
    }
    private static String checked(String value) {
        if (value == null || value.indexOf('\0') >= 0) throw new IllegalArgumentException("invalid execution text");
        for (int i = 0; i < value.length(); ++i) {
            char c = value.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i >= value.length() || !Character.isLowSurrogate(value.charAt(i)))
                    throw new IllegalArgumentException("invalid execution Unicode");
            } else if (Character.isLowSurrogate(c)) throw new IllegalArgumentException("invalid execution Unicode");
        }
        return value;
    }
    // Invoked synchronously by JNI. A failed open must not close someone else's lease.
    private static String[] dispatch(Store store, String operation, String scope, String revision, String state) {
        if (store == null) throw new IllegalArgumentException("execution store is null");
        if (operation.equals("open")) {
            Snapshot snapshot = store.open(scope);
            try {
                if (snapshot == null) throw new IllegalArgumentException("null snapshot");
                return new String[]{checked(snapshot.revision), checked(snapshot.stateJson)};
            } catch (RuntimeException | Error failure) {
                try { store.close(scope); } catch (RuntimeException | Error ignored) { }
                throw failure;
            }
        }
        if (operation.equals("commit")) return new String[]{checked(store.commit(scope, revision, state)), ""};
        if (operation.equals("close")) { store.close(scope); return new String[]{"", ""}; }
        throw new IllegalArgumentException("unknown execution operation");
    }
    /** Call only after the host verifies a definite external receipt. outcome is none/applied.
     * Stale revision, task or key is rejected. */
    public static void confirm(Store store, String scope, String input, String instructions,
            String expectedRevision, String key, Agent.ToolResult output, String outcome, String reference) {
        if (store == null || output == null) throw new IllegalArgumentException("null store/output");
        nativeConfirm(store, checked(scope), checked(input), checked(instructions), checked(expectedRevision),
                checked(key), checked(output.text), output.error, checked(outcome), checked(reference));
    }
    /** Trusted migration of an unfinished legacy log after verifying its remaining budget.
     * Zero means unlimited. Requires the current revision; does not reconcile effects. */
    public static void adoptDeadline(Store store, String scope, String input, String instructions,
            String expectedRevision, long remainingMillis, String reference) {
        if (store == null || remainingMillis < 0) throw new IllegalArgumentException("invalid store/budget");
        nativeAdoptDeadline(store, checked(scope), checked(input), checked(instructions),
                checked(expectedRevision), remainingMillis, checked(reference));
    }
    private static native void nativeAdoptDeadline(Store store, String scope, String input,
            String instructions, String revision, long remainingMillis, String reference);
    private static native void nativeConfirm(Store store, String scope, String input, String instructions,
            String revision, String key, String output, boolean error, String outcome, String reference);
}
