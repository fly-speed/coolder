package io.coolder.app;

import io.coolder.libai.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Java-owned asynchronous agent jobs, replay, drafts and review state. */
final class Runs {
  final Projects projects;
  final CodingService providers;
  final ConcurrentHashMap<String, Job> jobs = new ConcurrentHashMap<>();
  final ExecutorService executor = Executors.newFixedThreadPool(16);
  static final String[] TOOLS = {
    "workspace_list",
    "workspace_read",
    "workspace_search",
    "workspace_outline",
    "workspace_propose",
    "workspace_propose_mkdir",
    "workspace_propose_delete",
    "workspace_propose_move",
    "project_plan",
    "project_tasks",
    "toolchain_commands",
    "project_diagnostics"
  };

  static final class Job {
    final Map<String, Object> record, actor;
    final Agent.Cancellation cancellation = new Agent.Cancellation();
    volatile boolean paused;
    String terminalStatus = "failed";

    Job(Map<String, Object> r, Map<String, Object> a) {
      record = r;
      actor = a;
    }
  }

  Runs(Projects p) throws Exception {
    projects = p;
    providers = new CodingService(p.auth.workspace.toString());
    for (Object list : Json.map(p.state.get("runs")).values())
      for (Object item : Json.list(list)) {
        Map<String, Object> r = Json.map(item);
        normalizeChanges(r);
        if (Json.text(r, "status").equals("running")) {
          r.put("status", "interrupted");
          r.put("recovery_available", r.containsKey("checkpoint"));
        }
      }
    p.save();
  }

  // One current review draft per path. Preserve superseded source for auditing.
  static void normalizeChanges(Map<String, Object> run) {
    if (!(run.get("changes") instanceof List)) return;
    List<Object> changes = Json.list(run.get("changes"));
    Map<String, Map<String, Object>> latest = new LinkedHashMap<>();
    List<Object> superseded = run.containsKey("superseded_changes")
        ? Json.list(run.get("superseded_changes")) : new ArrayList<>();
    for (Object item : changes) {
      Map<String, Object> change = Json.map(item);
      Map<String, Object> previous = latest.put(Json.text(change, "path"), change);
      if (previous != null) {
        superseded.add(new LinkedHashMap<>(previous));
        change.put("generation", Math.max(Json.number(change, "generation", 1),
            Json.number(previous, "generation", 1) + 1));
        if (Json.text(previous, "review_status").equals("pending"))
          change.put("original_hash", previous.get("original_hash"));
      }
    }
    if (latest.size() != changes.size()) {
      run.put("changes", new ArrayList<>(latest.values()));
      run.put("superseded_changes", superseded);
    }
  }

  Map<String, Object> record(Map<String, Object> a, String id) {
    synchronized (projects) {
      Map<String, Object> r = projects.get(projects.records("runs", a), id);
      normalizeChanges(r);
      String status = Json.text(r, "status");
      Map<String, Object> checkpoint = r.get("checkpoint") instanceof Map
          ? Json.map(r.get("checkpoint")) : new LinkedHashMap<>();
      boolean stopped = !status.isEmpty() && !status.equals("running")
          && !status.equals("completed") && !status.equals("success");
      boolean valid = !Json.text(checkpoint, "prompt").isEmpty()
          && !Json.text(checkpoint, "path").isEmpty()
          && !Json.text(checkpoint, "provider_id").isEmpty();
      boolean current = false;
      for (Object item : projects.records("sessions", a)) {
        Map<String, Object> session = Json.map(item);
        if (Json.text(session, "id").equals(Json.text(r, "session_id"))
            && Json.text(session, "last_run_id").equals(id)) current = true;
      }
      r.put("recovery_available", stopped && valid && current);
      return r;
    }
  }

