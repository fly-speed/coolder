//! Rust async sandbox supervision; libai validates fixed command plans and isolation.
use crate::{
    app::{App, Error, Result},
    projects,
    store::*,
};
use serde_json::{json, Value};
use std::path::PathBuf;
pub fn route(app: &App, s: &mut State, a: &Value, op: &str, b: &Value) -> Result<Value> {
    for key in ["sandbox_runs", "sandbox_plans"] {
        if s.docs.get(key).is_none() {
            s.docs[key] = json!({});
        }
    }
    match op {
        "capabilities" => {
            let root = projects::workspace(s, a)?.resolve(text(b, "path"), true)?;
            let mut caps = projects::coding(&root, "sandbox.capabilities", &json!({}))?;
            let toolchain =
                projects::coding(&root, "projects.toolchain", &json!({"project_path":""}))?;
            for (k, v) in toolchain.as_object().unwrap() {
                caps[k] = v.clone();
            }
            caps["path"] = json!(text(b, "path"));
            caps["backend_available"] = caps["available"].clone();
            caps["execution_enabled"] = caps["available"].clone();
            caps["requires_confirmation"] = json!(true);
            caps["backend_reason"] = caps["reason"].clone();
            caps["browser_acceptance_enabled"] = json!(false);
            Ok(caps)
        }
        "plan" => {
            let root = projects::workspace(s, a)?.resolve(text(b, "path"), true)?;
            let mut p = projects::coding(
                &root,
                "sandbox.preview",
                &json!({"project_path":"","command_id":text(b,"command_id")}),
            )?;
            let id = text(&p, "id").to_string();
            p["root"] = json!(root.to_string_lossy());
            p["owner"] = a["id"].clone();
            p["path"] = json!(text(b, "path"));
            p["plan_id"] = json!(id);
            p["requires_confirmation"] = json!(true);
            p["expires_in_seconds"] = json!(300);
            s.docs["sandbox_plans"][&id] = p.clone();
            s.save()?;
            p.as_object_mut().unwrap().remove("root");
            p.as_object_mut().unwrap().remove("owner");
            Ok(p)
        }
        "runs" => {
            let rows = s
                .rows("sandbox_runs", a)
                .iter()
                .map(|r| {
                    let mut r = r.clone();
                    for k in ["stdout", "stderr", "root"] {
                        r.as_object_mut().unwrap().remove(k);
                    }
                    r
                })
                .collect::<Vec<_>>();
            Ok(json!({"ok":true,"metadata_only":true,"runs":rows}))
        }
        "runs/status" => {
            let mut r = s.get("sandbox_runs", a, text(b, "id"))?;
            r.as_object_mut().unwrap().remove("root");
            Ok(r)
        }
        "runs/cancel" => {
            let mut r = s.get("sandbox_runs", a, text(b, "run_id"))?;
            if !flag(&r, "running", false) {
                return Err(Error::new(409, "sandbox already finished"));
            }
            r["cancel_requested"] = json!(true);
            let _ = projects::coding(
                &PathBuf::from(text(&r, "root")),
                "sandbox.cancel",
                &json!({"id":text(&r,"id")}),
            );
            projects::replace(s, "sandbox_runs", a, &r)?;
            Ok(json!({"ok":true,"cancel_requested":true,"run_id":r["id"]}))
        }
        "execute" => {
            if !flag(b, "confirm", false) {
                return Err(Error::bad("explicit confirmation required"));
            }
            let id = text(b, "plan_id").to_string();
            let p = s.docs["sandbox_plans"]
                .get(&id)
                .cloned()
                .ok_or_else(|| Error::new(404, "sandbox plan missing"))?;
            if p["owner"] != a["id"] {
                return Err(Error::new(404, "sandbox plan missing"));
            }
            if s.rows("sandbox_runs", a)
                .iter()
                .filter(|r| flag(r, "running", false))
                .count()
                >= 4
            {
                return Err(Error::new(429, "sandbox worker limit"));
            }
            s.docs["sandbox_plans"].as_object_mut().unwrap().remove(&id);
            let r = json!({"ok":true,"id":id,"run_id":id,"path":p["path"],"command_id":p["command_id"],"root":p["root"],"status":"queued","running":true,"started_at":now(),"stdout":"","stderr":""});
            s.rows("sandbox_runs", a).push(r);
            s.save()?;
            let app = app.clone();
            let actor = a.clone();
            let run = id.clone();
            std::thread::spawn(move || {
                let result = libai::CodingService::new(text(&p, "root")).and_then(|service| {
                    service.call(
                        "sandbox.execute",
                        &json!({"id":run,"confirm":true,"helper":app.0.helper}).to_string(),
                    )
                });
                let result: Value = match result {
                    Ok(raw) => serde_json::from_str(&raw)
                        .unwrap_or_else(|_| json!({"ok":false,"error":"invalid sandbox result"})),
                    Err(e) => json!({"ok":false,"error":e.to_string()}),
                };
                let mut s = app.0.state.lock().unwrap();
                if let Ok(mut r) = s.get("sandbox_runs", &actor, &run) {
                    for (k, v) in result.as_object().unwrap() {
                        r[k] = v.clone();
                    }
                    r["running"] = json!(false);
                    r["status"] = json!(if flag(&r, "cancelled", false) {
                        "cancelled"
                    } else if flag(&r, "ok", false) && number(&r, "exit_code", -1) == 0 {
                        "completed"
                    } else {
                        "failed"
                    });
                    r["finished_at"] = json!(now());
                    r.as_object_mut().unwrap().remove("root");
                    if let Err(e) = projects::replace(&mut s, "sandbox_runs", &actor, &r) {
                        eprintln!("persist sandbox: {e}");
                    }
                }
            });
            Ok(
                json!({"ok":true,"run_id":id,"plan_id":id,"running":true,"started":true,"status":"queued"}),
            )
        }
        _ => Err(Error::new(404, "sandbox route missing")),
    }
}
pub fn stop(s: &mut State) {
    if let Some(users) = s.docs["sandbox_runs"].as_object() {
        for rows in users.values() {
            if let Some(rows) = rows.as_array() {
                for r in rows {
                    if flag(r, "running", false) {
                        let _ = projects::coding(
                            &PathBuf::from(text(r, "root")),
                            "sandbox.cancel",
                            &json!({"id":text(r,"id")}),
                        );
                    }
                }
            }
        }
    }
}
