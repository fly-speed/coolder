//! Host-owned completion gate: compile the review overlay in libai's sandbox.
use crate::{
    app::{App, Error, Result},
    projects,
    runs::Job,
    store::*,
    workspace::Workspace,
};
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
    sync::mpsc,
    time::Duration,
};

struct Snapshot(PathBuf);
impl Drop for Snapshot {
    fn drop(&mut self) {
        // Keep only private receipts; source and compiler outputs are disposable.
        let project = self.0.join("project");
        if let Ok(entries) = fs::read_dir(&project) {
            for entry in entries.flatten() {
                if entry.file_name() == ".libai_agent" { continue; }
                let path = entry.path();
                let result = if path.is_dir() && !path.is_symlink() { fs::remove_dir_all(&path) } else { fs::remove_file(&path) };
                if let Err(error) = result { eprintln!("validation cleanup: {error}"); }
            }
        }
    }
}
fn raw(root: &Path, op: &str, args: &Value) -> Result<Value> {
    let svc = libai::CodingService::new(&root.to_string_lossy())
        .map_err(|e| Error::bad(&e.to_string()))?;
    let response = svc
        .call(op, &args.to_string())
        .map_err(|e| Error::bad(&e.to_string()))?;
    serde_json::from_str(&response).map_err(|_| Error::bad("invalid sandbox validation response"))
}
fn copy_tree(
    src: &Path,
    dst: &Path,
    depth: usize,
    count: &mut usize,
    bytes: &mut u64,
) -> Result<()> {
    if fs::symlink_metadata(src)?.file_type().is_symlink() {
        return Err(Error::bad(
            "validation snapshot refuses symlink directories",
        ));
    }
    if depth > 24 {
        return Err(Error::bad("validation snapshot exceeds directory limit"));
    }
    private_dir(dst)?;
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let name = entry
            .file_name()
            .into_string()
            .map_err(|_| Error::bad("invalid snapshot filename"))?;
        if [
            ".git",
            ".libai_agent",
            ".libai-build",
            "build",
            "target",
            "dist",
            "node_modules",
            "var",
            ".env",
            "secrets",
            "private",
        ]
        .contains(&name.as_str())
        {
            continue;
        }
        *count += 1;
        if *count > 10000 {
            return Err(Error::bad("validation snapshot exceeds entry limit"));
        }
        let meta = fs::symlink_metadata(entry.path())?;
        if meta.file_type().is_symlink() {
            return Err(Error::bad("validation snapshot refuses symlinks"));
        }
        if meta.is_dir() {
            copy_tree(&entry.path(), &dst.join(name), depth + 1, count, bytes)?;
        } else if meta.is_file() {
            *bytes += meta.len();
            if *count > 10000 || *bytes > 128 * 1024 * 1024 {
                return Err(Error::bad("validation snapshot exceeds copy limit"));
            }
            fs::copy(entry.path(), dst.join(name))?;
        } else {
            return Err(Error::bad("unsupported snapshot file"));
        }
    }
    Ok(())
}
fn overlay(w: &Workspace, change: &Value) -> Result<()> {
    let path = w.resolve(text(change, "relative_path"), false)?;
    match text(change, "operation") {
        "write" => {
            private_dir(path.parent().unwrap())?;
            fs::write(path, text(change, "content"))?;
        }
        "mkdir" => private_dir(&path)?,
        "delete" => {
            if path.is_file() {
                fs::remove_file(path)?;
            } else if path.is_dir() {
                fs::remove_dir_all(path)?;
            }
        }
        "move" => {
            let target = w.resolve(text(change, "target_path"), false)?;
            private_dir(target.parent().unwrap())?;
            fs::rename(path, target)?;
        }
        _ => return Err(Error::bad("unknown validation proposal operation")),
    }
    Ok(())
}
fn build_command(id: &str) -> bool {
    [
        "go.build",
        "rust.build",
        "java.compile-main",
        "kotlin.compile-main",
        "swift.build",
        "csharp.build",
        "d.build",
        "objective-c.build",
        "javascript.syntax-check",
        "python.syntax-check",
        "php.syntax-check",
        "c.make",
        "cpp.make",
    ]
    .contains(&id)
        || id.ends_with(".cmake.configure")
        || id.ends_with(".cmake.build")
}
fn bounded_output(value: &mut Value) {
    for key in ["stdout", "stderr"] {
        if let Some(s) = value[key].as_str() {
            let truncated = s.chars().count() > 8192;
            value[key] = json!(s.chars().take(8192).collect::<String>());
            if truncated {
                value["output_truncated"] = json!(true);
            }
        }
    }
}
fn execute(
    app: &App,
    j: &Job,
    root: &Path,
    plan: &Value,
    invocation: &libai::Invocation<'_>,
    execution_key: &str,
) -> Result<Value> {
    let (tx, rx) = mpsc::channel();
    let task_root = root.to_path_buf();
    let id = text(plan, "id").to_string();
    let args = json!({"id":id,"confirm":true,"helper":app.0.helper,"idempotency_key":execution_key,"parent_idempotency_key":invocation.idempotency_key()});
    std::thread::spawn(move || {
        let _ = tx.send(raw(&task_root, "sandbox.execute", &args));
    });
    loop {
        match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(result) => return result,
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(Error::bad("validation worker stopped"))
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if invocation.stopped() || j.cancellation.cancelled() {
                    // Retry cancellation until the sandbox worker has registered its active plan.
                    let _ = raw(root, "sandbox.cancel", &json!({"id":id}));
                }
            }
        }
    }
}
fn check(
    app: &App,
    j: &Job,
    changes: &[Value],
    invocation: &libai::Invocation<'_>,
) -> Result<Value> {
    if let Some(reason) = no_compile_reason(changes) {
        return Ok(json!({"status":"not_required","reason":reason,"commands":[]}));
    }
    if invocation.stopped() {
        return Err(Error::bad("validation cancelled"));
    }
    let parent = invocation.idempotency_key();
    if parent.is_empty() { return Err(Error::bad("validation invocation has no execution key")); }
    let directory = j.root.join(".libai_agent/validation_runs");
    private_dir(&directory)?;
    let base = directory.join(hash(parent.as_bytes()));
    fs::create_dir(&base)?;
    let snapshot = Snapshot(base);
    private_dir(&snapshot.0)?;
    let root = snapshot.0.join("project");
    copy_tree(&j.root, &root, 0, &mut 0, &mut 0)?;
    let w = Workspace {
        root: root.clone(),
        projects: vec![],
    };
    for change in changes {
        overlay(&w, change)?;
    }
    let artifact = projects::coding(&root, "tasks.revision", &json!({}))?;
    let artifact_revision = text(&artifact, "artifact_revision");
    let toolchain = projects::coding(&root, "projects.toolchain", &json!({"project_path":""}))?;
    let mut commands = toolchain["commands"]
        .as_array()
        .cloned()
        .unwrap_or_default();
    let cmake = commands
        .iter()
        .any(|c| text(c, "id").ends_with(".cmake.configure"));
    commands.retain(|c| {
        build_command(text(c, "id")) && !(cmake && ["c.make", "cpp.make"].contains(&text(c, "id")))
    });
    commands.sort_by_key(|c| {
        if text(c, "id").ends_with(".cmake.configure") {
            0
        } else {
            1
        }
    });
    if commands.is_empty() {
        return Ok(
            json!({"status":"unverified","reason":"没有可用的受控编译或语法检查命令；请检查项目结构和管理员工具链设置","unavailable_tools":toolchain["unavailable_tools"],"commands":[]}),
        );
    }
    if commands.len() > 16 {
        return Err(Error::bad("too many validation commands"));
    }
    let mut results = Vec::new();
    let parent_key = invocation.idempotency_key();
    if parent_key.is_empty() {
        return Err(Error::bad("validation invocation has no execution key"));
    }
    for command in commands {
        if invocation.stopped() {
            return Err(Error::bad("validation cancelled"));
        }
        let plan = projects::coding(
            &root,
            "sandbox.preview",
            &json!({"project_path":"","command_id":command["id"]}),
        )?;
        let execution_key = format!("validation.{}", hash(format!("{}\n{}\n{}",
            parent_key, artifact_revision, command).as_bytes()));
        let mut result = execute(app, j, &root, &plan, invocation, &execution_key)?;
        if text(&result, "idempotency_key") != execution_key {
            return Err(Error::bad("validation command execution key mismatch"));
        }
        bounded_output(&mut result);
        result["command_id"] = command["id"].clone();
        result["executable"] = plan["executable"].clone();
        result["fixed_arguments"] = plan["fixed_arguments"].clone();
        let started = flag(&result, "started", false);
        let passed = started
            && flag(&result, "ok", false)
            && number(&result, "exit_code", -1) == 0
            && !flag(&result, "cancelled", false)
            && !flag(&result, "timed_out", false);
        results.push(result);
        if !passed {
            return Ok(
                json!({"status":if started {"failed"} else {"unverified"},"reason":"编译或语法检查未通过","commands":results,"artifact_revision":artifact_revision}),
            );
        }
    }
    Ok(
        json!({"status":"passed","reason":"临时验证目录中的待确认修改已通过编译或语法检查","commands":results,"artifact_revision":artifact_revision}),
    )
}
fn pending_changes(app: &App, j: &Job) -> Vec<Value> {
    let changes = {
        let mut s = app.0.state.lock().unwrap();
        let sid = text(&j.args, "session_id");
        s.rows("runs", &j.actor)
            .iter()
            .filter(|r| {
                text(r, "path") == text(&j.args, "path")
                    && (text(r, "id") == j.id || (!sid.is_empty() && text(r, "session_id") == sid))
            })
            .flat_map(|r| r["changes"].as_array().cloned().unwrap_or_default())
            .collect::<Vec<_>>()
    };
    // A newer accepted/rejected draft supersedes an older pending generation too.
    let mut latest: Vec<Value> = Vec::new();
    for change in changes {
        latest.retain(|old| text(old, "relative_path") != text(&change, "relative_path"));
        latest.push(change);
    }
    latest
        .into_iter()
        .filter(|c| text(c, "review_status") == "pending")
        .collect()
}

