package io.coolder.app;

import io.coolder.libai.CodingService;
import java.nio.file.*;
import java.util.*;

/** Native Java project planning, membership records, patch previews and attachment drafts. */
final class Projects {
  final Auth auth;
  final Path file;
  final Map<String, Object> state;

  Projects(Auth auth) throws Exception {
    this.auth = auth;
    file = auth.data.resolve("auth/app-state.v1");
    state =
        Json.map(
            Store.shared(
                file,
                auth.data.resolve("auth/app-state-java.v1"),
                auth.data.resolve("auth/app-state-rust.v1"),
                Json.obj(
                    "projects",
                    Json.obj(),
                    "sessions",
                    Json.obj(),
                    "runs",
                    Json.obj(),
                    "grants",
                    Json.obj(),
                    "patches",
                    Json.obj())));
    for (String category : Arrays.asList("projects", "sessions", "runs", "grants", "patches"))
      Json.map(state.get(category));
    save();
  }

  void save() throws Exception {
    Store.write(file, state);
  }

  List<Object> records(String type, Map<String, Object> a) {
    Map<String, Object> all = Json.map(state.get(type));
    String id = Json.text(a, "id");
    if (!all.containsKey(id)) all.put(id, new ArrayList<>());
    return Json.list(all.get(id));
  }

  Map<String, Object> get(List<Object> rs, String id) {
    for (Object v : rs) {
      Map<String, Object> r = Json.map(v);
      if (id.equals(Json.text(r, "project_id")) || id.equals(Json.text(r, "id"))) return r;
    }
    throw new App.Error(404, "record not found");
  }

  Workspace workspace(Map<String, Object> a) throws Exception {
    return new Workspace(auth.root(a), records("projects", a));
  }

  Map<String, Object> create(
      Map<String, Object> a, Map<String, Object> b, Map<String, Object> policy, boolean initialize)
      throws Exception {
    String requested = Json.text(b, "path"), language = Json.text(b, "language");
    if (language.isEmpty()) language = "python";
    if (Json.text(b, "storage_scope").equals("shared"))
      throw new App.Error(403, "shared storage disabled");
    if (initialize && !Json.flag(b, "confirm", false))
      throw new App.Error(400, "confirmation required");
    boolean external = Paths.get(requested).isAbsolute();
    if (external
        && !Json.flag(a, "admin", false)
        && !Json.flag(policy, "allow_users_local_projects", false))
      throw new App.Error(403, "local project access denied");
    List<Object> ps = records("projects", a);
    for (Object v : ps) {
      Map<String, Object> p = Json.map(v);
      if (requested.equals(Json.text(p, "path")) || requested.equals(Json.text(p, "external"))) {
        if (initialize) throw new App.Error(409, "project exists");
        return p;
      }
    }
    Path physical =
        external
            ? Paths.get(requested).toAbsolutePath().normalize()
            : workspace(a).resolve(requested, false);
    if (external) {
      Path ancestor = physical;
      while (ancestor != null) {
        if (Files.isSymbolicLink(ancestor)
            && !ancestor.equals(Paths.get("/var"))
            && !ancestor.equals(Paths.get("/tmp"))) throw new App.Error(400, "symlink path");
        ancestor = ancestor.getParent();
      }
      physical = physical.getParent().toRealPath().resolve(physical.getFileName());
      if (physical.startsWith(auth.data.toRealPath()) && !physical.startsWith(auth.workspace))
        throw new App.Error(400, "private storage");
    }
    if (initialize) {
      if (Files.exists(physical)) throw new App.Error(409, "project exists");
      Files.createDirectories(physical);
      Map<String, Object> scaffold =
          Json.map(
              Json.parse(
                  new CodingService(physical.getParent().toString())
                      .call(
                          "projects.scaffold",
                          Json.encode(
                              Json.obj(
                                  "project_path",
                                  physical.getFileName().toString(),
                                  "language",
                                  language,
                                  "platform",
                                  Json.text(b, "platform").isEmpty()
                                      ? "cross-platform"
                                      : b.get("platform"))))));
      if (!Json.flag(scaffold, "ok", false)) throw new App.Error(400, Json.text(scaffold, "error"));
    } else if (!Files.isDirectory(physical)) throw new App.Error(400, "project directory missing");
    String id = Store.id(), path = external ? "external/" + id : requested;
    Map<String, Object> p =
        Json.obj(
            "ok",
            true,
            "project_id",
            id,
            "id",
            id,
            "title",
            physical.getFileName().toString(),
            "path",
            path,
            "project_path",
            path,
            "language",
            language,
            "platform",
            Json.text(b, "platform"),
            "external",
            external ? physical.toString() : "",
            "storage_scope",
            external ? "local" : "private",
            "planning_available",
            true,
            "plan_seeded",
            true,
            "plan_version",
            1,
            "goal",
            "",
            "modules",
            new ArrayList<>(
                Arrays.asList(Json.obj("id", "main", "title", "Main module", "path", path))),
            "tasks",
            new ArrayList<>());
    Map<String, Object> seed = Planning.proposal(p, "Build " + Json.text(p, "title"), "standard");
    p.put("goal", seed.get("goal"));
    p.put("modules", seed.get("modules"));
    p.put("tasks", seed.get("tasks"));
    p.put("status", "active");
    p.put("created_at", System.currentTimeMillis() / 1000L);
    p.put("updated_at", System.currentTimeMillis() / 1000L);
    ps.add(p);
    save();
    return p;
  }

