package io.coolder.app;

import io.coolder.libai.CodingService;
import java.net.URLDecoder;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.stream.*;

/** HTTP routing and application lifecycle implemented entirely in Java. */
final class App implements AutoCloseable {
  interface Sink {
    boolean write(int status, String headers, byte[] bytes);
  }

  static final class Error extends RuntimeException {
    final int status;

    Error(int status, String message) {
      super(message);
      this.status = status;
    }
  }

  final Auth auth;
  final Projects projects;
  final Runs runs;
  final Sandbox sandbox;
  final String authority;
  Map<String, Object> policy;
  final FileChannel lockFile;
  final FileLock lock;
  volatile boolean stopped;

  App(String configuration) throws Exception {
    Map<String, Object> cfg = Json.map(Json.parse(configuration));
    authority = Json.text(cfg, "authority");
    if (!Files.isRegularFile(Paths.get(Json.text(cfg, "html")).resolve("index.html")))
      throw new IllegalArgumentException("UI files missing");
    Path data = Paths.get(Json.text(cfg, "data")).toAbsolutePath().normalize();
    Store.privateDir(data);
    Path workspace =
        Paths.get(
                Json.text(cfg, "workspace").isEmpty()
                    ? data.resolve("workspace").toString()
                    : Json.text(cfg, "workspace"))
            .toAbsolutePath()
            .normalize();
    for (String privateName : new String[] {"auth", "users", "shared"})
      if (workspace.startsWith(data.resolve(privateName))
          || data.resolve(privateName).startsWith(workspace))
        throw new IllegalArgumentException("workspace overlaps private account storage");
    Store.privateDir(workspace);
    lockFile =
        FileChannel.open(
            data.resolve("server.lock"), StandardOpenOption.CREATE, StandardOpenOption.WRITE);
    lock = lockFile.tryLock();
    if (lock == null) throw new IllegalArgumentException("data directory already in use");
    Map<String, Object> migration =
        Json.map(Json.parse(new CodingService(data.toString()).call("storage.migrate", "{}")));
    if (!Json.flag(migration, "ok", false))
      throw new Error(400, "JSON storage migration failed: " + Json.text(migration, "error"));
    Map<String,Object> workspaceMigration=Json.map(Json.parse(new CodingService(workspace.toString()).call("storage.migrate","{}")));
    if(!Json.flag(workspaceMigration,"ok",false))throw new Error(400,"workspace naming migration failed: "+Json.text(workspaceMigration,"error"));
    auth =
        new Auth(data, workspace, Paths.get(Json.text(cfg, "html")).toAbsolutePath().normalize());
    projects = new Projects(auth);
    runs = new Runs(projects, Json.text(cfg, "sandbox_helper"));
    sandbox = new Sandbox(projects, Json.text(cfg, "sandbox_helper"));
    policy =
        Json.map(
            Store.shared(
                data.resolve(".libai_settings/ai-policy.v1"),
                data.resolve("auth/ai-policy-java.v1"),
                data.resolve("auth/ai-policy-rust.v1"),
                Json.obj(
                    "enabled",
                    true,
                    "allow_users",
                    true,
                    "allow_users_local_projects",
                    false,
                    "max_active_runs_per_user",
                    3)));
    if (!policy.containsKey("allow_browser_debug")) policy.put("allow_browser_debug", false);
    Map<String, Object> effective = runtimePolicy("runtime.policy.configure", policy);
    effective.putAll(policy);
    policy = effective;
    Store.write(auth.data.resolve(".libai_settings/ai-policy.v1"), policy);
  }

  Map<String, Object> runtimePolicy(String operation, Map<String, Object> candidate) {
    Map<String, Object> result =
        Json.map(Json.parse(runs.providers.call(operation, Json.encode(candidate))));
    if (!Json.flag(result, "ok", false)) throw new Error(400, Json.text(result, "error"));
    result.remove("ok");
    return result;
  }

  static String decode(String s) {
    try {
      String value = URLDecoder.decode(s, "UTF-8");
      if (value.indexOf('\0') >= 0) throw new Error(400, "NUL target");
      return value;
    } catch (java.io.UnsupportedEncodingException e) {
      throw new IllegalStateException(e);
    }
  }