  Object start(Map<String, Object> a, Map<String, Object> b, Map<String, Object> policy)
      throws Exception {
    if (Json.flag(b, "resume_from_progress", false)) {
      Map<String, Object> session =
          projects.get(projects.records("sessions", a), Json.text(b, "session_id"));
      Map<String, Object> previous = record(a, Json.text(session, "last_run_id"));
      if (!previous.containsKey("checkpoint"))
        throw new App.Error(400, "restart checkpoint unavailable");
      Map<String, Object> request = new LinkedHashMap<>(Json.map(previous.get("checkpoint")));
      request.put("session_id", session.get("id"));
      request.put("remember_session", true);
      b = request;
    }
    if (Json.text(b, "prompt").isEmpty()) throw new App.Error(400, "prompt required");
    String path = Json.text(b, "path");
    Path projectRoot = projects.workspace(a).resolve(path, false);
    if (!Files.isDirectory(projectRoot)) throw new App.Error(400, "project missing");
    Map<String, Object> selected = null;
    for (Object v :
        Json.list(Json.map(Json.parse(providers.call("providers.list", "{}"))).get("providers")))
      if (Json.text(Json.map(v), "id").equals(Json.text(b, "provider_id"))) selected = Json.map(v);
    if (selected == null || !Json.flag(selected, "enabled", true))
      throw new App.Error(400, "provider missing or disabled");
    long count =
        jobs.values().stream()
            .filter(
                j ->
                    j.actor.get("id").equals(a.get("id"))
                        && Json.text(j.record, "status").equals("running"))
            .count();
    if (count >= Json.number(policy, "max_active_runs_per_user", 3))
      throw new App.Error(429, "run limit");
    String draft = Json.text(b, "attachment_draft");
    List<Object> attachments =
        b.containsKey("attachments") ? Json.list(b.get("attachments")) : new ArrayList<>();
    List<Object> images = new ArrayList<>();
    StringBuilder prompt = new StringBuilder(Json.text(b, "prompt"));
    if (!draft.isEmpty()) {
      if (!draft.matches("\\.webcool_agent/attachments/[a-f0-9]{64}"))
        throw new App.Error(400, "invalid draft");
      for (Object v : attachments) {
        String rel = (String) v;
        if (!rel.startsWith(draft + "/") || rel.substring(draft.length() + 1).contains("/"))
          throw new App.Error(400, "invalid attachment");
        Path file = projects.auth.root(a).resolve(rel);
        if (!Files.isRegularFile(file) || Files.isSymbolicLink(file))
          throw new App.Error(400, "attachment missing");
        byte[] bytes = Files.readAllBytes(file);
        String ext = rel.toLowerCase(Locale.ROOT);
        if (ext.matches(".*\\.(png|jpg|jpeg|gif|webp)"))
          images.add(
              Json.obj(
                  "name",
                  file.getFileName().toString(),
                  "mime_type",
                  "image/"
                      + (ext.endsWith(".jpg") || ext.endsWith(".jpeg")
                          ? "jpeg"
                          : ext.substring(ext.lastIndexOf('.') + 1)),
                  "base64",
                  Base64.getEncoder().encodeToString(bytes)));
        prompt
            .append("\nAttachment ")
            .append(rel)
            .append(":\n")
            .append(
                ext.matches(".*\\.(png|jpg|jpeg|gif|webp)")
                    ? "[image attachment]"
                    : new String(bytes, StandardCharsets.UTF_8));
      }
    }
    Map<String, Object> args = new LinkedHashMap<>(b);
    args.put("_images", images);
    args.put("_model", Json.text(selected, "model"));
    args.put("_policy", new LinkedHashMap<>(policy));
    List<Object> history = new ArrayList<>();
    if (!Json.text(b, "session_id").isEmpty()) {
      Map<String, Object> session =
          projects.get(projects.records("sessions", a), Json.text(b, "session_id"));
      if (session.containsKey("transcript")) history = Json.list(session.get("transcript"));
    }
    args.put("_history", history);
    if (Json.flag(b, "remember_session", false) && Json.text(args, "session_id").isEmpty()) {
      String sid = Store.id();
      Map<String, Object> session =
          Json.obj(
              "id",
              sid,
              "session_id",
              sid,
              "title",
              Json.text(b, "prompt"),
              "project_id",
              Json.text(b, "project_id"),
              "project_path",
              path,
              "messages",
              new ArrayList<>(),
              "transcript",
              new ArrayList<>());
      projects.records("sessions", a).add(session);
      args.put("session_id", sid);
    }
    String id = Store.id();
    Map<String, Object> r =
        Json.obj(
            "ok",
            true,
            "id",
            id,
            "run_id",
            id,
            "status",
            "running",
            "paused",
            false,
            "path",
            path,
            "project_id",
            Json.text(b, "project_id"),
            "prompt",
            Json.text(b, "prompt"),
            "provider_id",
            Json.text(b, "provider_id"),
            "agent_id",
            Json.text(b, "agent_id"),
            "text",
            "",
            "error",
            "",
            "tool_calls",
            new ArrayList<>(),
            "changes",
            new ArrayList<>(),
            "input_tokens",
            0L,
            "output_tokens",
            0L,
            "cached_input_tokens",
            0L,
            "cache_usage_available",
            false,
            "latency_ms",
            0L,
            "model_interactions_json",
            "[]",
            "started_at",
            System.currentTimeMillis() / 1000L);
    if (Json.flag(b, "resume_after_restart", false)) {
      Map<String, Object> checkpoint = new LinkedHashMap<>(b);
      checkpoint.remove("attachments");
      checkpoint.remove("attachment_draft");
      checkpoint.remove("resume_from_progress");
      r.put("checkpoint", checkpoint);
    }
    if (!Json.text(args, "session_id").isEmpty()) {
      Map<String, Object> session =
          projects.get(projects.records("sessions", a), Json.text(args, "session_id"));
      session.put("last_run_id", id);
      r.put("session_id", session.get("id"));
    }
    Job j = new Job(r, new LinkedHashMap<>(a));
    jobs.put(id, j);
    projects.records("runs", a).add(r);
    projects.save();
    String input = prompt.toString();
    executor.submit(() -> execute(j, projectRoot, input, args));
    return Json.obj("ok", true, "run_id", id, "status", "running");
  }

