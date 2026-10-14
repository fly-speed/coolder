package io.coolder.app;

import io.coolder.libai.Agent;
import java.util.*;
import java.util.concurrent.Callable;

/** Host evidence committed with the proposal; never exposed as a model tool. */
final class ToolReceipts {
  static boolean uncertain(Map<String,Object> run) {
    if (Json.text(run,"status").equals("recovery_required")
        || Json.flag(run,"execution_recovery_required",false)) return true;
    if (!run.containsKey("tool_effects")) return false;
    if (!(run.get("tool_effects") instanceof List)) return true;
    Set<String> keys = new HashSet<>();
    for (Object value : Json.list(run.get("tool_effects"))) {
      if (!(value instanceof Map)) return true;
      Map<String,Object> receipt = Json.map(value);
      if (Json.number(receipt,"version",0)!=1 || !Json.text(receipt,"state").equals("done")
          || Json.text(receipt,"key").isEmpty() || !keys.add(Json.text(receipt,"key"))
          || !Objects.equals(receipt.get("scope"),run.get("execution_scope"))
          || !Objects.equals(receipt.get("run_id"),run.get("id"))
          || !Boolean.FALSE.equals(receipt.get("output_error"))
          || !(receipt.get("output") instanceof String) || Json.text(receipt,"reference").isEmpty())
        return true;
      try { Json.parse(Json.text(receipt,"output")); }
      catch (RuntimeException e) { return true; }
    }
    return false;
  }

  static Object proposal(Projects projects, Runs.Job job, Agent.ToolCall call,
      Agent.Invocation invocation, Callable<Object> operation) throws Exception {
    synchronized (projects) {
      Map<String,Object> run = job.record;
      Map<String,Object> before = ExecutionContext.copy(run);
      Object output;
      try { output = operation.call(); }
      catch (Exception e) { run.clear(); run.putAll(before); throw e; }
      try {
        String key = invocation.idempotencyKey();
        if (key.isEmpty()) key = "host-" + Store.id();
        Map<String,Object> receipt = Json.obj("version",1L,"key",key,"scope",run.get("execution_scope"),
            "run_id",run.get("id"),"name",call.name,"call_id",call.id,"arguments",call.arguments,
            "execution_revision","5-command-receipts","state","done","output",Json.encode(output),
            "output_error",false,"reference",Json.text(run,"id")+":tool:"+key);
        if (!run.containsKey("tool_effects")) run.put("tool_effects",new ArrayList<>());
        Json.list(run.get("tool_effects")).add(receipt);
        Json.list(run.get("tool_calls")).add(Json.obj("name",call.name,"arguments",call.arguments,
            "ok",true,"result",Json.parse(Json.encode(output)),"idempotency_key",key));
        projects.save();
      } catch (Exception e) {
        run.clear(); run.putAll(before);
        run.put("storage_error",e.toString());
        job.cancellation.cancel();
        throw new IllegalStateException("proposal storage outcome uncertain",e);
      }
      return output;
    }
  }
}
