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
        let _ = fs::remove_dir_all(&self.0);
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
            ".webcool_agent",
            ".webcool-build",
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
) -> Result<Value> {
    let (tx, rx) = mpsc::channel();
    let task_root = root.to_path_buf();
    let id = text(plan, "id").to_string();
    let args = json!({"id":id,"confirm":true,"helper":app.0.helper});
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
    if changes.is_empty() {
        return Ok(
            json!({"status":"not_required","reason":"本次会话没有待验证的文件修改","commands":[]}),
        );
    }
    let documentation_only = changes.iter().all(|c| {
        text(c, "operation") == "write"
            && Path::new(text(c, "relative_path"))
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| ["md", "txt", "rst", "adoc"].contains(&e))
                .unwrap_or(false)
    });
    if documentation_only {
        return Ok(
            json!({"status":"not_required","reason":"本次只修改文档或文本文件，无需编译","commands":[]}),
        );
    }
    if invocation.stopped() {
        return Err(Error::bad("validation cancelled"));
    }
    let base = std::env::temp_dir().join(format!("coolder-validation-{}", id()));
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
    for command in commands {
        if invocation.stopped() {
            return Err(Error::bad("validation cancelled"));
        }
        let plan = projects::coding(
            &root,
            "sandbox.preview",
            &json!({"project_path":"","command_id":command["id"]}),
        )?;
        let mut result = execute(app, j, &root, &plan, invocation)?;
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
                json!({"status":if started {"failed"} else {"unverified"},"reason":"编译或语法检查未通过","commands":results}),
            );
        }
    }
    Ok(
        json!({"status":"passed","reason":"临时验证目录中的待确认修改已通过编译或语法检查","commands":results}),
    )
}
pub fn validate(app: &App, j: &Job, invocation: &libai::Invocation<'_>) -> Value {
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
    let changes: Vec<Value> = latest
        .into_iter()
        .filter(|c| text(c, "review_status") == "pending")
        .collect();
    let began = std::time::Instant::now();
    let mut result = check(app, j, &changes, invocation)
        .unwrap_or_else(|e| json!({"status":"unverified","reason":e.to_string(),"commands":[]}));
    result["checked_at"] = json!(now());
    result["elapsed_ms"] = json!(began.elapsed().as_millis() as u64);
    result["proposal_count"] = json!(changes.len());
    result["formal_files_changed"] = json!(false);
    result
}
