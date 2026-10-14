//! Rust-owned agent jobs, model audit, proposal generation and explicit reviews.
use crate::{
    app::{App, Error, Result},
    projects,
    store::*,
    workspace::Workspace,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use std::{
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};
pub struct Job {
    pub id: String,
    pub actor: Value,
    pub args: Value,
    pub root: PathBuf,
    pub cancellation: Arc<libai::Cancellation>,
    pub paused: AtomicBool,
}
const INSTRUCTIONS: &str = "You are a coding agent. Inspect files with tools; propose changes for explicit human review. The host compiles pending proposals before accepting a final answer. If project_validation_result reports a compiler failure, repair the proposals using that output and then finish again. Toolchain discovery and an empty diagnostic list do not prove compilation succeeded.";
const TOOLS: &[&str] = &[
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
    "project_diagnostics",
];
// Derive UI recovery capability for old records as well as current jobs.
fn refresh_contract(s: &mut State, actor: &Value, run: &mut Value) {
    let result = (|| -> Result<Value> {
        let w = projects::workspace(s, actor)?;
        let root = w.resolve(text(run, "path"), false)?;
        projects::coding(&root, "tasks.describe_run", run)
    })();
    run["task_contract"] = match result {
        Ok(mut contract) => {
            contract.as_object_mut().unwrap().remove("ok");
            contract
        }
        Err(e) => json!({"version":1,"accepted":false,"error":e.to_string()}),
    };
}
// Authenticated host diagnostic, never registered as a model tool.
pub fn recovery(s: &mut State, actor: &Value, id: &str) -> Result<Value> {
    use libai::ExecutionStore;
    let run=s.get("runs",actor,id)?;
    if text(&run,"id")!=id {return Err(Error::new(404,"record not found"));}
    let scope=text(&run,"execution_scope");
    if !flag(&run,"durable_execution",false) {return Err(Error::new(409,"run has no SDK execution journal"));}
    if text(&run,"status")=="running" || s.jobs.values().any(|job|text(&job.args,"_execution_scope")==scope) {
        return Err(Error::new(409,"execution is active; wait until it stops"));
    }
    let context=&run["execution_context"];
    if context["version"].as_i64()!=Some(1) || text(context,"scope")!=scope
        || !context["input"].is_string() || !context["instructions"].is_string()
        || context["path"]!=run["path"] {
        return Err(Error::new(409,"frozen execution identity cannot be verified"));
    }
    let root=s.data.join("auth/executions").join(text(actor,"id"));
    let mut store=libai::FileExecutionStore::plaintext(&root.to_string_lossy()).map_err(|e|Error::new(409,&e.to_string()))?;
    let snapshot=store.open(scope).map_err(|e|Error::new(409,&e.to_string()))?;
    store.close(scope);
    if snapshot.state_json.is_empty() {return Err(Error::new(409,"execution checkpoint missing"));}
    let args=json!({"scope":scope,"revision":snapshot.revision,"state_json":snapshot.state_json,
        "input":context["input"],"instructions":context["instructions"]});
    let output=libai::governance_call("executions.inspect",&args.to_string()).map_err(|e|Error::new(409,&e.to_string()))?;
    let mut result:Value=serde_json::from_str(&output).map_err(|e|Error::new(409,&e.to_string()))?;
    if !flag(&result,"ok",false) {return Err(Error::new(409,text(&result,"error")));}
    result["run_id"]=json!(id);Ok(result)
}

pub fn status(s: &mut State, actor: &Value, id: &str) -> Result<Value> {
    let mut run = s.get("runs", actor, id)?;
    let stopped = !matches!(
        text(&run, "status"),
        "running" | "completed" | "success" | ""
    );
    let valid_checkpoint = run
        .get("checkpoint")
        .filter(|cp| cp.is_object())
        .map(|cp| {
            !text(cp, "prompt").is_empty()
                && !text(cp, "path").is_empty()
                && !text(cp, "provider_id").is_empty()
        })
        .unwrap_or(false);
    let current_session = !text(&run, "session_id").is_empty()
        && s.get("sessions", actor, text(&run, "session_id"))
            .map(|session| text(&session, "last_run_id") == text(&run, "id"))
            .unwrap_or(false);
    let uncertain = uncertain_effect(&run);
    run["recovery_available"] = json!(stopped && valid_checkpoint && current_session && !uncertain);
    run["recovery_blocked_reason"] = json!(if uncertain {"unconfirmed host tool effect"} else {""});
    let old = run.get("task_contract").cloned();
    refresh_contract(s, actor, &mut run);
    if old.as_ref() != run.get("task_contract") {
        projects::replace(s, "runs", actor, &run)?;
    }
    Ok(run)
}

fn provider_identity(provider: &Value) -> Value {
    let mut identity=json!({});
    for key in ["id","protocol","base_url","model","allow_file_content","model_profile_json",
        "response_state_mode","responses_store","responses_background","responses_compact","responses_strict_tools",
        "responses_min_reasoning_effort","responses_reasoning_summary","responses_text_verbosity","responses_service_tier",
        "responses_cache_ttl","qwen_session_cache","openai_organization","openai_project"] {
        identity[key]=provider.get(key).cloned().unwrap_or(Value::Null);
    }
    identity
}
// Preserve original ceilings while allowing current administrator limits to tighten them.
fn restricted_policy(frozen: &Value, current: &Value) -> Value {
    let mut policy=json!({});
    for (key,default) in [("max_output_tokens",32768),("standard_mode_tool_calls",64),
        ("large_mode_tool_calls",64),("quick_mode_tool_calls",64),("provider_request_timeout_seconds",900)] {
        policy[key]=json!(number(frozen,key,default).min(number(current,key,default)));
    }
    let output=number(&policy,"max_output_tokens",32768);
    policy["quick_mode_output_tokens"]=json!(number(frozen,"quick_mode_output_tokens",output)
        .min(number(current,"quick_mode_output_tokens",output)).min(output));
    policy
}
pub fn start(app: &App, s: &mut State, a: &Value, b: &Value) -> Result<Value> {
    let request;
    let mut frozen = None;
    let mut resume_state = None;
    let mut durable = libai::FileExecutionStore::available();
    let b = if flag(b, "resume_from_progress", false) {
        let session = s.get("sessions", a, text(b, "session_id"))?;
        let previous = s.get("runs", a, text(&session, "last_run_id"))?;
        resume_state=Some(previous.clone());
        durable=flag(&previous,"durable_execution",false);
        if durable && !libai::FileExecutionStore::available() {
            return Err(Error::new(409,"durable execution store unavailable on this platform"));
        }
        if uncertain_effect(&previous) {
            return Err(Error::new(409,"unconfirmed host tool effect requires host reconciliation"));
        }
        if !text(&previous,"storage_error").is_empty() {
            return Err(Error::new(409,"cannot automatically resume an uncertain storage outcome"));
        }
        if let Some(context)=previous.get("execution_context") {
            frozen=Some(context.clone());
        }
        let mut cp = previous
            .get("checkpoint")
            .cloned()
            .ok_or_else(|| Error::bad("restart checkpoint unavailable"))?;
        cp["session_id"] = session["id"].clone();
        cp["remember_session"] = json!(true);
        request = cp;
        &request
    } else {
        b
    };
    if text(b, "prompt").is_empty() {
        return Err(Error::bad("prompt required"));
    }
    let root = projects::workspace(s, a)?.resolve(text(b, "path"), false)?;
    if !root.is_dir() {
        return Err(Error::bad("project missing"));
    }
    let ps = projects::coding(&s.workspace, "providers.list", &json!({}))?;
    let selected = ps["providers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| text(p, "id") == text(b, "provider_id") && flag(p, "enabled", true));
    if selected.is_none() {
        return Err(Error::bad("provider missing or disabled"));
    }
    let count = s
        .rows("runs", a)
        .iter()
        .filter(|r| text(r, "status") == "running")
        .count();
    if count >= number(&s.policy, "max_active_runs_per_user", 3) as usize {
        return Err(Error::new(429, "run limit"));
    }
    let provider_identity=provider_identity(selected.unwrap());
    if let Some(context)=&frozen {
        if number(context,"version",0)!=1 || text(context,"instructions")!=INSTRUCTIONS ||
            context["provider_identity"]!=provider_identity || context["path"]!=b["path"] ||
            text(context,"scope").is_empty() || text(context,"scope").len()>128 ||
            !text(context,"scope").bytes().all(|c|c.is_ascii_alphanumeric() || c==b'_' || c==b'-') ||
            !context["args"].is_object() || !context["input"].is_string() ||
            !context["args"]["_images"].is_array() || !context["args"]["_history"].is_array() ||
            !context["args"]["_policy"].is_object() ||
            ["provider_id","project_id","path","prompt"].iter().any(|key|context["args"][*key]!=b[*key]) {
            return Err(Error::new(409,"frozen execution context changed or invalid"));
        }
    }
    if durable {
        if let Some(context)=&frozen {
            if s.jobs.values().any(|job|text(&job.args,"_execution_scope")==text(context,"scope")) {
                return Err(Error::new(409,"execution scope still active"));
            }
        }
    }
    let mut input = text(b, "prompt").to_string();
    let mut images = Vec::new();
    let draft = text(b, "attachment_draft");
    if frozen.is_none() && !draft.is_empty() {
        if !projects::valid_draft(draft) {
            return Err(Error::bad("invalid draft"));
        }
        let files = b["attachments"]
            .as_array()
            .ok_or_else(|| Error::bad("attachments array required"))?;
        for v in files {
            let rel = v.as_str().ok_or_else(|| Error::bad("invalid attachment"))?;
            if !rel.starts_with(&format!("{draft}/")) || rel[draft.len() + 1..].contains('/') {
                return Err(Error::bad("invalid attachment"));
            }
            let file = s.root(a)?.join(rel);
            if !file.is_file() || fs::symlink_metadata(&file)?.file_type().is_symlink() {
                return Err(Error::bad("attachment missing"));
            }
            let bytes = fs::read(&file)?;
            let content = if let Some(m) =
                crate::workspace::mime(rel).filter(|m| m.starts_with("image/"))
            {
                images.push(json!({"name":file.file_name().unwrap().to_string_lossy(),"mime_type":m,"base64":STANDARD.encode(&bytes)}));
                format!("[image attachment: {m}]")
            } else {
                String::from_utf8(bytes).map_err(|_| Error::bad("invalid text attachment"))?
            };
            input.push_str(&format!("\nAttachment {rel}:\n{content}"));
        }
    }
    let mut args = b.clone();
    args["_images"] = json!(images);
    args["_model_profile"] = json!(text(selected.unwrap(), "model_profile_json"));
    args["_policy"] = restricted_policy(&s.policy,&s.policy);
    let history = if text(b, "session_id").is_empty() {
        json!([])
    } else {
        s.get("sessions", a, text(b, "session_id"))?
            .get("transcript")
            .cloned()
            .unwrap_or(json!([]))
    };
    args["_history"] = history;
    if let Some(context)=&frozen {
        input=text(context,"input").to_string();
        args=context["args"].clone();
        args["session_id"]=b["session_id"].clone();
        args["remember_session"]=json!(true);
        args["_policy"]=restricted_policy(&args["_policy"],&s.policy);
    }
    let previous_docs=s.docs.clone();
    if flag(b, "remember_session", false) && text(&args, "session_id").is_empty() {
        let sid = id();
        s.rows("sessions",a).push(json!({"id":sid,"session_id":sid,"title":text(b,"prompt"),"project_id":text(b,"project_id"),"project_path":text(b,"path"),"messages":[],"transcript":[]}));
        args["session_id"] = json!(sid);
    }
    let id = id();
    let r = json!({"ok":true,"id":id,"run_id":id,"status":"running","paused":false,"path":text(b,"path"),"project_id":text(b,"project_id"),"prompt":text(b,"prompt"),"provider_id":text(b,"provider_id"),"agent_id":text(b,"agent_id"),"text":"","error":"","tool_calls":[],"changes":[],"input_tokens":0,"output_tokens":0,"cached_input_tokens":0,"cache_usage_available":false,"latency_ms":0,"model_interactions_json":"[]","started_at":now()});
    let mut r = r;
    if durable {
        if let Some(previous)=&resume_state {
            for key in ["changes","validation","validation_attempts","tool_calls"] {
                if let Some(value)=previous.get(key){r[key]=value.clone();}
            }
        }
    }
    let scope=frozen.as_ref().map(|context|text(context,"scope").to_string()).unwrap_or_else(||id.clone());
    let mut context_args=args.clone();
    // Copy only runtime inputs. Caller-supplied unknown fields and credentials never enter the context.
    context_args.as_object_mut().unwrap().retain(|key,_|[
        "agent_id","provider_id","project_id","path","prompt","execution_mode","remember_session",
        "resume_after_restart","session_id","_images","_history","_model_profile","_policy"
    ].contains(&key.as_str()));
    r["execution_context"]=frozen.clone().unwrap_or_else(||json!({"version":1,"scope":scope,
        "instructions":INSTRUCTIONS,"path":b["path"],"provider_identity":provider_identity,
        "input":input,"args":context_args}));
    // Carry tightened ceilings forward; a later administrative increase cannot undo them.
    r["execution_context"]["args"]["_policy"]=args["_policy"].clone();
    r["execution_scope"]=json!(scope);
    r["durable_execution"]=json!(durable);
    if flag(b, "resume_after_restart", false) {
        let mut checkpoint = b.clone();
        checkpoint.as_object_mut().unwrap().retain(|key,_|[
            "agent_id","provider_id","project_id","path","prompt","execution_mode",
            "remember_session","resume_after_restart","session_id"
        ].contains(&key.as_str()));
        r["checkpoint"] = checkpoint;
    }
    if !text(&args, "session_id").is_empty() {
        let mut session = s.get("sessions", a, text(&args, "session_id"))?;
        session["last_run_id"] = json!(id);
        r["session_id"] = session["id"].clone();
        let row=s.rows("sessions",a).iter_mut().find(|row|row["id"]==session["id"])
            .ok_or_else(||Error::bad("session missing"))?;
        *row=session;
    }
    args["_execution_scope"]=r["execution_scope"].clone();
    args["_durable_execution"]=json!(durable);
    let token = libai::Cancellation::new().map_err(|e| Error::bad(&e.to_string()))?;
    let job = Arc::new(Job {
        id: id.clone(),
        actor: a.clone(),
        args,
        root,
        cancellation: Arc::new(token),
        paused: AtomicBool::new(false),
    });
    s.jobs.insert(id.clone(), job.clone());
    s.rows("runs", a).push(r);
    if let Err(error)=s.save() {
        s.jobs.remove(&id);
        s.docs=previous_docs;
        return Err(error); // No worker or model has started; disk may contain either complete snapshot.
    }
    let app = app.clone();
    std::thread::spawn(move || execute(&app, job, &input));
    Ok(json!({"ok":true,"run_id":id,"status":"running"}))
}
fn update(app: &App, j: &Job, f: impl FnOnce(&mut Value)) {
    let mut s = app.0.state.lock().unwrap();
    if let Some(r) = s
        .rows("runs", &j.actor)
        .iter_mut()
        .find(|r| text(r, "id") == j.id)
    {
        f(r);
    }
}
// Publish one tool mutation only after durable state save. On ambiguous storage
// failure retain the previous view, flag the run and stop further model/tool work.
fn update_persist(app: &App, j: &Job, change: impl FnOnce(&mut Value)) -> Result<()> {
    let mut state=app.0.state.lock().unwrap();
    let previous=state.get("runs",&j.actor,&j.id)?;
    let row=state.rows("runs",&j.actor).iter_mut().find(|row|text(row,"id")==j.id)
        .ok_or_else(||Error::bad("run missing"))?;
    change(row);
    if let Err(error)=state.save() {
        if let Some(row)=state.rows("runs",&j.actor).iter_mut().find(|row|text(row,"id")==j.id) {
            *row=previous;
            row["storage_error"]=json!(error.to_string());
        }
        j.cancellation.cancel();
        return Err(error);
    }
    Ok(())
}
// Host receipts share the atomic APP-state commit with the actual proposal/report.
// They are evidence for later SDK reconciliation, never a model confirmation tool.
fn uncertain_effect(run: &Value) -> bool {
    if text(run,"status")=="recovery_required" || flag(run,"execution_recovery_required",false){return true;}
    run.get("tool_effects").map(|effects|effects.as_array().map(|rows| {
        let mut keys=std::collections::BTreeSet::new();
        rows.iter().any(|row|text(row,"state")!="done" || number(row,"version",0)!=1 ||
            text(row,"key").is_empty() || !keys.insert(text(row,"key")) ||
            row["scope"]!=run["execution_scope"] || row["run_id"]!=run["id"] ||
            !row["output_error"].is_boolean() ||
            serde_json::from_str::<Value>(text(row,"output")).is_err() || text(row,"reference").is_empty())
    }).unwrap_or(true)).unwrap_or(false)
}
fn host_effect(j: &Job, name: &str, call_id: &str, arguments: &str, key: String) -> Value {
    let key=if key.is_empty(){format!("host-{}",id())}else{key};
    json!({"version":1,"key":key,"scope":text(&j.args,"_execution_scope"),"run_id":j.id,"name":name,"call_id":call_id,
        "arguments":arguments,"execution_revision":"6-durable-validation-receipts","state":"running"})
}
fn save_effect(run: &mut Value, effect: Value) {
    if !run["tool_effects"].is_array(){run["tool_effects"]=json!([]);}
    let rows=run["tool_effects"].as_array_mut().unwrap();
    if let Some(old)=rows.iter_mut().find(|old|old["key"]==effect["key"]){*old=effect;}else{rows.push(effect);}
}
fn completed_effect(mut effect: Value, output: &Value) -> Value {
    effect["state"]=json!("done");
    effect["output"]=json!(output.to_string());
    effect["output_error"]=json!(false);
    effect["reference"]=json!(format!("{}:tool:{}",text(&effect,"run_id"),text(&effect,"key")));
    effect
}
fn execute(app: &App, j: Arc<Job>, input: &str) {
    let began = Instant::now();
    let sent_at = now();
    let app_model = app.clone();
    let jm = j.clone();
    let providers = app.0.state.lock().unwrap().workspace.clone();
    let result = (|| -> std::result::Result<String, libai::Error> {
        let mut agent=libai::Agent::new(move|request,invocation|{
  while jm.paused.load(Ordering::Relaxed)&&!jm.cancellation.cancelled(){std::thread::sleep(Duration::from_millis(20));}
  let mut q:Value=match serde_json::from_str(request){Ok(q)=>q,Err(e)=>{update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_failed","error":e.to_string()}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});return libai::ModelResponse{error:e.to_string(),..Default::default()}}};q["id"]=json!(text(&jm.args,"provider_id"));let remaining=invocation.remaining_ms();if invocation.stopped()||remaining==0{return libai::ModelResponse{error:"任务时限或取消阻止模型请求".into(),..Default::default()};}let request_timeout=number(&jm.args["_policy"],"provider_request_timeout_seconds",900)*1000;q["timeout_ms"]=json!(if remaining>0{request_timeout.min(remaining)}else{request_timeout});q["images"]=jm.args["_images"].clone();let mut history=jm.args["_history"].as_array().cloned().unwrap_or_default();history.extend(q["messages"].as_array().cloned().unwrap_or_default());q["messages"]=json!(history);
  update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_started","task_overview":text(&jm.args,"prompt"),"context_overview":jm.root.to_string_lossy(),"max_output_tokens":q["max_output_tokens"],"request_timeout_ms":q["timeout_ms"]}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});
  let response=match projects::coding(&providers,"providers.complete",&q){Ok(v)=>v,Err(e)=>{update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_failed","error":e.to_string()}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});return libai::ModelResponse{error:e.to_string(),..Default::default()}}};
  let mut calls: Vec<libai::ToolCall>=response["calls"].as_array().map(|a|a.iter().map(|c|libai::ToolCall{id:text(c,"id").into(),name:text(c,"name").into(),arguments:text(c,"arguments").into()}).collect()).unwrap_or_default();
  update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();let mut reply=json!({"event":"model_response_completed","requested_tools":response["calls"]});for k in ["input_tokens","output_tokens","latency_ms","cached_input_tokens"]{reply[k]=json!(number(&response,k,0));r[k]=json!(number(r,k,0)+number(&response,k,0));}reply["cache_usage_available"]=json!(flag(&response,"cache_usage_available",false));r["cache_usage_available"]=json!(flag(r,"cache_usage_available",false)||flag(&response,"cache_usage_available",false));if let Ok(t)=serde_json::from_str::<Value>(text(&response,"text")){reply["completion_summary"]=json!(text(&t,"completion_summary"));}events.push(reply);r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});
  let mut completion_error = String::new();
  if calls.is_empty() {
    match crate::validation::completion_gate(&app_model,&jm) {
      crate::validation::CompletionGate::Satisfied(report)=>update(&app_model,&jm,|r|r["validation"]=report),
      crate::validation::CompletionGate::Verify=>calls.push(libai::ToolCall{id:format!("compiler-feedback-{}",id()),name:"project_validation_result".into(),arguments:"{}".into()}),
      crate::validation::CompletionGate::Failed(error)=>completion_error=error,
    }
  }
  libai::ModelResponse{text:text(&response,"text").into(),reasoning:text(&response,"reasoning").into(),input_tokens:number(&response,"input_tokens",0) as usize,output_tokens:number(&response,"output_tokens",0) as usize,calls,error:completion_error,..Default::default()}
 },INSTRUCTIONS)?;
        if flag(&j.args,"_durable_execution",false) {
            let store=crate::run_execution::store(app,&j)?;
            crate::run_execution::reconcile(app,&j,store.clone(),input,INSTRUCTIONS)?;
            if crate::run_execution::migrate_deadline(app,&j,store.clone(),input,INSTRUCTIONS)? {
                return Ok(json!({"status":"deadline_exceeded","text":"","error":"原任务总时限已耗尽，请新建任务；旧检查点保留。"}).to_string());
            }
            agent.set_execution_store(text(&j.args,"_execution_scope"),Some(store))?;
        }
        let profile=text(&j.args,"_model_profile");
        agent.configure_model(if profile.is_empty(){r#"{"version":1}"#}else{profile})?;
        for name in TOOLS {
            let app = app.clone();
            let jt = j.clone();
            let name = (*name).to_string();
            let toolname = name.clone();
            let side_effect_free=["workspace_list","workspace_read","workspace_search"].contains(&toolname.as_str());
            agent.add_tool_governed(&name,&name,r#"{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"additionalProperties":true}"#,false,"6-durable-validation-receipts",side_effect_free,move|call,invocation|{
                let effect=host_effect(&jt,&toolname,&call.id,&call.arguments,invocation.idempotency_key());
                let result=serde_json::from_str::<Value>(&call.arguments).map_err(|e|Error::bad(&e.to_string()))
                    .and_then(|b|tool(&app,&jt,&toolname,&b,&effect));
                match result {
                    Ok(v)=>{
                        if !toolname.starts_with("workspace_propose") {
                            update(&app,&jt,|r|r["tool_calls"].as_array_mut().unwrap().push(json!({"name":call.name,"arguments":call.arguments,"ok":true,"result":v})));
                        }
                        libai::ToolResult{text:v.to_string(),error:false}
                    },
                    Err(e)=>libai::ToolResult{text:e.to_string(),error:true}
                }
            })?;
        }
        let validation_app = app.clone();
        let validation_job = j.clone();
        agent.add_tool_versioned(
            "project_validation_result",
            "Run host compiler verification on pending proposals; inspect the report and repair failures before finishing.",
            r#"{"type":"object","properties":{},"additionalProperties":false}"#,
            false,
            "6-durable-validation-receipts",
            move |call, invocation| {
                let effect=host_effect(&validation_job,"project_validation_result",&call.id,&call.arguments,invocation.idempotency_key());
                if let Err(error)=update_persist(&validation_app,&validation_job,|run|save_effect(run,effect.clone())) {
                    return libai::ToolResult{text:format!("validation intent storage failed: {error}"),error:true};
                }
                let (report,executed)=match crate::validation::completion_gate(&validation_app,&validation_job) {
                    crate::validation::CompletionGate::Satisfied(report)=>(report,false),
                    crate::validation::CompletionGate::Failed(reason)=>(json!({"status":"unverified","reason":reason,"validation_blocked":true,"commands":[]}),false),
                    crate::validation::CompletionGate::Verify=>{
                        update(&validation_app,&validation_job,|run|run["phase"]=json!("validating"));
                        (crate::validation::validate(&validation_app,&validation_job,invocation),true)
                    },
                };
                let persisted=update_persist(&validation_app,&validation_job,|run|{
                    if executed {
                        run["validation"]=report.clone();
                        if !run["validation_attempts"].is_array(){run["validation_attempts"]=json!([]);}
                        run["validation_attempts"].as_array_mut().unwrap().push(report.clone());
                    }
                    save_effect(run,completed_effect(effect.clone(),&report));
                    run["phase"]=json!("validation_finished");
                    run["tool_calls"].as_array_mut().unwrap().push(json!({"name":"project_validation_result",
                        "ok":true,"result":report,"executed":executed,"idempotency_key":effect["key"]}));
                });
                if let Err(error)=persisted {
                    return libai::ToolResult{text:format!("validation report storage failed: {error}"),error:true};
                }
                // A compiler failure is a successfully collected report, not an
                // uncertain tool exception. The model receives the report to repair.
                libai::ToolResult {text:report.to_string(),error:false}
            },
        )?;
        let mut on_text = |t: &str| {
            update(app, &j, |r| {
                r["text"] = json!(format!("{}{t}", text(r, "text")))
            });
            !j.cancellation.cancelled()
        };
        let mut authorize = |_: &str, _: &str, required: bool| !required;
        let mut limits = libai::Options::default();
        limits.max_output_tokens = number(&j.args["_policy"], "max_output_tokens", 32768) as usize;
        limits.max_tokens_per_call = if text(&j.args, "execution_mode") == "quick" {
            number(
                &j.args["_policy"],
                "quick_mode_output_tokens",
                limits.max_output_tokens as i64,
            ) as usize
        } else {
            limits.max_output_tokens
        };
        limits.max_tool_calls = number(
            &j.args["_policy"],
            match text(&j.args, "execution_mode") {
                "large" => "large_mode_tool_calls",
                "quick" => "quick_mode_tool_calls",
                _ => "standard_mode_tool_calls",
            },
            64,
        ) as usize;
        // Each tool turn may need a separate request, plus a final delivery turn.
        limits.max_model_calls = limits.max_tool_calls + 1;
        limits.timeout_ms = number(&j.args["_policy"], "provider_request_timeout_seconds", 900)
            * 1000
            * limits.max_model_calls as i64;
        let result = agent.run_with_cancellation(
            input,
            &limits,
            libai::Hooks {
                on_text: Some(&mut on_text),
                authorize: Some(&mut authorize),
                on_event: None,
            },
            Some(&j.cancellation),
        );
        if let Ok(messages) = agent.messages() {
            if let Ok(messages) = serde_json::from_str::<Value>(&messages) {
                update(app, &j, |r| r["transcript"] = messages);
            }
        }
        result
    })();
    update(app, &j, |r| match result {
        Ok(ref raw) => {
            let out: Value = serde_json::from_str(raw).unwrap_or_default();
            let status = if j.cancellation.cancelled() {
                "cancelled"
            } else {
                match text(&out, "status") {
                    "success" => "completed",
                    s => s,
                }
            };
            if status=="recovery_required" {r["execution_recovery_required"]=json!(true);}
            r["_terminal_status"] = json!(status);
            r["text"] = out["text"].clone();
            r["error"] = if status == "deadline_exceeded" {
                json!("任务执行达到总时限，请查看交互记录并继续任务。")
            } else {
                out["error"].clone()
            };
        }
        Err(ref e) => {
            if flag(&j.args,"_durable_execution",false){r["execution_recovery_required"]=json!(true);}
            r["_terminal_status"] = json!(if j.cancellation.cancelled() {
                "cancelled"
            } else {
                "failed"
            });
            r["error"] = json!(e.to_string());
        }
    });
    let mut s = app.0.state.lock().unwrap();
    let persisted = (|| -> Result<()> {
        let mut r = s.get("runs", &j.actor, &j.id)?;
        r["status"] = r
            .as_object_mut()
            .unwrap()
            .remove("_terminal_status")
            .unwrap_or(json!("failed"));
        if !text(&r,"storage_error").is_empty() {
            r["status"]=json!("failed");
            r["error"]=json!(format!("任务存储失败，执行已停止，不能确认本次写入：{}",text(&r,"storage_error")));
        }
        r["finished_at"] = json!(now());
        r["elapsed_ms"] = json!(began.elapsed().as_millis() as u64);
        refresh_contract(&mut s, &j.actor, &mut r);
        projects::replace(&mut s, "runs", &j.actor, &r)?;
        if flag(&j.args, "remember_session", false) {
            let existing = text(&j.args, "session_id");
            let mut session=if !existing.is_empty(){s.get("sessions",&j.actor,existing).ok()}else{None}.unwrap_or_else(||{let sid=id();json!({"id":sid,"session_id":sid,"title":text(&j.args,"prompt"),"project_id":text(&j.args,"project_id"),"project_path":text(&j.args,"path"),"messages":[]})});
            let sid = text(&session, "id").to_string();
            let mut reply = json!({"role":"assistant","text":r["text"],"run_id":j.id,"duration_ms":r["latency_ms"],"elapsed_ms":began.elapsed().as_millis() as u64});
            for k in [
                "input_tokens",
                "output_tokens",
                "cached_input_tokens",
                "cache_usage_available",
            ] {
                reply[k] = r[k].clone();
            }
            session["messages"].as_array_mut().unwrap().push(json!({"role":"user","text":text(&j.args,"prompt"),"sent_at":sent_at,"run_id":j.id}));
            session["messages"].as_array_mut().unwrap().push(reply);
            session["last_run_id"] = json!(j.id);
            session["transcript"] = r.get("transcript").cloned().unwrap_or(json!([]));
            let rows = s.rows("sessions", &j.actor);
            if let Some(old) = rows.iter_mut().find(|r| text(r, "id") == sid) {
                *old = session;
            } else {
                rows.push(session);
            }
            r["session_id"] = json!(sid);
            projects::replace(&mut s, "runs", &j.actor, &r)?;
        }
        if !text(&j.args, "attachment_draft").is_empty() {
            projects::attachments(&s, &j.actor, &j.args, true)?;
        }
        s.save()
    })();
    if let Err(e) = persisted {
        eprintln!("persist run: {e}");
    }
    s.jobs.remove(&j.id);
}
fn tool(app: &App, j: &Job, name: &str, b: &Value, effect: &Value) -> Result<Value> {
    let w = Workspace {
        root: j.root.clone(),
        projects: vec![],
    };
    let path = text(b, "path");
    match name {
        "workspace_list" => return w.list(path),
        "workspace_read" => return w.read(path),
        "workspace_search" => return w.search(path, text(b, "query")),
        "workspace_outline" => return projects::coding(&j.root, "workspace.outline", b),
        "project_diagnostics" if text(b,"source").is_empty() => return Err(Error::bad("project_diagnostics only parses supplied compiler output; empty input does not mean compilation passed")),
        _ => {}
    }
    if name.starts_with("workspace_propose") {
        let f = w.resolve(path, false)?;
        let operation = match name {
            "workspace_propose_mkdir" => "mkdir",
            "workspace_propose_delete" => "delete",
            "workspace_propose_move" => "move",
            _ => "write",
        };
        let content = text(b, "content");
        let mut c = json!({"path":format!("{}/{path}",text(&j.args,"path")),"relative_path":path,"operation":operation,"target_path":text(b,"target_path"),"content":content,"generation":1,"draft_hash":hash(content.as_bytes()),"review_status":"pending","original_hash":if f.is_file(){hash(&fs::read(f)?)}else{String::new()}});
        update_persist(app, j, |r| {
            let changes = r["changes"].as_array_mut().unwrap();
            if let Some(previous) = changes
                .iter_mut()
                .find(|previous| text(previous, "path") == text(&c, "path"))
            {
                c["generation"] = json!(number(previous, "generation", 1) + 1);
                *previous = c.clone();
            } else {
                changes.push(c.clone());
            }
            let output=json!({"ok":true,"proposal":c});
            save_effect(r,completed_effect(effect.clone(),&output));
            r["tool_calls"].as_array_mut().unwrap().push(json!({"name":name,"arguments":effect["arguments"],
                "ok":true,"result":output,"idempotency_key":effect["key"]}));
        })?;
        return Ok(json!({"ok":true,"proposal":c}));
    }
    projects::coding(
        &j.root,
        match name {
            "toolchain_commands" => "projects.toolchain",
            "project_diagnostics" => "projects.diagnostics",
            _ => "capabilities",
        },
        b,
    )
}
pub fn control(s: &mut State, a: &Value, op: &str, b: &Value) -> Result<Value> {
    let id = text(b, "run_id");
    let mut r = s.get("runs", a, id)?;
    match op {
        "cancel" => {
            if let Some(j) = s.jobs.get(id) {
                j.cancellation.cancel();
            }
            r["status"] = json!("cancelled");
        }
        "pause" => {
            let paused = flag(b, "paused", true);
            if let Some(j) = s.jobs.get(id) {
                j.paused.store(paused, Ordering::Relaxed);
            }
            r["paused"] = json!(paused);
        }
        "reasoning/save" => r["reasoning"] = json!(text(b, "reasoning")),
        op if op.starts_with("result/") => {
            if uncertain_effect(&r) {
                return Err(Error::new(409,"unconfirmed host tool effect; review disabled"));
            }
            if !text(&r,"storage_error").is_empty() {
                return Err(Error::new(409,"run storage outcome uncertain; review disabled"));
            }
            let reviews = b["reviews"].as_array().cloned().unwrap_or_default();
            let cs = r["changes"].as_array_mut().unwrap();
            for review in &reviews {
                let c = cs
                    .iter()
                    .find(|c| text(c, "path") == text(review, "path"))
                    .ok_or_else(|| Error::new(409, "draft missing"))?;
                if number(c, "generation", 0) != number(review, "generation", 0)
                    || text(c, "draft_hash") != text(review, "draft_hash")
                {
                    return Err(Error::new(409, "stale draft"));
                }
            }
            let w = projects::workspace(s, a)?;
            let mut indices = Vec::new();
            for (i, c) in cs.iter_mut().enumerate() {
                let decision = reviews
                    .iter()
                    .find(|rv| text(rv, "path") == text(c, "path"))
                    .map(|rv| text(rv, "decision"))
                    .unwrap_or(if op == "result/apply" {
                        "accepted"
                    } else {
                        text(b, "decision")
                    });
                if decision == "accepted" {
                    let f = w.resolve(text(c, "path"), false)?;
                    if text(c, "operation") != "mkdir"
                        && text(c, "original_hash")
                            != if f.is_file() {
                                hash(&fs::read(f)?)
                            } else {
                                String::new()
                            }
                    {
                        return Err(Error::new(409, "formal file changed"));
                    }
                    indices.push(i);
                } else if decision == "rejected" {
                    c["review_status"] = json!("rejected");
                }
            }
            for i in indices {
                let c = &mut cs[i];
                let f = w.resolve(text(c, "path"), false)?;
                match text(c, "operation") {
                    "mkdir" => fs::create_dir_all(f)?,
                    "delete" => {
                        if f.exists() {
                            fs::remove_file(f)?;
                        }
                    }
                    "move" => fs::rename(
                        f,
                        w.resolve(
                            &format!("{}/{}", text(b, "path"), text(c, "target_path")),
                            false,
                        )?,
                    )?,
                    _ => {
                        fs::create_dir_all(f.parent().unwrap())?;
                        fs::write(f, text(c, "content"))?;
                    }
                }
                c["review_status"] = json!("accepted");
                c["accepted_by"] = a["id"].clone();
                c["review_reference"] = json!(crate::store::id());
            }
        }
        _ => return Err(Error::new(404, "run operation missing")),
    }
    refresh_contract(s, a, &mut r);
    projects::replace(s, "runs", a, &r)?;
    Ok(json!({"ok":true,"run_id":id,"status":r["status"]}))
}
