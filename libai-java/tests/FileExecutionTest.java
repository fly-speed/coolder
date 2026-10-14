import io.coolder.libai.Agent;
import io.coolder.libai.Execution;
import io.coolder.libai.FileExecutionStore;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.concurrent.TimeUnit;

public final class FileExecutionTest {
    static void check(boolean value) { if (!value) throw new AssertionError(); }
    static int requests;
    static String run(FileExecutionStore store) {
        try (Agent agent = new Agent((request, invocation) -> { requests++; return new Agent.Response("done"); })) {
            agent.addTool("write", "", "{}", false, (call, invocation) -> { throw new AssertionError("duplicate effect"); });
            agent.setExecutionStore("crash", store);
            return agent.run("input");
        }
    }
    static void worker(String root) {
        try (FileExecutionStore store = FileExecutionStore.plaintext(root);
             Agent agent = new Agent((request, invocation) -> {
                Agent.Response response = new Agent.Response();
                response.calls.add(new Agent.ToolCall("a", "write", "{}")); return response;
             })) {
            agent.addTool("write", "", "{}", false, (call, invocation) -> {
                check(invocation.idempotencyKey().equals("crash.0"));
                try (FileOutputStream output = new FileOutputStream(Paths.get(root, "effect").toFile(), true)) {
                    output.write('x'); output.getFD().sync();
                } catch (Exception error) { throw new RuntimeException(error); }
                Runtime.getRuntime().halt(77);
                throw new AssertionError("halt returned");
            });
            agent.setExecutionStore("crash", store); agent.run("input");
        }
        throw new AssertionError("worker completed");
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 0) { worker(args[0]); return; }
        check(FileExecutionStore.available());
        Path root = Files.createTempDirectory("libai-java-file-",
                PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        try {
            try (FileExecutionStore first = FileExecutionStore.plaintext(root.toString());
                 FileExecutionStore second = FileExecutionStore.plaintext(root.toString())) {
                check(first.open("task").revision.isEmpty());
                try { first.close(); throw new AssertionError(); } catch (IllegalStateException expected) { }
                try { second.open("task"); throw new AssertionError(); } catch (IllegalStateException expected) { }
                check(first.commit("task", "", "{\"text\":\"你好🌏\"}").equals("1"));
                try { first.close("wrong"); throw new AssertionError(); } catch (IllegalStateException expected) { }
                try { second.open("task"); throw new AssertionError(); } catch (IllegalStateException expected) { }
                first.close("task");
                Execution.Snapshot snapshot = second.open("task");
                check(snapshot.revision.equals("1") && snapshot.stateJson.equals("{\"text\":\"你好🌏\"}"));
                try { second.commit("task", "stale", "{}"); throw new AssertionError(); } catch (IllegalStateException expected) { }
                second.close("task");
                check(new String(Files.readAllBytes(root.resolve("task.state")), StandardCharsets.UTF_8).startsWith("{\"version\":1,"));
            }
            String java = Paths.get(System.getProperty("java.home"), "bin", "java").toString();
            Process child = new ProcessBuilder(java, "-Xcheck:jni", "-Djava.library.path=" + System.getProperty("java.library.path"),
                    "-cp", System.getProperty("java.class.path"), "FileExecutionTest", root.toString()).inheritIO().start();
            if (!child.waitFor(20, TimeUnit.SECONDS)) { child.destroyForcibly(); throw new AssertionError("worker timeout"); }
            check(child.exitValue() == 77);
            try (FileExecutionStore store = FileExecutionStore.plaintext(root.toString())) {
                check(run(store).contains("recovery_required") && requests == 0);
                Execution.Snapshot snapshot = store.open("crash"); store.close("crash");
                Execution.confirm(store, "crash", "input", "", snapshot.revision, "crash.0",
                        new Agent.ToolResult("written"), "applied", "effect/one-byte");
                check(run(store).contains("completed") && requests == 1);
                check(run(store).contains("done") && requests == 1);
                check(Files.size(root.resolve("effect")) == 1);
            }
        } finally {
            try (java.util.stream.Stream<Path> files = Files.list(root)) {
                for (Path file : (Iterable<Path>) files::iterator) Files.delete(file);
            }
            Files.delete(root);
        }
        System.out.println("Java file execution tests passed");
    }
}
