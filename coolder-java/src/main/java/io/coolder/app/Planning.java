package io.coolder.app;

import io.coolder.libai.CodingService;
import java.nio.file.*;
import java.util.*;

/** Java project index and durable validation checkpoints. */
final class Planning {
  static Map<String, Object> proposal(Map<String, Object> project, String goal, String scale) {
    List<Object> modules = new ArrayList<>();
    modules.add(
        Json.obj(
            "id",
            "main",
            "name",
            "Application",
            "layer",
            "application",
            "path",
            project.get("path"),
            "dependencies",
            new ArrayList<>()));
    List<Object> tasks = new ArrayList<>();
    String[] steps =
        scale.equals("quick")
            ? new String[] {"Implement the requested feature", "Validate and review"}
            : scale.equals("large")
                ? new String[] {
                  "Define requirements",
                  "Design interfaces",
                  "Implement core behavior",
                  "Integrate components",
                  "Add error handling",
                  "Validate and review"
                }
                : new String[] {
                  "Define requirements",
                  "Implement core behavior",
                  "Integrate components",
                  "Validate and review"
                };
    for (int i = 0; i < steps.length; i++) {
      String id = "task-" + (i + 1);
      tasks.add(
          Json.obj(
              "id",
              id,
              "title",
              steps[i] + ": " + goal,
              "module_id",
              "main",
              "status",
              "pending",
              "ready",
              i == 0,
              "dependencies",
              i == 0 ? new ArrayList<>() : Arrays.asList("task-" + i),
              "acceptance_criteria",
              Arrays.asList("Meet the project goal without unrelated file changes"),
              "test_plan",
              Arrays.asList("Run the project's available validation commands")));
    }
    return Json.obj(
        "ok",
        true,
        "proposal",
        true,
        "goal",
        goal,
        "scale",
        scale,
        "modules",
        modules,
        "tasks",
        tasks);
  }

  static Object workflow(Projects ps, Map<String, Object> a, Map<String, Object> b, String method)
      throws Exception {
    String id = Json.text(b, "project_id"),
        session = Json.text(b, "session_id"),
        task = Json.text(b, "task_id");
    Map<String, Object> p = ps.get(ps.records("projects", a), id);
    if (!session.isEmpty()) ps.get(ps.records("sessions", a), session);
    if (!task.isEmpty()
        && Json.list(p.get("tasks")).stream()
            .noneMatch(t -> Json.text(Json.map(t), "id").equals(task)))
      throw new App.Error(404, "task missing");
    if (!p.containsKey("workflows")) p.put("workflows", Json.obj());
    Map<String, Object> checkpoints = Json.map(p.get("workflows"));
    String key = session + ":" + task;
    if (method.equals("POST")) {
      if (Json.flag(b, "remove", false)) checkpoints.remove(key);
      else {
        List<Object> queue = b.containsKey("queue") ? Json.list(b.get("queue")) : new ArrayList<>(),
            diagnostics =
                b.containsKey("diagnostics") ? Json.list(b.get("diagnostics")) : new ArrayList<>();
        if (queue.size() > 32
            || queue.stream().anyMatch(v -> !(v instanceof String))
            || diagnostics.size() > 100) throw new App.Error(400, "invalid checkpoint");
        Map<String, Object> cp = new LinkedHashMap<>(b);
        cp.put("updated_at", System.currentTimeMillis() / 1000L);
        checkpoints.put(key, cp);
      }
      ps.save();
    }
    Map<String, Object> out =
        Json.obj(
            "ok",
            true,
            "found",
            checkpoints.containsKey(key),
            "removed",
            Json.flag(b, "remove", false));
    if (checkpoints.containsKey(key)) out.putAll(Json.map(checkpoints.get(key)));
    return out;
  }

  static Object index(Projects ps, Map<String, Object> a, Map<String, Object> b, boolean refresh)
      throws Exception {
    String id = Json.text(b, "project_id");
    if (id.isEmpty()) id = Json.text(b, "id");
    Map<String, Object> p = ps.get(ps.records("projects", a), id);
    if (!refresh && p.containsKey("index")) return p.get("index");
    Path root = ps.workspace(a).resolve(Json.text(p, "path"), false);
    List<Object> files = new ArrayList<>();
    int[] dirs = {0};
    scan(
        new Workspace(root, new ArrayList<>()),
        "",
        Json.text(p, "path"),
        Json.text(p, "language"),
        files,
        dirs,
        0);
    long rev =
        p.containsKey("index") ? Json.number(Json.map(p.get("index")), "revision", 0) + 1 : 1;
    Map<String, Object> index =
        Json.obj(
            "ok",
            true,
            "project_id",
            id,
            "path",
            p.get("path"),
            "revision",
            rev,
            "indexed_at",
            System.currentTimeMillis() / 1000L,
            "file_count",
            files.size(),
            "directory_count",
            dirs[0],
            "skipped_directory_count",
            0,
            "changed_file_count",
            files.size(),
            "truncated",
            files.size() >= 2000,
            "files",
            files,
            "entries_truncated",
            false);
    p.put("index", index);
    ps.save();
    return index;
  }

  static void scan(
      Workspace w,
      String path,
      String prefix,
      String language,
      List<Object> files,
      int[] dirs,
      int depth)
      throws Exception {
    if (depth > 12 || files.size() >= 2000) return;
    dirs[0]++;
    for (Object v : Json.list(Json.map(w.list(path)).get("entries"))) {
      Map<String, Object> e = Json.map(v);
      String rel = Json.text(e, "path");
      if (Json.flag(e, "directory", false)) {
        if (!Arrays.asList("node_modules", "build", "target", "var").contains(Json.text(e, "name")))
          scan(w, rel, prefix, language, files, dirs, depth + 1);
        continue;
      }
      List<Object> symbols = new ArrayList<>();
      try {
        Map<String, Object> outline =
            Json.map(
                Json.parse(
                    new CodingService(w.root.toString())
                        .call("workspace.outline", Json.encode(Json.obj("path", rel)))));
        if (outline.containsKey("items")) symbols = Json.list(outline.get("items"));
      } catch (Exception ignored) {
      }
      files.add(
          Json.obj(
              "path",
              prefix + "/" + rel,
              "module_id",
              "main",
              "kind",
              "source",
              "language",
              language,
              "size",
              e.get("size"),
              "modified_at",
              Files.getLastModifiedTime(w.resolve(rel, false)).toMillis() / 1000L,
              "symbol_count",
              symbols.size(),
              "symbols",
              symbols));
      if (files.size() >= 2000) return;
    }
  }
}
