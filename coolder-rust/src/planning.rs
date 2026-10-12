//! Rust project indexes and durable validation checkpoints.
use crate::{
    app::{Error, Result},
    projects,
    store::*,
    workspace::Workspace,
};
use serde_json::{json, Value};
use std::fs;
pub fn workflow(s: &mut State, a: &Value, b: &Value, method: &str) -> Result<Value> {
    let id = text(b, "project_id");
    let session = text(b, "session_id");
    let task = text(b, "task_id");
    let mut p = s.get("projects", a, id)?;
    if !session.is_empty() {
        s.get("sessions", a, session)?;
    }
    if !task.is_empty()
        && !p["tasks"]
            .as_array()
            .unwrap()
            .iter()
            .any(|t| text(t, "id") == task)
    {
        return Err(Error::new(404, "task missing"));
    }
    if p.get("workflows").is_none() {
        p["workflows"] = json!({});
    }
    let key = format!("{session}:{task}");
    if method == "POST" {
        if flag(b, "remove", false) {
            p["workflows"].as_object_mut().unwrap().remove(&key);
        } else {
            let queue = b["queue"].as_array().cloned().unwrap_or_default();
            let diagnostics = b["diagnostics"].as_array().cloned().unwrap_or_default();
            if queue.len() > 32 || queue.iter().any(|v| !v.is_string()) || diagnostics.len() > 100 {
                return Err(Error::bad("invalid checkpoint"));
            }
            let mut cp = b.clone();
            cp["updated_at"] = json!(now());
            p["workflows"][&key] = cp;
        }
        projects::replace(s, "projects", a, &p)?;
    }
    let mut out = p["workflows"].get(&key).cloned().unwrap_or(json!({}));
    out["ok"] = json!(true);
    out["found"] = json!(p["workflows"].get(&key).is_some());
    out["removed"] = json!(flag(b, "remove", false));
    Ok(out)
}
pub fn index(s: &mut State, a: &Value, b: &Value, refresh: bool) -> Result<Value> {
    let id = if text(b, "project_id").is_empty() {
        text(b, "id")
    } else {
        text(b, "project_id")
    };
    let mut p = s.get("projects", a, id)?;
    if !refresh {
        if let Some(index) = p.get("index") {
            return Ok(index.clone());
        }
    }
    let w = Workspace {
        root: projects::workspace(s, a)?.resolve(text(&p, "path"), false)?,
        projects: vec![],
    };
    let mut files = Vec::new();
    let mut directories = 0;
    scan(
        &w,
        "",
        text(&p, "path"),
        text(&p, "language"),
        &mut files,
        &mut directories,
        0,
    )?;
    let index = json!({"ok":true,"project_id":id,"path":p["path"],"revision":number(&p["index"],"revision",0)+1,"indexed_at":now(),"file_count":files.len(),"directory_count":directories,"skipped_directory_count":0,"changed_file_count":files.len(),"truncated":files.len()>=2000,"files":files,"entries_truncated":false});
    p["index"] = index.clone();
    projects::replace(s, "projects", a, &p)?;
    Ok(index)
}
fn scan(
    w: &Workspace,
    path: &str,
    prefix: &str,
    language: &str,
    files: &mut Vec<Value>,
    dirs: &mut usize,
    depth: usize,
) -> Result<()> {
    if depth > 12 || files.len() >= 2000 {
        return Ok(());
    }
    *dirs += 1;
    let list = w.list(path)?;
    for e in list["entries"].as_array().unwrap() {
        let rel = text(e, "path");
        if flag(e, "directory", false) {
            if !["node_modules", "build", "target", "var"].contains(&text(e, "name")) {
                scan(w, rel, prefix, language, files, dirs, depth + 1)?;
            }
            continue;
        }
        let outline = projects::coding(&w.root, "workspace.outline", &json!({"path":rel}))
            .unwrap_or(json!({"items":[]}));
        let symbols = outline["items"].as_array().cloned().unwrap_or_default();
        let modified = fs::metadata(w.resolve(rel, false)?)?
            .modified()?
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        files.push(json!({"path":format!("{prefix}/{rel}"),"module_id":"main","kind":"source","language":language,"size":e["size"],"modified_at":modified,"symbol_count":symbols.len(),"symbols":symbols}));
        if files.len() >= 2000 {
            break;
        }
    }
    Ok(())
}

pub fn proposal(project: &Value, goal: &str, scale: &str) -> Value {
    let steps: &[&str] = match scale {
        "quick" => &["Implement the requested feature", "Validate and review"],
        "large" => &[
            "Define requirements",
            "Design interfaces",
            "Implement core behavior",
            "Integrate components",
            "Add error handling",
            "Validate and review",
        ],
        _ => &[
            "Define requirements",
            "Implement core behavior",
            "Integrate components",
            "Validate and review",
        ],
    };
    let tasks=steps.iter().enumerate().map(|(i,step)|json!({"id":format!("task-{}",i+1),"title":format!("{step}: {goal}"),"module_id":"main","status":"pending","ready":i==0,"dependencies":if i==0{vec![]}else{vec![format!("task-{i}")]},"acceptance_criteria":["Meet the project goal without unrelated file changes"],"test_plan":["Run the project's available validation commands"]})).collect::<Vec<_>>();
    json!({"ok":true,"proposal":true,"goal":goal,"scale":scale,"modules":[{"id":"main","name":"Application","layer":"application","path":project["path"],"dependencies":[]}],"tasks":tasks})
}
