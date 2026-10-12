package io.coolder.app;

import io.coolder.libai.CodingService;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Java job supervision over libai's fixed-command, one-time sandbox API. */
final class Sandbox {
  final Projects projects;
  final String helper;
  final ExecutorService workers = Executors.newFixedThreadPool(4);
  final Map<String, String> active = new ConcurrentHashMap<>();

  Sandbox(Projects p, String helper) {
    projects = p;
    this.helper = helper;
    for (String k : new String[] {"sandbox_runs", "sandbox_plans"})
      if (!p.state.containsKey(k)) p.state.put(k, Json.obj());
  }

  Map<String, Object> nativeCall(Path root, String op, Object b) {
    return Json.map(Json.parse(new CodingService(root.toString()).call(op, Json.encode(b))));
  }

  Object route(Map<String, Object> a, String op, Map<String, Object> b) throws Exception {
    if (op.equals("capabilities")) {
      Path root = projects.workspace(a).resolve(Json.text(b, "path"), true);
      Map<String, Object> caps = nativeCall(root, "sandbox.capabilities", Json.obj());
      caps.putAll(nativeCall(root, "projects.toolchain", Json.obj("project_path", "")));
      caps.putAll(
          Json.obj(
              "path",
              Json.text(b, "path"),
              "backend_available",
              Json.flag(caps, "available", false),
              "execution_enabled",
              Json.flag(caps, "available", false),
              "requires_confirmation",
              true,
              "backend_reason",
              Json.text(caps, "reason"),
              "browser_acceptance_enabled",
              false));
      return caps;
    }
    if (op.equals("plan")) {
      Path root = projects.workspace(a).resolve(Json.text(b, "path"), true);
      Map<String, Object> r =
          nativeCall(
              root,
              "sandbox.preview",
              Json.obj("project_path", "", "command_id", Json.text(b, "command_id")));
      if (!Json.flag(r, "ok", false)) throw new App.Error(400, Json.text(r, "error"));
      String id = Json.text(r, "id");
      Map<String, Object> plan = new LinkedHashMap<>(r);
      plan.putAll(
          Json.obj(
              "root",
              root.toString(),
              "owner",
              a.get("id"),
              "path",
              Json.text(b, "path"),
              "plan_id",
              id,
              "requires_confirmation",
              true,
              "expires_in_seconds",
              300));
      Json.map(projects.state.get("sandbox_plans")).put(id, plan);
      projects.save();
      Map<String, Object> out = new LinkedHashMap<>(plan);
      out.remove("root");
      out.remove("owner");
      return out;
    }
    if (op.equals("runs")) {
      List<Object> rows = new ArrayList<>();
      for (Object item : projects.records("sandbox_runs", a)) {
        Map<String, Object> r = new LinkedHashMap<>(Json.map(item));
        r.remove("stdout");
        r.remove("stderr");
        r.remove("root");
        rows.add(r);
      }
      return Json.obj("ok", true, "metadata_only", true, "runs", rows);
    }
    if (op.equals("runs/status")) {
      Map<String, Object> r =
          new LinkedHashMap<>(
              projects.get(projects.records("sandbox_runs", a), Json.text(b, "id")));
      r.remove("root");
      return r;
    }
    if (op.equals("runs/cancel")) {
      Map<String, Object> r =
          projects.get(projects.records("sandbox_runs", a), Json.text(b, "run_id"));
      if (!Json.flag(r, "running", false)) throw new App.Error(409, "sandbox already finished");
      r.put("cancel_requested", true);
      nativeCall(Paths.get(Json.text(r, "root")), "sandbox.cancel", Json.obj("id", r.get("id")));
      return Json.obj("ok", true, "cancel_requested", true, "run_id", r.get("id"));
    }
    if (!op.equals("execute")) throw new App.Error(404, "sandbox route missing");
    if (!Json.flag(b, "confirm", false)) throw new App.Error(400, "explicit confirmation required");
    String id = Json.text(b, "plan_id");
    Map<String, Object> plans = Json.map(projects.state.get("sandbox_plans"));
    if (!plans.containsKey(id)) throw new App.Error(404, "sandbox plan missing");
    Map<String, Object> p = Json.map(plans.get(id));
    if (!a.get("id").equals(p.get("owner"))) throw new App.Error(404, "sandbox plan missing");
    if (active.size() >= 4) throw new App.Error(429, "sandbox worker limit");
    plans.remove(id);
    Map<String, Object> r =
        Json.obj(
            "ok",
            true,
            "id",
            id,
            "run_id",
            id,
            "path",
            p.get("path"),
            "command_id",
            p.get("command_id"),
            "root",
            p.get("root"),
            "status",
            "queued",
            "running",
            true,
            "started_at",
            System.currentTimeMillis() / 1000L,
            "stdout",
            "",
            "stderr",
            "");
    projects.records("sandbox_runs", a).add(r);
    projects.save();
    active.put(id, Json.text(p, "root"));
    workers.submit(
        () -> {
          Map<String, Object> result;
          try {
            result =
                nativeCall(
                    Paths.get(Json.text(p, "root")),
                    "sandbox.execute",
                    Json.obj("id", id, "confirm", true, "helper", helper));
          } catch (Exception e) {
            result = Json.obj("ok", false, "error", e.toString());
          }
          synchronized (projects) {
            try {
              r.putAll(result);
              r.put("running", false);
              r.put(
                  "status",
                  Json.flag(r, "cancelled", false)
                      ? "cancelled"
                      : Json.flag(r, "ok", false) && Json.number(r, "exit_code", -1) == 0
                          ? "completed"
                          : "failed");
              r.put("finished_at", System.currentTimeMillis() / 1000L);
              r.remove("root");
              projects.save();
            } catch (Exception e) {
              System.err.println(e);
            }
          }
          active.remove(id);
        });
    return Json.obj(
        "ok", true, "run_id", id, "plan_id", id, "running", true, "started", true, "status",
        "queued");
  }

  void stop() {
    for (Map.Entry<String, String> e : active.entrySet())
      try {
        nativeCall(Paths.get(e.getValue()), "sandbox.cancel", Json.obj("id", e.getKey()));
      } catch (Exception ignored) {
      }
    workers.shutdownNow();
  }
}