  Object plan(Map<String, Object> a, Map<String, Object> b, boolean propose) throws Exception {
    Map<String, Object> p = get(records("projects", a), Json.text(b, "project_id"));
    if (propose) return Planning.proposal(p, Json.text(b, "goal"), Json.text(b, "scale"));
    if (Json.number(b, "plan_version", -1) != Json.number(p, "plan_version", 1))
      throw new App.Error(409, "plan changed");
    for (String k : new String[] {"goal", "modules", "tasks"})
      if (b.containsKey(k)) p.put(k, b.get(k));
    p.put("plan_version", Json.number(p, "plan_version", 1) + 1);
    save();
    return p;
  }

  Object patch(Map<String, Object> a, Map<String, Object> b, boolean apply) throws Exception {
    Map<String, Object> patches = Json.map(state.get("patches"));
    Workspace w = workspace(a);
    if (!apply) {
      String path = Json.text(b, "path");
      Map<String, Object> r = Json.map(w.read(path));
      String id = Store.id();
      Map<String, Object> p =
          Json.obj(
              "id",
              id,
              "patch_id",
              id,
              "path",
              path,
              "content",
              Json.text(b, "content"),
              "expected_sha256",
              r.get("sha256"),
              "owner",
              a.get("id"),
              "diff",
              "--- " + path + "\n+++ " + path + "\n" + Json.text(b, "content"));
      patches.put(id, p);
      save();
      return p;
    }
    String id = Json.text(b, "patch_id");
    if (id.isEmpty()) id = Json.text(b, "id");
    Map<String, Object> p = patches.containsKey(id) ? Json.map(patches.get(id)) : null;
    if (p == null || !a.get("id").equals(p.get("owner")))
      throw new App.Error(404, "preview missing");
    w.replace(Json.text(p, "path"), Json.text(p, "expected_sha256"), Json.text(p, "content"));
    patches.remove(id);
    save();
    return Json.obj("ok", true);
  }

