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
    run["recovery_available"] = json!(stopped && valid_checkpoint && current_session);
    Ok(run)
}

pub fn start(app: &App, s: &mut State, a: &Value, b: &Value) -> Result<Value> {
    let request;
    let b = if flag(b, "resume_from_progress", false) {
        let session = s.get("sessions", a, text(b, "session_id"))?;
        let previous = s.get("runs", a, text(&session, "last_run_id"))?;
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
    let mut input = text(b, "prompt").to_string();
    let mut images = Vec::new();
    let draft = text(b, "attachment_draft");
    if !draft.is_empty() {
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
    args["_policy"] = s.policy.clone();
    let history = if text(b, "session_id").is_empty() {
        json!([])
    } else {
        s.get("sessions", a, text(b, "session_id"))?
            .get("transcript")
            .cloned()
            .unwrap_or(json!([]))
    };
    args["_history"] = history;
    if flag(b, "remember_session", false) && text(&args, "session_id").is_empty() {
        let sid = id();
        s.rows("sessions",a).push(json!({"id":sid,"session_id":sid,"title":text(b,"prompt"),"project_id":text(b,"project_id"),"project_path":text(b,"path"),"messages":[],"transcript":[]}));
        args["session_id"] = json!(sid);
    }
    let id = id();
    let r = json!({"ok":true,"id":id,"run_id":id,"status":"running","paused":false,"path":text(b,"path"),"project_id":text(b,"project_id"),"prompt":text(b,"prompt"),"provider_id":text(b,"provider_id"),"agent_id":text(b,"agent_id"),"text":"","error":"","tool_calls":[],"changes":[],"input_tokens":0,"output_tokens":0,"cached_input_tokens":0,"cache_usage_available":false,"latency_ms":0,"model_interactions_json":"[]","started_at":now()});
    let mut r = r;
    if flag(b, "resume_after_restart", false) {
        let mut checkpoint = b.clone();
        for k in ["attachments", "attachment_draft", "resume_from_progress"] {
            checkpoint.as_object_mut().unwrap().remove(k);
        }
        r["checkpoint"] = checkpoint;
    }
    if !text(&args, "session_id").is_empty() {
        let mut session = s.get("sessions", a, text(&args, "session_id"))?;
        session["last_run_id"] = json!(id);
        r["session_id"] = session["id"].clone();
        projects::replace(s, "sessions", a, &session)?;
    }
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
    s.save()?;
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
fn execute(app: &App, j: Arc<Job>, input: &str) {
    let began = Instant::now();
    let sent_at = now();
    let app_model = app.clone();
    let jm = j.clone();
    let providers = app.0.state.lock().unwrap().workspace.clone();
    let result = (|| -> std::result::Result<String, libai::Error> {
        let mut validation_failures = 0;
        let mut agent=libai::Agent::new(move|request,invocation|{
  while jm.paused.load(Ordering::Relaxed)&&!jm.cancellation.cancelled(){std::thread::sleep(Duration::from_millis(20));}
  let mut q:Value=match serde_json::from_str(request){Ok(q)=>q,Err(e)=>{update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_failed","error":e.to_string()}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});return libai::ModelResponse{error:e.to_string(),..Default::default()}}};q["id"]=json!(text(&jm.args,"provider_id"));q["timeout_ms"]=json!(number(&jm.args["_policy"],"provider_request_timeout_seconds",900)*1000);q["images"]=jm.args["_images"].clone();let mut history=jm.args["_history"].as_array().cloned().unwrap_or_default();history.extend(q["messages"].as_array().cloned().unwrap_or_default());q["messages"]=json!(history);
  update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_started","task_overview":text(&jm.args,"prompt"),"context_overview":jm.root.to_string_lossy(),"max_output_tokens":q["max_output_tokens"],"request_timeout_ms":q["timeout_ms"]}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});
  let response=match projects::coding(&providers,"providers.complete",&q){Ok(v)=>v,Err(e)=>{update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();events.push(json!({"event":"model_request_failed","error":e.to_string()}));r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});return libai::ModelResponse{error:e.to_string(),..Default::default()}}};
  let mut calls: Vec<libai::ToolCall>=response["calls"].as_array().map(|a|a.iter().map(|c|libai::ToolCall{id:text(c,"id").into(),name:text(c,"name").into(),arguments:text(c,"arguments").into()}).collect()).unwrap_or_default();
  update(&app_model,&jm,|r|{let mut events:Vec<Value>=serde_json::from_str(text(r,"model_interactions_json")).unwrap_or_default();let mut reply=json!({"event":"model_response_completed","requested_tools":response["calls"]});for k in ["input_tokens","output_tokens","latency_ms","cached_input_tokens"]{reply[k]=json!(number(&response,k,0));r[k]=json!(number(r,k,0)+number(&response,k,0));}reply["cache_usage_available"]=json!(flag(&response,"cache_usage_available",false));r["cache_usage_available"]=json!(flag(r,"cache_usage_available",false)||flag(&response,"cache_usage_available",false));if let Ok(t)=serde_json::from_str::<Value>(text(&response,"text")){reply["completion_summary"]=json!(text(&t,"completion_summary"));}events.push(reply);r["model_interactions_json"]=json!(serde_json::to_string(&events).unwrap());});
  let mut completion_error = String::new();
  if calls.is_empty() {
    update(&app_model,&jm,|r|r["phase"]=json!("validating"));
    let validation=crate::validation::validate(&app_model,&jm,invocation);
    update(&app_model,&jm,|r|{
      r["validation"]=validation.clone();
      if !r["validation_attempts"].is_array(){r["validation_attempts"]=json!([]);}
      r["validation_attempts"].as_array_mut().unwrap().push(validation.clone());
      r["phase"]=json!("validation_finished");
    });
    match text(&validation,"status") {
      "passed" | "not_required" => {},
      "failed" if validation_failures < 3 => {
        validation_failures += 1;
        calls.push(libai::ToolCall{id:format!("compiler-feedback-{}",id()),name:"project_validation_result".into(),arguments:"{}".into()});
      },
      "failed" => completion_error="编译验证仍未通过，已达到自动修复次数上限；请查看编译输出后继续任务。".into(),
      _ => completion_error=format!("编译未验证：{}",text(&validation,"reason")),
    }
  }
  libai::ModelResponse{text:text(&response,"text").into(),reasoning:text(&response,"reasoning").into(),input_tokens:number(&response,"input_tokens",0) as usize,output_tokens:number(&response,"output_tokens",0) as usize,calls,error:completion_error,..Default::default()}
 },"You are a coding agent. Inspect files with tools; propose changes for explicit human review. The host compiles pending proposals before accepting a final answer. If project_validation_result reports a compiler failure, repair the proposals using that output and then finish again. Toolchain discovery and an empty diagnostic list do not prove compilation succeeded.")?;
        for name in TOOLS {
            let app = app.clone();
            let jt = j.clone();
            let name = (*name).to_string();
            let toolname = name.clone();
            agent.add_tool(&name,&name,r#"{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"additionalProperties":true}"#,false,move|call,_|{
  let result=serde_json::from_str::<Value>(&call.arguments).map_err(|e|Error::bad(&e.to_string())).and_then(|b|tool(&app,&jt,&toolname,&b));match result{Ok(v)=>{update(&app,&jt,|r|r["tool_calls"].as_array_mut().unwrap().push(json!({"name":call.name,"arguments":call.arguments,"ok":true,"result":v})));libai::ToolResult{text:v.to_string(),error:false}},Err(e)=>libai::ToolResult{text:e.to_string(),error:true}}
 })?;
        }
        let validation_app = app.clone();
        let validation_job = j.clone();
        agent.add_tool(
            "project_validation_result",
            "Read host compiler verification output; fix failures before finishing.",
            r#"{"type":"object","properties":{},"additionalProperties":false}"#,
            false,
            move |_, _| {
                let mut s = validation_app.0.state.lock().unwrap();
                let report = s
                    .get("runs", &validation_job.actor, &validation_job.id)
                    .ok()
                    .map(|r| r["validation"].clone())
                    .unwrap_or(json!({"status":"unverified"}));
                libai::ToolResult {
                    text: report.to_string(),
                    error: text(&report, "status") != "passed",
                }
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
            r["_terminal_status"] = json!(status);
            r["text"] = out["text"].clone();
            r["error"] = if status == "deadline_exceeded" {
                json!("任务执行达到总时限，请查看交互记录并继续任务。")
            } else {
                out["error"].clone()
            };
        }
        Err(ref e) => {
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
        r["finished_at"] = json!(now());
        r["elapsed_ms"] = json!(began.elapsed().as_millis() as u64);
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
}
fn tool(app: &App, j: &Job, name: &str, b: &Value) -> Result<Value> {
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
        update(app, j, |r| {
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
        });
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
            }
        }
        _ => return Err(Error::new(404, "run operation missing")),
    }
    projects::replace(s, "runs", a, &r)?;
    Ok(json!({"ok":true,"run_id":id,"status":r["status"]}))
}