  static Map<String, Object> query(String target) {
    Map<String, Object> b = Json.obj();
    int q = target.indexOf('?');
    if (q >= 0)
      for (String part : target.substring(q + 1).split("&")) {
        String[] kv = part.split("=", 2);
        String key = decode(kv[0]);
        if (b.containsKey(key)) throw new Error(400, "duplicate parameter");
        b.put(key, kv.length == 1 ? "" : decode(kv[1]));
      }
    return b;
  }

  static String fields(Map<String, String> m) {
    StringBuilder b = new StringBuilder();
    for (Map.Entry<String, String> e : m.entrySet())
      b.append(e.getKey()).append(": ").append(e.getValue()).append("\r\n");
    return b.toString();
  }

  void send(Sink sink, int status, Object value, Map<String, String> headers) {
    byte[] bytes = Json.encode(value).getBytes(StandardCharsets.UTF_8);
    headers.put("Content-Length", Integer.toString(bytes.length));
    headers.put("Content-Type", "application/json; charset=utf-8");
    sink.write(status, fields(headers), bytes);
  }

  boolean staticAsset(String path, Map<String, String> headers, Sink sink) throws Exception {
    boolean asset =
        path.equals("/")
            || path.matches("/(coolder|app|admin|i18n|personal|code-editor|attachments)\\.js")
            || path.equals("/style.css")
            || path.startsWith("/vendor/")
            || path.startsWith("/i18n/");
    if (!asset) return false;
    String relative = path.equals("/") ? "index.html" : path.substring(1);
    if (relative.contains("..") || !relative.matches("[A-Za-z0-9_./-]+"))
      throw new Error(404, "asset not found");
    Path f = auth.html;
    for (String c : relative.split("/")) {
      f = f.resolve(c);
      if (Files.isSymbolicLink(f)) throw new Error(404, "asset not found");
    }
    if (!Files.isRegularFile(f)) throw new Error(404, "asset not found");
    String ext = relative.substring(relative.lastIndexOf('.') + 1);
    String mime =
        ext.equals("js")
            ? "text/javascript"
            : ext.equals("css")
                ? "text/css"
                : ext.equals("html")
                    ? "text/html"
                    : ext.equals("json")
                        ? "application/json"
                        : ext.equals("ttf")
                            ? "font/ttf"
                            : ext.equals("woff")
                                ? "font/woff"
                                : ext.equals("woff2") ? "font/woff2" : "";
    if (mime.isEmpty() || (path.startsWith("/vendor/") && ext.equals("json")))
      throw new Error(404, "asset not found");
    byte[] bytes = Files.readAllBytes(f);
    if (path.equals("/")) {
      String page = new String(bytes, StandardCharsets.UTF_8);
      String csp = headers.get("Content-Security-Policy"),
          nonce = csp.substring(csp.indexOf("nonce-") + 6, csp.indexOf("'; style-src"));
      bytes =
          page.replace(
                  "</head>",
                  "<meta name=\"preview-script-nonce\" content=\"" + nonce + "\">\n</head>")
              .getBytes(StandardCharsets.UTF_8);
    }
    headers.put(
        "Content-Type",
        mime
            + ((ext.equals("js") || ext.equals("html") || ext.equals("css") || ext.equals("json"))
                ? "; charset=utf-8"
                : ""));
    headers.put("Content-Length", Integer.toString(bytes.length));
    sink.write(200, fields(headers), bytes);
    return true;
  }

