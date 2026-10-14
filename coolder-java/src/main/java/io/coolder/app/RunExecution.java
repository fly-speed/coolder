package io.coolder.app;

import io.coolder.libai.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.*;

/** Private SDK storage and trusted receipt reconciliation; not a model capability. */
final class RunExecution {
  static FileExecutionStore store(Projects projects, Runs.Job job) throws Exception {
    if (!Json.flag(job.record,"durable_execution",false)) return null;
    Path parent = projects.auth.data.resolve("auth/executions");
    Path root = parent.resolve(Json.text(job.actor,"id"));
    Store.privateDir(parent);
    Store.privateDir(root);
    return FileExecutionStore.plaintext(root.toRealPath().toString());
  }

  static void reconcile(Projects projects, Runs.Job job, FileExecutionStore store,
      String input, String instructions) {
    String scope = Json.text(job.record,"execution_scope");
    Execution.Snapshot snapshot = store.open(scope);
    store.close(scope);
    if (snapshot.stateJson.isEmpty()) return;
    Map<String,Object> journal = Json.map(Json.parse(snapshot.stateJson));
    if (!Json.text(journal,"phase").equals("tools")) return;
    long cursor = Json.number(journal,"cursor",-1);
    List<Object> effects = Json.list(journal.get("effects"));
    if (cursor<0 || cursor>=effects.size()) throw new IllegalStateException("invalid journal cursor");
    Map<String,Object> effect = Json.map(effects.get((int)cursor));
    if (!Json.text(effect,"state").equals("running") || Json.flag(effect,"side_effect_free",false)) return;
    List<Map<String,Object>> matches = new ArrayList<>();
    synchronized (projects) {
      for (Object value : projects.records("runs",job.actor)) {
        Map<String,Object> run = Json.map(value);
        if (!Json.text(run,"execution_scope").equals(scope) || !run.containsKey("tool_effects")) continue;
        for (Object item : Json.list(run.get("tool_effects"))) {
          Map<String,Object> receipt = Json.map(item);
          if (Objects.equals(receipt.get("key"),effect.get("key"))
              && Objects.equals(receipt.get("run_id"),run.get("id"))
              && Objects.equals(receipt.get("scope"),run.get("execution_scope")))
            matches.add(ExecutionContext.copy(receipt));
        }
      }
    }
    if (matches.isEmpty()) return; // SDK leaves the running intent recovery_required.
    if (matches.size()!=1) throw new IllegalStateException("ambiguous host receipts");
    Map<String,Object> receipt = matches.get(0);
    boolean valid = Json.number(receipt,"version",0)==1
        && Json.text(receipt,"state").equals("done")
        && Objects.equals(receipt.get("name"),effect.get("name"))
        && Objects.equals(receipt.get("call_id"),effect.get("id"))
        && Objects.equals(receipt.get("arguments"),effect.get("arguments"))
        && Objects.equals(receipt.get("execution_revision"),effect.get("revision"))
        && Boolean.FALSE.equals(receipt.get("output_error"))
        && receipt.get("output") instanceof String && !Json.text(receipt,"reference").isEmpty()
        && Json.text(receipt,"output").getBytes(StandardCharsets.UTF_8).length<=64*1024;
    if (!valid) throw new IllegalStateException("host receipt does not confirm pending tool");
    Json.parse(Json.text(receipt,"output"));
    Execution.confirm(store,scope,input,instructions,snapshot.revision,Json.text(effect,"key"),
        new Agent.ToolResult(Json.text(receipt,"output")),"applied",Json.text(receipt,"reference"));
  }
  static long exact(Map<String,Object> value,String key) {
    Object number=value.get(key);
    if (!(number instanceof Long || number instanceof Integer))
      throw new IllegalStateException("旧任务时间/策略字段缺失或不是整数："+key);
    return ((Number)number).longValue();
  }

  // Return true only when the verified original budget is already exhausted.
  static boolean migrateDeadline(Projects projects,Runs.Job job,FileExecutionStore store,
      String input,String instructions) {
    String scope=Json.text(job.record,"execution_scope");
    Execution.Snapshot snapshot=store.open(scope);store.close(scope);
    if(snapshot.stateJson.isEmpty())return false;
    Map<String,Object> journal=Json.map(Json.parse(snapshot.stateJson));
    if(journal.containsKey("time_budget") || Json.text(journal,"phase").equals("finished")
        || !Json.text(journal,"output_failure").isEmpty())return false;
    if(Json.text(journal,"phase").equals("tools")) {
      long cursor=exact(journal,"cursor");List<Object> effects=Json.list(journal.get("effects"));
      if(cursor<0 || cursor>=effects.size())throw new IllegalStateException("invalid journal cursor");
      Map<String,Object> effect=Json.map(effects.get((int)cursor));
      if(Json.text(effect,"state").equals("running")&&!Boolean.TRUE.equals(effect.get("side_effect_free")))return false;
    }
    Map<String,Object> original=null;
    synchronized(projects) {
      for(Object value:projects.records("runs",job.actor)) {
        Map<String,Object> run=Json.map(value);
        if(scope.equals(Json.text(run,"id"))) {
          if(original!=null)throw new IllegalStateException("旧任务原始运行记录不唯一");
          original=ExecutionContext.copy(run);
        }
      }
    }
    if(original==null || !Boolean.TRUE.equals(original.get("durable_execution"))
        || !scope.equals(Json.text(original,"execution_scope")))
      throw new IllegalStateException("无法核实旧任务原始时限；保留检查点等待宿主核对");
    Map<String,Object> context=Json.map(original.get("execution_context"));
    if(exact(context,"version")!=1 || !scope.equals(Json.text(context,"scope"))
        || !input.equals(Json.text(context,"input")) || !instructions.equals(Json.text(context,"instructions"))
        || !Objects.equals(original.get("path"),job.record.get("path")))
      throw new IllegalStateException("旧任务冻结身份无法核实");
    Map<String,Object> args=Json.map(context.get("args")),policy=Json.map(args.get("_policy"));
    String mode=Json.text(args,"execution_mode");String key=mode.equals("large")?"large_mode_tool_calls":mode.equals("quick")?"quick_mode_tool_calls":"standard_mode_tool_calls";
    long started=exact(original,"started_at"),seconds=exact(policy,"provider_request_timeout_seconds"),tools=exact(policy,key);
    if(started<=0 || seconds<=0 || tools<0)throw new IllegalStateException("旧任务原始时限字段非法");
    long began=Math.multiplyExact(started,1000),duration=Math.multiplyExact(Math.multiplyExact(seconds,1000),Math.addExact(tools,1));
    long now=System.currentTimeMillis();if(now<began)throw new IllegalStateException("旧任务开始时间在未来，不能迁移");
    long end=Math.addExact(began,duration);if(now>=end)return true;
    String reference="java-app:v1:"+scope+":started_at:"+started+":timeout_ms:"+duration;
    Execution.adoptDeadline(store,scope,input,instructions,snapshot.revision,end-now,reference);
    return false;
  }

}