  void execute(Job job, Path root, String input, Map<String, Object> args) {
    Map<String, Object> r = job.record;
    List<Object> events = new ArrayList<>();
    long began = System.currentTimeMillis();
    try (Agent agent =
        new Agent(
            (request, invocation) -> {
              while (job.paused && !job.cancellation.cancelled())
                try {
                  Thread.sleep(20);
                } catch (InterruptedException e) {
                  Thread.currentThread().interrupt();
                  break;
                }
              Map<String, Object> q = Json.map(Json.parse(request));
              q.put("id", r.get("provider_id"));
              q.put("timeout_ms", Json.number(Json.map(args.get("_policy")), "provider_request_timeout_seconds", 900) * 1000);
              q.put("images", args.get("_images"));
              String model = Json.text(args, "_model").toLowerCase(Locale.ROOT);
              if (model.startsWith("glm-5.3")) q.put("reasoning_effort", "low");
              List<Object> transcript = new ArrayList<>(Json.list(args.get("_history")));
              transcript.addAll(Json.list(q.get("messages")));
              q.put("messages", transcript);
              if (!q.containsKey("messages")) q.put("messages", new ArrayList<>());
              Map<String, Object> sent =
                  Json.obj(
                      "event",
                      "model_request_started",
                      "task_overview",
                      r.get("prompt"),
                      "context_overview",
                      root.toString(),
                      "max_output_tokens",
                      q.get("max_output_tokens"),
                      "request_timeout_ms",
                      q.get("timeout_ms"));
              synchronized (projects) {
                events.add(sent);
                r.put("model_interactions_json", Json.encode(events));
                saveProgress();
              }
              Map<String, Object> response;
              try {
                response = Json.map(Json.parse(providers.call("providers.complete", Json.encode(q))));
              } catch (Exception e) {
                synchronized (projects) {
                  events.add(Json.obj("event", "model_request_failed", "error", e.toString()));
                  r.put("model_interactions_json", Json.encode(events));
                }
                throw e;
              }
              if (!Json.flag(response, "ok", false)) {
                synchronized (projects) {
                  events.add(Json.obj("event", "model_request_failed", "error", Json.text(response, "error")));
                  r.put("model_interactions_json", Json.encode(events));
                }
              }
              Agent.Response out = new Agent.Response(Json.text(response, "text"));
              out.error = Json.flag(response, "ok", false) ? "" : Json.text(response, "error");
              out.reasoning = Json.text(response, "reasoning");
              out.inputTokens = Json.number(response, "input_tokens", 0);
              out.outputTokens = Json.number(response, "output_tokens", 0);
              for (Object v :
                  response.containsKey("calls")
                      ? Json.list(response.get("calls"))
                      : new ArrayList<>()) {
                Map<String, Object> call = Json.map(v);
                out.calls.add(
                    new Agent.ToolCall(
                        Json.text(call, "id"),
                        Json.text(call, "name"),
                        Json.text(call, "arguments")));
              }
              Map<String, Object> reply =
                  Json.obj(
                      "event",
                      "model_response_completed",
                      "requested_tools",
                      response.get("calls"),
                      "input_tokens",
                      out.inputTokens,
                      "output_tokens",
                      out.outputTokens,
                      "latency_ms",
                      Json.number(response, "latency_ms", 0),
                      "cached_input_tokens",
                      Json.number(response, "cached_input_tokens", 0),
                      "cache_usage_available",
                      Json.flag(response, "cache_usage_available", false));
              try {
                reply.put(
                    "completion_summary",
                    Json.text(Json.map(Json.parse(out.text)), "completion_summary"));
              } catch (Exception ignored) {
              }
              events.add(reply);
              synchronized (projects) {
                for (String k :
                    new String[] {
                      "input_tokens", "output_tokens", "latency_ms", "cached_input_tokens"
                    }) r.put(k, Json.number(r, k, 0) + Json.number(reply, k, 0));
                if (Json.flag(reply, "cache_usage_available", false))
                  r.put("cache_usage_available", true);
                r.put("model_interactions_json", Json.encode(events));
              }
              return out;
            },
            "You are a coding agent implementing the requested project in the current project root. "
                + "Work incrementally: inspect existing files, then propose complete files using workspace_propose. "
                + "Paths are relative to this project; do not add another project directory. "
                + "Use actual function calls, never print tool calls or all source code as chat text. "
                + "Proposals require human review before formal writes. workspace_read can read pending drafts. "
                + "Keep reasoning concise and implement a small working slice before adding more features. "
                + "Finish only after proposing the requested implementation; honestly report any remaining work.")) {
      for (String name : TOOLS)
        agent.addTool(
            name,
            toolDescription(name),
            toolSchema(name),
            false,
            (call, invocation) -> {
              try {
                Map<String, Object> b = Json.map(Json.parse(call.arguments));
                Object result = tool(job, root, call.name, b);
                synchronized (projects) {
                  Json.list(r.get("tool_calls"))
                      .add(
                          Json.obj(
                              "name",
                              call.name,
                              "arguments",
                              call.arguments,
                              "ok",
                              true,
                              "result",
                              result));
                }
                return new Agent.ToolResult(Json.encode(result));
              } catch (Exception e) {
                synchronized (projects) {
                  Json.list(r.get("tool_calls")).add(Json.obj("name", call.name,
                      "arguments", call.arguments, "ok", false, "error", e.toString()));
                  saveProgress();
                }
                return new Agent.ToolResult(
                    e.getMessage() == null ? "tool failed" : e.getMessage(), true);
              }
            });
      Agent.Hooks hooks = new Agent.Hooks();
      hooks.onText =
          t -> {
            synchronized (projects) {
              r.put("text", Json.text(r, "text") + t);
            }
            return !job.cancellation.cancelled();
          };
      hooks.authorize = (n, a, required) -> !required;
      Agent.Options limits = new Agent.Options();
      Map<String, Object> policy = Json.map(args.get("_policy"));
      limits.maxOutputTokens = Json.number(policy, "max_output_tokens", 32768);
      limits.maxTokensPerCall =
          Json.text(args, "execution_mode").equals("quick")
              ? Json.number(policy, "quick_mode_output_tokens", limits.maxOutputTokens)
              : Math.min(limits.maxOutputTokens,
                  Json.text(args, "execution_mode").equals("large") ? 32768 : 16384);
      limits.maxToolCalls =
          Json.number(
              policy,
              Json.text(args, "execution_mode").equals("large")
                  ? "large_mode_tool_calls"
                  : Json.text(args, "execution_mode").equals("quick")
                      ? "quick_mode_tool_calls"
                      : "standard_mode_tool_calls",
              64);
      limits.maxModelCalls = limits.maxToolCalls + 1;
      limits.timeoutMs = Json.number(policy, "provider_request_timeout_seconds", 900)
          * 1000 * limits.maxModelCalls;
      String result = agent.run(input, limits, hooks, job.cancellation);
      synchronized (projects) {
        r.put("transcript", Json.parse(agent.messages()));
      }
      Map<String, Object> out = Json.map(Json.parse(result));
      synchronized (projects) {
        job.terminalStatus = job.cancellation.cancelled() ? "cancelled" : Json.text(out, "status");
        if (job.terminalStatus.equals("success")) job.terminalStatus = "completed";
        r.put("text", out.get("text"));
        r.put("error", job.terminalStatus.equals("deadline_exceeded")
            ? "任务执行达到总时限，请查看交互记录并继续任务。" : out.get("error"));
      }
    } catch (Exception e) {
      synchronized (projects) {
        job.terminalStatus = job.cancellation.cancelled() ? "cancelled" : "failed";
        r.put("error", e.toString());
      }
    } finally {
      synchronized (projects) {
        try {
          r.put("status", job.terminalStatus);
          r.put("finished_at", System.currentTimeMillis() / 1000);
          r.put("elapsed_ms", System.currentTimeMillis() - began);
          if (Json.flag(args, "remember_session", false)) {
            String sid = Json.text(args, "session_id");
            Map<String, Object> s = null;
            if (!sid.isEmpty())
              try {
                s = projects.get(projects.records("sessions", job.actor), sid);
              } catch (App.Error ignored) {
              }
            if (s == null) {
              sid = Store.id();
              s =
                  Json.obj(
                      "id",
                      sid,
                      "session_id",
                      sid,
                      "title",
                      r.get("prompt"),
                      "project_id",
                      r.get("project_id"),
                      "project_path",
                      r.get("path"),
                      "messages",
                      new ArrayList<>());
              projects.records("sessions", job.actor).add(s);
            }
            s.put("last_run_id", r.get("run_id"));
            s.put("transcript", r.getOrDefault("transcript", new ArrayList<>()));
            List<Object> messages = Json.list(s.get("messages"));
            messages.add(
                Json.obj(
                    "role",
                    "user",
                    "text",
                    r.get("prompt"),
                    "sent_at",
                    began / 1000L,
                    "run_id",
                    r.get("run_id")));
            Map<String, Object> reply =
                Json.obj(
                    "role",
                    "assistant",
                    "text",
                    r.get("text"),
                    "run_id",
                    r.get("run_id"),
                    "duration_ms",
                    r.get("latency_ms"),
                    "elapsed_ms",
                    System.currentTimeMillis() - began);
            for (String k :
                new String[] {
                  "input_tokens", "output_tokens", "cached_input_tokens", "cache_usage_available"
                }) reply.put(k, r.get(k));
            messages.add(reply);
            r.put("session_id", sid);
          }
          if (!Json.text(args, "attachment_draft").isEmpty())
            projects.attachments(job.actor, args, true);
          projects.save();
        } catch (Exception e) {
          System.err.println("persist run: " + e);
        }
      }
    }
  }