  void dispatch(String method, String target, String headerBlock, byte[] body, Sink sink) {
    Map<String, String> out = new LinkedHashMap<>();
    out.put("X-Content-Type-Options", "nosniff");
    out.put("Cache-Control", "no-store");
    out.put(
        "Content-Security-Policy",
        "default-src 'self'; script-src 'self' 'nonce-"
            + Store.id().substring(0, 32)
            + "'; style-src 'self' 'unsafe-inline'; worker-src 'self'; font-src 'self' data:; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'");
    try {
      if (stopped) throw new Error(503, "stopped");
      Map<String, String> headers = new HashMap<>();
      for (String line : headerBlock.split("\r\n")) {
        int colon = line.indexOf(':');
        if (colon > 0)
          headers.put(
              line.substring(0, colon).toLowerCase(Locale.ROOT), line.substring(colon + 1).trim());
      }
      if (!authority.equals(headers.get("host"))) throw new Error(403, "invalid Host");
      if (headers.containsKey("origin") && !headers.get("origin").equals("http://" + authority))
        throw new Error(403, "cross-origin request denied");
      String path = decode(target.split("\\?", 2)[0]);
      Map<String, Object> b = query(target);
      verifyMethod(path, method);
      if (method.equals("POST")) {
        String content = headers.getOrDefault("content-type", "").split(";", 2)[0].trim();
        if (!content.equalsIgnoreCase("application/json") && !content.equalsIgnoreCase("text/json"))
          throw new Error(400, "JSON Content-Type required");
        b = Json.map(Json.parse(new String(body, StandardCharsets.UTF_8)));
      }
      if (path.startsWith("/api/v1/auth/")
          || path.equals("/api/login")
          || path.equals("/api/logout")) {
        String op = path.startsWith("/api/v1/auth/") ? path.substring(13) : path.substring(5);
        Object result = auth.route(op, method, b, headers, out);
        if (op.equals("status")) {
          Map<String, Object> status = Json.map(result);
          synchronized (projects) {
            status.put("ai_agent_allowed",
                Json.flag(status, "authenticated", false)
                    && Json.flag(policy, "enabled", true)
                    && Json.flag(policy,
                        Json.flag(status, "admin", false) ? "allow_admin" : "allow_users", true));
          }
        }
        send(sink, 200, result, out);
        return;
      }
      if (method.equals("GET") && staticAsset(path, out, sink)) return;
      Map<String, Object> a = auth.current(headers);
      auth.require(a);
      if (path.equals("/api/health")) {
        send(sink, 200, Json.obj("ok", true, "app", "coolder-java", "version", "1.0.0"), out);
        return;
      }
      if (path.equals("/api/v1/admin/ai-policy")) {
        auth.admin(a);
        synchronized (projects) {
          if (method.equals("POST")) {
            Map<String, Object> candidate = new LinkedHashMap<>(policy);
            candidate.putAll(b);
            validatePolicy(candidate);
            runtimePolicy("runtime.policy.validate", candidate);
            Store.write(auth.data.resolve(".libai_settings/ai-policy.v1"), candidate);
            runtimePolicy("runtime.policy.configure", candidate);
            policy = candidate;
          }
          send(sink, 200, policy, out);
        }
        return;
      }
      if (path.startsWith("/api/v1/ai/")
          && !path.startsWith("/api/v1/ai/providers")
          && (!Json.flag(policy, "enabled", true)
              || (!Json.flag(a, "admin", false) && !Json.flag(policy, "allow_users", true))))
        throw new Error(403, "AI access disabled");
      String op = path.startsWith("/api/v1/ai/") ? path.substring(11) : "";
      if (op.equals("runs/events")) {
        stream(a, Json.text(b, "id"), sink, out);
        return;
      }
      synchronized (projects) {
        Object result;
        int status = 200;
        if (path.startsWith("/api/v1/collaborate/"))
          result = projects.collaborate(a, path.substring(20), method, b);
        else if (op.equals("attachments") || op.equals("attachments/discard"))
          result = projects.attachments(a, b, op.endsWith("discard"));
        else if (op.startsWith("providers")) {
          if (!op.equals("providers")) auth.admin(a);
          String nativeOp =
              op.equals("providers")
                  ? "providers.list"
                  : op.equals("providers/delete")
                      ? "providers.remove"
                      : op.equals("providers/test") ? "providers.test" : "providers.save";
          Map<String, Object> r =
              Json.map(Json.parse(runs.providers.call(nativeOp, Json.encode(b))));
          if (!Json.flag(r, "ok", false)) throw new Error(400, Json.text(r, "error"));
          result = op.equals("providers/save") ? Json.obj("ok", true, "provider", r) : r;
        } else if (op.equals("agents"))
          result = Json.parse(runs.providers.call("agents.list", "{}"));
        else if (op.equals("workspace/project/create")
            || op.equals("projects/ensure")
            || op.equals("projects/import-git"))
          result = projects.create(a, b, policy, op.equals("workspace/project/create"));
        else if (op.equals("projects/directories")) {
          if (!Json.flag(a, "admin", false)
              && !Json.flag(policy, "allow_users_local_projects", false))
            throw new Error(403, "local access disabled");
          String p = Json.text(b, "path");
          if (p.isEmpty()) p = System.getProperty("user.home");
          Path dir = Paths.get(p);
          if (!dir.isAbsolute() || !Files.isDirectory(dir) || Files.isSymbolicLink(dir))
            throw new Error(400, "invalid directory");
          List<Object> entries = new ArrayList<>();
          try (Stream<Path> s = Files.list(dir)) {
            for (Path f : s.sorted().collect(Collectors.toList()))
              if (Files.isDirectory(f) && !Files.isSymbolicLink(f))
                entries.add(
                    Json.obj(
                        "name",
                        f.getFileName().toString(),
                        "path",
                        f.toString(),
                        "directory",
                        true));
          }
          result = Json.obj("path", dir.toRealPath().toString(), "entries", entries);
        } else if (op.equals("projects"))
          result =
              Json.text(b, "id").isEmpty()
                  ? Json.obj(
                      "ok",
                      true,
                      "projects",
                      projects.records("projects", a),
                      "local_project_location_allowed",
                      Json.flag(a, "admin", false)
                          || Json.flag(policy, "allow_users_local_projects", false),
                      "shared_project_location_allowed",
                      false)
                  : projects.get(projects.records("projects", a), Json.text(b, "id"));
        else if (op.equals("projects/delete")) {
          Map<String, Object> p =
              projects.get(projects.records("projects", a), Json.text(b, "project_id"));
          projects.records("projects", a).remove(p);
          projects.save();
          result = Json.obj("ok", true);
        } else if (op.equals("projects/plan") || op.equals("projects/plan/propose"))
          result = projects.plan(a, b, op.endsWith("propose"));
        else if (op.equals("projects/tasks/status")) {
          Map<String, Object> p =
              projects.get(projects.records("projects", a), Json.text(b, "project_id"));
          if (Json.number(b, "plan_version", -1) != Json.number(p, "plan_version", 1))
            throw new Error(409, "plan changed");
          for (Object t : Json.list(p.get("tasks")))
            if (Json.text(Json.map(t), "id").equals(Json.text(b, "task_id")))
              Json.map(t).put("status", Json.text(b, "status"));
          p.put("plan_version", Json.number(p, "plan_version", 1) + 1);
          projects.save();
          result = p;
        } else if (op.equals("projects/workflow"))
          result = Planning.workflow(projects, a, b, method);
        else if (op.equals("projects/index") || op.equals("projects/index/refresh"))
          result = Planning.index(projects, a, b, op.endsWith("refresh"));
        else if (op.equals("workspace/list"))
          result = projects.workspace(a).list(Json.text(b, "path"));
        else if (op.equals("workspace/read"))
          result = projects.workspace(a).read(Json.text(b, "path"));
        else if (op.equals("workspace/search"))
          result = projects.workspace(a).search(Json.text(b, "path"), Json.text(b, "query"));
        else if (op.equals("workspace/directory/create")) {
          Files.createDirectory(projects.workspace(a).resolve(Json.text(b, "path"), false));
          result = Json.obj("ok", true);
        } else if (op.startsWith("workspace/patch/"))
          result = projects.patch(a, b, op.endsWith("apply"));
        else if (op.startsWith("workspace/change-set/")) {
          Map<String, Object> r =
              Json.map(
                  Json.parse(
                      new CodingService(auth.root(a).toString())
                          .call(
                              op.endsWith("preview") ? "changes.preview" : "changes.apply",
                              Json.encode(
                                  op.endsWith("preview")
                                      ? b
                                      : Json.obj("id", Json.text(b, "change_set_id"))))));
          if (!Json.flag(r, "ok", false))
            throw new Error(op.endsWith("apply") ? 409 : 400, Json.text(r, "error"));
          result = r;
        } else if (op.equals("runs/start")) {
          result = runs.start(a, b, policy);
          status = 202;
        } else if (op.equals("runs/recovery")) result = runs.recovery(a, Json.text(b, "id"));
        else if (op.equals("runs/status")) result = runs.record(a, Json.text(b, "id"));
        else if (op.equals("runs"))
          result = Json.obj("ok", true, "runs", projects.records("runs", a));
        else if (op.startsWith("runs/")) {
          result = runs.control(a, op.substring(5), b);
          if (op.equals("runs/cancel") || op.equals("runs/pause")) status = 202;
        } else if (op.equals("sessions")) {
          List<Object> ss = new ArrayList<>();
          for (Object v : projects.records("sessions", a))
            if (Json.text(b, "project_id").isEmpty()
                || Json.text(Json.map(v), "project_id").equals(Json.text(b, "project_id")))
              ss.add(v);
          result = Json.obj("ok", true, "sessions", ss);
        } else if (op.equals("sessions/save")) {
          String id = Json.text(b, "session_id");
          Map<String, Object> s = projects.get(projects.records("sessions", a), id);
          s.put("title", Json.text(b, "title"));
          projects.save();
          result = s;
        } else if (op.equals("sessions/delete")) {
          projects
              .records("sessions", a)
              .remove(projects.get(projects.records("sessions", a), Json.text(b, "session_id")));
          projects.save();
          result = Json.obj("ok", true);
        } else if (op.startsWith("sandbox/")) {
          result = sandbox.route(a, op.substring(8), b);
          if (op.equals("sandbox/execute") || op.equals("sandbox/runs/cancel")) status = 202;
        } else throw new Error(404, "route not found");
        send(sink, status, result, out);
      }
    } catch (Error e) {
      send(sink, e.status, Json.obj("ok", false, "error", e.getMessage()), out);
    } catch (IllegalArgumentException e) {
      send(sink, 400, Json.obj("ok", false, "error", e.getMessage()), out);
    } catch (Exception e) {
      System.err.println(e);
      send(sink, 500, Json.obj("ok", false, "error", "application operation failed"), out);
    }
  }