fn no_compile_reason(changes: &[Value]) -> Option<&'static str> {
    if changes.is_empty() {
        return Some("本次会话没有待验证的文件修改");
    }
    if changes.iter().all(|c| {
        text(c, "operation") == "write"
            && Path::new(text(c, "relative_path"))
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| ["md", "txt", "rst", "adoc"].contains(&e))
                .unwrap_or(false)
    }) {
        return Some("本次只修改文档或文本文件，无需编译");
    }
    None
}
pub enum CompletionGate {
    Satisfied(Value),
    Verify,
    Failed(String),
}
// Pure inspection: no compiler/process launch occurs inside the model callback.
fn verify_receipts(project: &Path, report: &Value) -> Result<()> {
    let parent = text(report, "parent_idempotency_key");
    if parent.is_empty() { return Err(Error::bad("编译收据缺少父执行键")); }
    let expected = project.join(".libai_agent/validation_runs").join(hash(parent.as_bytes())).join("project");
    if expected.canonicalize()? != Path::new(text(&projects::coding(project,"storage.canonical_path",&json!({"path":text(report,"receipt_root")}))?,"path")).canonicalize()? {
        return Err(Error::bad("编译收据目录不匹配"));
    }
    let ownership = projects::coding(&expected,"sandbox.parent.inspect",&json!({"idempotency_key":parent,"project_path":""}))?;
    if !flag(&ownership,"found",false) { return Err(Error::bad("编译收据父关联缺失")); }
    let commands = report["commands"].as_array().ok_or_else(|| Error::bad("编译收据命令无效"))?;
    if commands.is_empty() || commands.len()>16 { return Err(Error::bad("编译收据命令数量无效")); }
    let mut keys = std::collections::HashSet::new();
    for command in commands {
        let key = text(command,"idempotency_key");
        if key.is_empty() || !keys.insert(key.to_string()) { return Err(Error::bad("编译收据键缺失或重复")); }
        let inspected = projects::coding(&expected,"sandbox.inspect",&json!({"idempotency_key":key}))?;
        if !flag(&inspected,"found",false) { return Err(Error::bad("编译收据缺失")); }
        let receipt: Value = serde_json::from_str(text(&inspected,"receipt_json")).map_err(|_| Error::bad("编译收据损坏"))?;
        if !["finished","confirmed_applied"].contains(&text(&receipt,"state")) || !text(&receipt,"project_path").is_empty()
            || text(&receipt,"key")!=key || text(&receipt,"command_id")!=text(command,"command_id") {
            return Err(Error::bad("编译收据状态或身份不匹配"));
        }
        let result=&receipt["result"];
        if !flag(result,"broker_ok",false)||!flag(result,"started",false)||number(result,"exit_code",-1)!=0
            ||flag(result,"cancelled",true)||flag(result,"timed_out",true)||flag(result,"error_present",true) {
            return Err(Error::bad("编译收据没有成功执行证据"));
        }
        for field in ["started","exit_code","cancelled","timed_out","elapsed_ms"] {
            if result[field]!=command[field] { return Err(Error::bad("编译收据与报告不一致")); }
        }
    }
    let children=ownership["children"].as_array().ok_or_else(|| Error::bad("编译收据父关联无效"))?;
    let child_keys: std::collections::HashSet<_> = children.iter().filter_map(|v|v.as_str().map(String::from)).collect();
    if child_keys.len()!=children.len() || keys!=child_keys { return Err(Error::bad("编译收据父关联不完整")); }
    Ok(())
}
pub fn completion_gate(app: &App, j: &Job) -> CompletionGate {
    let changes = pending_changes(app, j);
    let revision = match projects::coding(&j.root, "tasks.revision", &json!({"changes":changes})) {
        Ok(value) => text(&value, "artifact_revision").to_string(),
        Err(error) => return CompletionGate::Failed(format!("成果版本无法验证：{error}")),
    };
    if let Some(reason) = no_compile_reason(&changes) {
        return CompletionGate::Satisfied(
            json!({"status":"not_required","reason":reason,"commands":[],
            "artifact_revision":revision,"checked_at":now(),"proposal_count":changes.len(),"formal_files_changed":false}),
        );
    }
    let mut state = app.0.state.lock().unwrap();
    let run = match state.get("runs", &j.actor, &j.id) {
        Ok(run) => run,
        Err(e) => return CompletionGate::Failed(e.to_string()),
    };
    let report = &run["validation"];
    if text(report, "artifact_revision") == revision && !revision.is_empty() {
        match text(report, "status") {
            "passed" => return match verify_receipts(&j.root,report) {
                Ok(())=>CompletionGate::Satisfied(report.clone()),
                Err(error)=>CompletionGate::Failed(format!("编译收据无法验证：{error}")),
            },
            "not_required" => return CompletionGate::Failed("编译收据不足：当前修改需要编译".into()),
            "unverified" => {
                return CompletionGate::Failed(format!("编译未验证：{}", text(report, "reason")))
            }
            _ => {}
        }
    }
    let failures = run["validation_attempts"]
        .as_array()
        .map(|attempts| {
            attempts
                .iter()
                .filter(|a| text(a, "status") == "failed")
                .count()
        })
        .unwrap_or(0);
    if failures >= 4 {
        return CompletionGate::Failed(
            "编译验证仍未通过，已达到自动修复次数上限；请查看编译输出后继续任务。".into(),
        );
    }
    CompletionGate::Verify
}
pub fn validate(app: &App, j: &Job, invocation: &libai::Invocation<'_>) -> Value {
    let changes = pending_changes(app, j);
    let revision = projects::coding(&j.root, "tasks.revision", &json!({"changes":changes}))
        .map(|v| text(&v, "artifact_revision").to_string());
    let began = std::time::Instant::now();
    let mut result = check(app, j, &changes, invocation)
        .unwrap_or_else(|e| json!({"status":"unverified","reason":e.to_string(),"commands":[]}));
    match revision {
        Ok(revision) => {
            if result.get("artifact_revision").is_none() {
                result["artifact_revision"] = json!(revision);
            }
        }
        Err(e) => {
            result["status"] = json!("unverified");
            result["reason"] = json!(format!("成果版本无法验证：{e}"));
        }
    }
    if !invocation.idempotency_key().is_empty() {
        result["receipt_root"] = json!(j.root.join(".libai_agent/validation_runs").join(hash(invocation.idempotency_key().as_bytes())).join("project").to_string_lossy());
        result["parent_idempotency_key"] = json!(invocation.idempotency_key());
    }
    result["checked_at"] = json!(now());
    result["elapsed_ms"] = json!(began.elapsed().as_millis() as u64);
    result["proposal_count"] = json!(changes.len());
    result["formal_files_changed"] = json!(false);
    result
}