  void saveProgress() {
    try {
      projects.save();
    } catch (Exception e) {
      throw new IllegalStateException("cannot save run progress", e);
    }
  }

  static String toolDescription(String name) {
    switch (name) {
      case "workspace_propose": return "Create or replace a file draft: path is relative to project root, content is the complete file text. The draft can be read immediately but requires human acceptance.";
      case "workspace_read": return "Read file contents, preferring this run's latest pending draft over the formal file.";
      case "workspace_list": return "List existing files in a relative directory; empty path lists project root.";
      case "workspace_search": return "Search file contents under path for query.";
      case "workspace_propose_mkdir": return "Propose creating a directory at path.";
      case "workspace_propose_delete": return "Propose deleting the file at path.";
      case "workspace_propose_move": return "Propose moving path to target_path, both relative to project root.";
      default: return "Inspect project information using " + name + ".";
    }
  }

  static String toolSchema(String name) {
    Map<String,Object> properties = Json.obj("path", Json.obj("type","string"));
    List<Object> required = new ArrayList<>();
    if (name.startsWith("workspace_") && !name.equals("workspace_list")) required.add("path");
    if (name.equals("workspace_propose")) {
      properties.put("content", Json.obj("type","string"));
      required.add("content");
    }
    if (name.equals("workspace_search")) {
      properties.put("query", Json.obj("type","string"));
      required.add("query");
    }
    if (name.equals("workspace_propose_move")) {
      properties.put("target_path", Json.obj("type","string"));
      required.add("target_path");
    }
    return Json.encode(Json.obj("type","object","properties",properties,
        "required",required,"additionalProperties",true));
  }