  void verifyMethod(String path, String method) {
    boolean both =
        path.equals("/api/v1/auth/preferences")
            || path.equals("/api/v1/admin/ai-policy")
            || path.equals("/api/v1/collaborate/settings")
            || path.equals("/api/v1/ai/projects/workflow");
    boolean post =
        path.equals("/api/login")
            || path.equals("/api/logout")
            || path.matches(
                "/api/v1/auth/(register|login|logout|password|users/create|users/update)")
            || path.equals("/api/v1/collaborate/save")
            || path.equals("/api/v1/ai/attachments")
            || path.endsWith("/discard")
            || path.matches(
                "/api/v1/ai/(providers/(save|delete|test)|workspace/(project/create|directory/create|patch/.*|change-set/.*)|runs/(start|cancel|pause|result/.*|reasoning/save)|sessions/(save|delete)|projects/(delete|import-git|ensure|plan|plan/propose|tasks/status|index/refresh)|sandbox/(plan|execute|runs/cancel))");
    if (!both && !method.equals(post ? "POST" : "GET")) throw new Error(405, "method not allowed");
  }

  void validatePolicy(Map<String, Object> p) {
    if (Json.flag(p, "allow_browser_debug", false)
        || Json.flag(p, "allow_users_shared_projects", false)
        || Json.number(p, "sandbox_memory_mib", 512) < 64
        || Json.number(p, "read_chunk_kib", 16) > Json.number(p, "read_file_limit_kib", 512))
      throw new Error(400, "unsupported or invalid policy");
    for (Map.Entry<String, Object> e : p.entrySet())
      if (e.getValue() instanceof Number && ((Number) e.getValue()).longValue() < 0)
        throw new Error(400, "negative policy limit");
  }

  void stream(Map<String, Object> a, String id, Sink sink, Map<String, String> headers)
      throws Exception {
    runs.record(a, id);
    headers.put("Content-Type", "text/event-stream");
    headers.put("Cache-Control", "no-cache");
    if (!sink.write(200, fields(headers), new byte[0])) return;
    while (!stopped) {
      String value, status;
      synchronized (projects) {
        Map<String, Object> r = runs.record(a, id);
        value = Json.encode(r);
        status = Json.text(r, "status");
      }
      if (!sink.write(
              200, "", ("event: status\ndata: " + value + "\n\n").getBytes(StandardCharsets.UTF_8))
          || !status.equals("running")) break;
      Thread.sleep(100);
    }
  }

  void stop() {
    stopped = true;
    runs.stop();
    sandbox.stop();
  }

  @Override
  public void close() throws Exception {
    stop();
    lock.release();
    lockFile.close();
  }
}