  Object attachments(Map<String, Object> a, Map<String, Object> b, boolean discard)
      throws Exception {
    Path root = auth.root(a), base = root.resolve(".libai_agent/attachments");
    Store.privateDir(base);
    if (discard) {
      String draft = Json.text(b, "attachment_draft");
      if (!draft.matches("\\.libai_agent/attachments/[a-f0-9]{64}"))
        throw new App.Error(400, "invalid attachment draft");
      Path p = root.resolve(draft);
      if (Files.isDirectory(p) && !Files.isSymbolicLink(p)) {
        try (java.util.stream.Stream<Path> s = Files.walk(p)) {
          for (Path f :
              s.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))
            Files.delete(f);
        }
      }
      return Json.obj("ok", true);
    }
    List<Object> files = Json.list(b.get("files"));
    if (files.isEmpty() || files.size() > 8)
      throw new App.Error(400, "1 to 8 attachments required");
    List<byte[]> bytes = new ArrayList<>();
    List<String> names = new ArrayList<>();
    for (Object v : files) {
      Map<String, Object> f = Json.map(v);
      String name = Json.text(f, "name");
      if (!name.matches("[A-Za-z0-9 _.-]{1,120}") || name.equals(".") || name.equals(".."))
        throw new App.Error(400, "invalid attachment name");
      byte[] data;
      try {
        data = Base64.getDecoder().decode(Json.text(f, "base64"));
      } catch (Exception e) {
        throw new App.Error(400, "invalid base64");
      }
      boolean image = name.matches("(?i).*\\.(png|jpg|jpeg|gif|webp)");
      if (data.length > (image ? 8 * 1024 * 1024 : 32768)
          || (!image
              && new String(data, java.nio.charset.StandardCharsets.UTF_8).indexOf('\0') >= 0))
        throw new App.Error(400, "invalid attachment size/content");
      bytes.add(data);
      names.add(name);
    }
    String draft = ".libai_agent/attachments/" + Store.id();
    Store.privateDir(root.resolve(draft));
    List<Object> paths = new ArrayList<>();
    for (int i = 0; i < bytes.size(); i++) {
      String p = draft + "/" + i + "-" + names.get(i);
      Files.write(root.resolve(p), bytes.get(i));
      paths.add(p);
    }
    return Json.obj("ok", true, "attachment_draft", draft, "attachments", paths);
  }

  String permission(Map<String, Object> owner, Map<String, Object> actor, String project) {
    return owner.get("id").equals(actor.get("id"))
        ? "owner"
        : Json.text(
            Json.map(state.get("grants")), owner.get("id") + ":" + project + ":" + actor.get("id"));
  }

  Object collaborate(Map<String, Object> a, String op, String method, Map<String, Object> b)
      throws Exception {
    if (op.equals("projects")) {
      List<Object> ps = new ArrayList<>();
      for (Object v : auth.snapshot()) {
        Map<String, Object> owner = Json.map(v);
        if (!Json.flag(owner, "enabled", true)) continue;
        for (Object item : records("projects", owner)) {
          Map<String, Object> p = Json.map(item);
          String access = permission(owner, a, Json.text(p, "project_id"));
          if (access.isEmpty()) continue;
          Map<String, Object> out = new LinkedHashMap<>(p);
          out.putAll(
              Json.obj(
                  "owner",
                  owner.get("username"),
                  "owner_id",
                  owner.get("id"),
                  "permission",
                  access,
                  "shared",
                  !access.equals("owner")));
          ps.add(out);
        }
      }
      return Json.obj("ok", true, "projects", ps);
    }
    Map<String, Object> owner = null;
    for (Object v : auth.snapshot())
      if (Json.text(Json.map(v), "id").equals(Json.text(b, "owner_id"))) owner = Json.map(v);
    if (owner == null || !Json.flag(owner, "enabled", true))
      throw new App.Error(404, "owner missing");
    String access = permission(owner, a, Json.text(b, "project_id"));
    if (access.isEmpty()) throw new App.Error(403, "project access denied");
    Map<String, Object> p = get(records("projects", owner), Json.text(b, "project_id"));
    if (op.equals("settings")) {
      if (method.equals("POST")) {
        if (!access.equals("owner")) throw new App.Error(403, "owner required");
        String request = Json.text(b, "permission"), name = Json.text(b, "username");
        if (!Arrays.asList("read", "write", "remove").contains(request))
          throw new App.Error(400, "invalid permission");
        if (auth.account(name) == null) throw new App.Error(404, "user missing");
        Map<String, Object> target = auth.account(name);
        if (!Json.flag(target, "enabled", true) && !request.equals("remove"))
          throw new App.Error(404, "disabled user");
        if (target.get("id").equals(owner.get("id")))
          throw new App.Error(400, "owner permission immutable");
        String key = owner.get("id") + ":" + p.get("project_id") + ":" + target.get("id");
        Map<String, Object> grants = Json.map(state.get("grants"));
        if (request.equals("remove")) grants.remove(key);
        else grants.put(key, request);
        save();
      }
      List<Object> users = new ArrayList<>(), members = new ArrayList<>();
      for (Object v : auth.snapshot()) {
        Map<String, Object> u = Json.map(v);
        String perm = permission(owner, u, Json.text(p, "project_id"));
        Map<String, Object> item =
            Json.obj(
                "username", u.get("username"), "permission", perm, "enabled", u.get("enabled"));
        users.add(item);
        if (!perm.isEmpty()) members.add(item);
      }
      Map<String, Object> out = Json.obj("ok", true, "members", members, "permission", access);
      if (access.equals("owner")) out.put("users", users);
      return out;
    }
    Path projectRoot = workspace(owner).resolve(Json.text(p, "path"), false);
    Workspace w = new Workspace(projectRoot, new ArrayList<>());
    String path = Json.text(b, "path");
    switch (op) {
      case "list":
        return w.list(path);
      case "read":
        return w.read(path);
      case "preview-asset":
        return w.asset(path);
      case "save":
        if (access.equals("read")) throw new App.Error(403, "read-only project");
        w.replace(path, Json.text(b, "sha256"), Json.text(b, "content"));
        return Json.obj("ok", true);
      default:
        throw new App.Error(404, "route missing");
    }
  }
}