  Object tool(Job job, Path root, String name, Map<String, Object> b) throws Exception {
    Workspace w = new Workspace(root, new ArrayList<>());
    String path = Json.text(b, "path");
    switch (name) {
      case "workspace_list":
        return w.list(path);
      case "workspace_read":
        w.resolve(path, false);
        synchronized (projects) {
          for (Object item : Json.list(job.record.get("changes"))) {
            Map<String,Object> change = Json.map(item);
            if (Json.text(change, "relative_path").equals(path)
                && Json.text(change, "review_status").equals("pending")) {
              String operation = Json.text(change, "operation");
              if (operation.equals("delete") || operation.equals("mkdir"))
                throw new App.Error(400, "draft is deleted or is a directory");
              if (!operation.equals("write")) break;
              return Json.obj("ok", true, "path", path, "content", change.get("content"),
                  "sha256", change.get("draft_hash"), "draft", true, "truncated", false);
            }
          }
        }
        return w.read(path);
      case "workspace_search":
        return w.search(path, Json.text(b, "query"));
      case "workspace_outline":
        return new CodingService(root.toString()).call("workspace.outline", Json.encode(b));
    }
    if (name.startsWith("workspace_propose")) {
      w.resolve(path, false);
      String operation =
          name.equals("workspace_propose_mkdir")
              ? "mkdir"
              : name.equals("workspace_propose_delete")
                  ? "delete"
                  : name.equals("workspace_propose_move") ? "move" : "write";
      String content = Json.text(b, "content");
      Map<String, Object> c =
          Json.obj(
              "path",
              Json.text(job.record, "path") + "/" + path,
              "relative_path",
              path,
              "operation",
              operation,
              "target_path",
              Json.text(b, "target_path"),
              "content",
              content,
              "generation",
              1,
              "draft_hash",
              Store.hash(content.getBytes(StandardCharsets.UTF_8)),
              "review_status",
              "pending",
              "original_hash",
              Files.isRegularFile(w.resolve(path, false))
                  ? Store.hash(Files.readAllBytes(w.resolve(path, false)))
                  : "");
      synchronized (projects) {
        normalizeChanges(job.record);
        List<Object> changes = Json.list(job.record.get("changes"));
        Map<String, Object> previous = null;
        for (Object item : changes)
          if (Json.text(Json.map(item), "path").equals(Json.text(c, "path"))) previous = Json.map(item);
        if (previous != null) {
          c.put("generation", Json.number(previous, "generation", 1) + 1);
          if (Json.text(previous, "review_status").equals("pending"))
            c.put("original_hash", previous.get("original_hash"));
          if (!job.record.containsKey("superseded_changes"))
            job.record.put("superseded_changes", new ArrayList<>());
          Json.list(job.record.get("superseded_changes")).add(new LinkedHashMap<>(previous));
          changes.set(changes.indexOf(previous), c);
        } else changes.add(c);
      }
      return Json.obj("ok", true, "proposal", c);
    }
    return Json.parse(
        new CodingService(root.toString())
            .call(
                name.equals("toolchain_commands")
                    ? "projects.toolchain"
                    : name.equals("project_diagnostics") ? "projects.diagnostics" : "capabilities",
                Json.encode(b)));
  }

  Object control(Map<String, Object> a, String op, Map<String, Object> b) throws Exception {
    String id = Json.text(b, "run_id");
    Map<String, Object> r = record(a, id);
    Job j = jobs.get(id);
    if (op.equals("cancel")) {
      if (j != null) j.cancellation.cancel();
      r.put("status", "cancelled");
    } else if (op.equals("pause")) {
      if (j != null) j.paused = Json.flag(b, "paused", true);
      r.put("paused", Json.flag(b, "paused", true));
    } else if (op.startsWith("result/")) {
      List<Object> cs = Json.list(r.get("changes"));
      List<Object> reviews =
          b.containsKey("reviews") ? Json.list(b.get("reviews")) : new ArrayList<>();
      for (Object v : reviews) {
        Map<String, Object> review = Json.map(v);
        boolean found = false;
        for (Object c : cs) {
          Map<String, Object> change = Json.map(c);
          if (Json.text(change, "path").equals(Json.text(review, "path"))) {
            found = true;
            if (Json.number(change, "generation", 0) != Json.number(review, "generation", 0)
                || !Json.text(change, "draft_hash").equals(Json.text(review, "draft_hash")))
              throw new App.Error(409, "stale draft");
          }
        }
        if (!found) throw new App.Error(409, "draft missing");
      }
      List<Map<String, Object>> accept = new ArrayList<>();
      List<Map<String, Object>> reject = new ArrayList<>();
      for (Object v : cs) {
        Map<String, Object> c = Json.map(v);
        String decision = Json.text(b, "decision");
        if (op.equals("result/apply")) decision = "accepted";
        for (Object rv : reviews)
          if (Json.text(Json.map(rv), "path").equals(Json.text(c, "path")))
            decision = Json.text(Json.map(rv), "decision");
        if (decision.equals("accepted")) accept.add(c);
        else if (decision.equals("rejected")) reject.add(c);
      }
      Workspace w = projects.workspace(a);
      for (Map<String, Object> c : accept) {
        Path f = w.resolve(Json.text(c, "path"), false);
        if (!Json.text(c, "operation").equals("mkdir")
            && !Json.text(c, "original_hash")
                .equals(Files.isRegularFile(f) ? Store.hash(Files.readAllBytes(f)) : ""))
          throw new App.Error(409, "formal file changed");
      }
      for (Map<String, Object> c : reject) c.put("review_status", "rejected");
      for (Map<String, Object> c : accept) {
        Path f = w.resolve(Json.text(c, "path"), false);
        String operation = Json.text(c, "operation");
        if (operation.equals("mkdir")) Files.createDirectories(f);
        else if (operation.equals("delete")) Files.deleteIfExists(f);
        else if (operation.equals("move"))
          Files.move(f, w.resolve(Json.text(r, "path") + "/" + Json.text(c, "target_path"), false));
        else {
          Files.createDirectories(f.getParent());
          Files.write(f, Json.text(c, "content").getBytes(StandardCharsets.UTF_8));
        }
        c.put("review_status", "accepted");
      }
    } else if (op.equals("reasoning/save")) r.put("reasoning", Json.text(b, "reasoning"));
    else throw new App.Error(404, "unknown run operation");
    projects.save();
    return Json.obj("ok", true, "run_id", id, "status", r.get("status"));
  }

  void stop() {
    for (Job j : jobs.values()) j.cancellation.cancel();
    executor.shutdownNow();
  }
}
