//! Project plans, external registrations, collaboration and drafts belong to this Rust APP.
use crate::{
    app::{Error, Result},
    store::*,
    workspace::Workspace,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
};
pub fn workspace(s: &mut State, a: &Value) -> Result<Workspace> {
    Ok(Workspace {
        root: s.root(a)?,
        projects: s.rows("projects", a).clone(),
    })
}
pub fn coding(root: &Path, op: &str, b: &Value) -> Result<Value> {
    let service = libai::CodingService::new(&root.to_string_lossy())
        .map_err(|e| Error::bad(&e.to_string()))?;
    let r = service
        .call(op, &b.to_string())
        .map_err(|e| Error::bad(&e.to_string()))?;
    let v: Value = serde_json::from_str(&r).map_err(|_| Error::bad("invalid binding response"))?;
    if !flag(&v, "ok", false) {
        return Err(Error::bad(text(&v, "error")));
    }
    Ok(v)
}
pub fn create(s: &mut State, a: &Value, b: &Value, initialize: bool) -> Result<Value> {
    let requested = text(b, "path");
    let language = if text(b, "language").is_empty() {
        "python"
    } else {
        text(b, "language")
    };
    if text(b, "storage_scope") == "shared" {
        return Err(Error::new(403, "shared storage disabled"));
    }
    if initialize && !flag(b, "confirm", false) {
        return Err(Error::bad("confirmation required"));
    }
    let external = Path::new(requested).is_absolute();
    if external && !flag(a, "admin", false) && !flag(&s.policy, "allow_users_local_projects", false)
    {
        return Err(Error::new(403, "local project access denied"));
    }
    if let Some(p) = s
        .rows("projects", a)
        .iter()
        .find(|p| text(p, "path") == requested || text(p, "external") == requested)
        .cloned()
    {
        if initialize {
            return Err(Error::new(409, "project exists"));
        }
        return Ok(p);
    }
    let physical = if external {
        let p = PathBuf::from(requested);
        for ancestor in p.ancestors() {
            if fs::symlink_metadata(ancestor)
                .map(|m| m.file_type().is_symlink())
                .unwrap_or(false)
                && ancestor != Path::new("/var")
                && ancestor != Path::new("/tmp")
            {
                return Err(Error::bad("symlink path"));
            }
        }
        if p.starts_with(&s.data) && !p.starts_with(&s.workspace) {
            return Err(Error::bad("private storage"));
        }
        p
    } else {
        workspace(s, a)?.resolve(requested, false)?
    };
    if initialize {
        if physical.exists() {
            return Err(Error::new(409, "project exists"));
        }
        fs::create_dir_all(
            physical
                .parent()
                .ok_or_else(|| Error::bad("invalid project"))?,
        )?;
        coding(
            physical.parent().unwrap(),
            "projects.scaffold",
            &json!({"project_path":physical.file_name().unwrap().to_string_lossy(),"language":language,"platform":if text(b,"platform").is_empty(){"cross-platform"}else{text(b,"platform")}}),
        )?;
    } else if !physical.is_dir() {
        return Err(Error::bad("project directory missing"));
    }
    let id = id();
    let path = if external {
        format!("external/{id}")
    } else {
        requested.into()
    };
    let p = json!({"ok":true,"id":id,"project_id":id,"title":physical.file_name().unwrap().to_string_lossy(),"path":path,"project_path":path,"language":language,"platform":text(b,"platform"),"external":if external{physical.to_string_lossy().into_owned()}else{String::new()},"storage_scope":if external{"local"}else{"private"},"planning_available":true,"plan_seeded":true,"plan_version":1,"goal":"","modules":[{"id":"main","title":"Main module","path":path}],"tasks":[]});
    let mut p = p;
    let seed = crate::planning::proposal(&p, &format!("Build {}", text(&p, "title")), "standard");
    for k in ["goal", "modules", "tasks"] {
        p[k] = seed[k].clone();
    }
    p["status"] = json!("active");
    p["created_at"] = json!(now());
    p["updated_at"] = json!(now());
    s.rows("projects", a).push(p.clone());
    s.save()?;
    Ok(p)
}
pub fn plan(s: &mut State, a: &Value, b: &Value, propose: bool) -> Result<Value> {
    let mut p = s.get("projects", a, text(b, "project_id"))?;
    if propose {
        return Ok(crate::planning::proposal(
            &p,
            text(b, "goal"),
            text(b, "scale"),
        ));
    }
    if number(b, "plan_version", -1) != number(&p, "plan_version", 1) {
        return Err(Error::new(409, "plan changed"));
    }
    for k in ["goal", "modules", "tasks"] {
        if let Some(v) = b.get(k) {
            p[k] = v.clone();
        }
    }
    p["plan_version"] = json!(number(&p, "plan_version", 1) + 1);
    replace(s, "projects", a, &p)?;
    Ok(p)
}
pub fn replace(s: &mut State, t: &str, a: &Value, p: &Value) -> Result<()> {
    let rows = s.rows(t, a);
    let old = rows
        .iter_mut()
        .find(|r| text(r, "id") == text(p, "id"))
        .ok_or_else(|| Error::new(404, "record missing"))?;
    *old = p.clone();
    s.save()
}
pub fn patch(s: &mut State, a: &Value, b: &Value, apply: bool) -> Result<Value> {
    let w = workspace(s, a)?;
    if !apply {
        let path = text(b, "path");
        let r = w.read(path)?;
        let id = id();
        let p = json!({"id":id,"patch_id":id,"path":path,"content":text(b,"content"),"expected_sha256":r["sha256"],"owner":a["id"],"diff":format!("--- {path}\n+++ {path}\n{}",text(b,"content"))});
        s.docs["patches"][&id] = p.clone();
        s.save()?;
        return Ok(p);
    }
    let id = if text(b, "patch_id").is_empty() {
        text(b, "id")
    } else {
        text(b, "patch_id")
    };
    let p = s.docs["patches"]
        .get(id)
        .cloned()
        .ok_or_else(|| Error::new(404, "preview missing"))?;
    if p["owner"] != a["id"] {
        return Err(Error::new(404, "preview missing"));
    }
    w.replace(
        text(&p, "path"),
        text(&p, "expected_sha256"),
        text(&p, "content"),
    )?;
    s.docs["patches"].as_object_mut().unwrap().remove(id);
    s.save()?;
    Ok(json!({"ok":true}))
}
pub fn attachments(s: &State, a: &Value, b: &Value, discard: bool) -> Result<Value> {
    let root = s.root(a)?;
    let base = root.join(".libai_agent/attachments");
    private_dir(&base)?;
    let draft = text(b, "attachment_draft");
    if discard {
        if !valid_draft(draft) {
            return Err(Error::bad("invalid attachment draft"));
        }
        let p = root.join(draft);
        if p.is_dir() && !fs::symlink_metadata(&p)?.file_type().is_symlink() {
            fs::remove_dir_all(p)?;
        }
        return Ok(json!({"ok":true}));
    }
    let files = b["files"]
        .as_array()
        .ok_or_else(|| Error::bad("files array required"))?;
    if files.is_empty() || files.len() > 8 {
        return Err(Error::bad("1 to 8 attachments required"));
    }
    let mut decoded = Vec::new();
    for f in files {
        let name = text(f, "name");
        if name.is_empty()
            || name.len() > 120
            || name == "."
            || name == ".."
            || !name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b" _.-".contains(&c))
        {
            return Err(Error::bad("invalid attachment name"));
        }
        let bytes = STANDARD
            .decode(text(f, "base64"))
            .map_err(|_| Error::bad("invalid base64"))?;
        let image = crate::workspace::mime(name)
            .map(|m| m.starts_with("image/"))
            .unwrap_or(false);
        if bytes.len() > if image { 8 * 1024 * 1024 } else { 32768 }
            || (!image && (bytes.contains(&0) || std::str::from_utf8(&bytes).is_err()))
        {
            return Err(Error::bad("invalid attachment size/content"));
        }
        decoded.push((name.to_string(), bytes));
    }
    let draft = format!(".libai_agent/attachments/{}", id());
    private_dir(&root.join(&draft))?;
    let mut paths = Vec::new();
    for (i, (name, bytes)) in decoded.into_iter().enumerate() {
        let path = format!("{draft}/{i}-{name}");
        fs::write(root.join(&path), bytes)?;
        paths.push(path);
    }
    Ok(json!({"ok":true,"attachment_draft":draft,"attachments":paths}))
}
pub fn valid_draft(d: &str) -> bool {
    d.strip_prefix(".libai_agent/attachments/")
        .map(|i| i.len() == 64 && i.bytes().all(|c| c.is_ascii_hexdigit()))
        .unwrap_or(false)
}
fn permission(s: &State, owner: &Value, actor: &Value, project: &str) -> String {
    if owner["id"] == actor["id"] {
        "owner".into()
    } else {
        text(
            &s.docs["grants"],
            &format!("{}:{project}:{}", text(owner, "id"), text(actor, "id")),
        )
        .into()
    }
}
pub fn collaborate(s: &mut State, a: &Value, op: &str, method: &str, b: &Value) -> Result<Value> {
    let accounts = s
        .accounts
        .as_object()
        .unwrap()
        .values()
        .cloned()
        .collect::<Vec<_>>();
    if op == "projects" {
        let mut out = Vec::new();
        for owner in &accounts {
            if !flag(owner, "enabled", true) {
                continue;
            }
            for p in s.rows("projects", owner).clone() {
                let access = permission(s, owner, a, text(&p, "project_id"));
                if access.is_empty() {
                    continue;
                }
                let mut p = p;
                p["owner"] = owner["username"].clone();
                p["owner_id"] = owner["id"].clone();
                p["permission"] = json!(access);
                p["shared"] = json!(access != "owner");
                out.push(p);
            }
        }
        return Ok(json!({"ok":true,"projects":out}));
    }
    let owner = accounts
        .iter()
        .find(|o| text(o, "id") == text(b, "owner_id") && flag(o, "enabled", true))
        .ok_or_else(|| Error::new(404, "owner missing"))?;
    let access = permission(s, owner, a, text(b, "project_id"));
    if access.is_empty() {
        return Err(Error::new(403, "project access denied"));
    }
    let p = s.get("projects", owner, text(b, "project_id"))?;
    if op == "settings" {
        if method == "POST" {
            if access != "owner" {
                return Err(Error::new(403, "owner required"));
            }
            let requested = text(b, "permission");
            if !["read", "write", "remove"].contains(&requested) {
                return Err(Error::bad("invalid permission"));
            }
            let target = accounts
                .iter()
                .find(|a| text(a, "username") == text(b, "username"))
                .ok_or_else(|| Error::new(404, "user missing"))?;
            if !flag(target, "enabled", true) && requested != "remove" {
                return Err(Error::new(404, "disabled user"));
            }
            if target["id"] == owner["id"] {
                return Err(Error::bad("owner permission immutable"));
            }
            let key = format!(
                "{}:{}:{}",
                text(owner, "id"),
                text(&p, "project_id"),
                text(target, "id")
            );
            if requested == "remove" {
                s.docs["grants"].as_object_mut().unwrap().remove(&key);
            } else {
                s.docs["grants"][&key] = json!(requested);
            }
            s.save()?;
        }
        let users=accounts.iter().map(|u|json!({"username":u["username"],"permission":permission(s,owner,u,text(&p,"project_id")),"enabled":u["enabled"]})).collect::<Vec<_>>();
        let members = users
            .iter()
            .filter(|u| !text(u, "permission").is_empty())
            .cloned()
            .collect::<Vec<_>>();
        let mut out = json!({"ok":true,"members":members,"permission":access});
        if access == "owner" {
            out["users"] = json!(users);
        }
        return Ok(out);
    }
    let w = Workspace {
        root: workspace(s, owner)?.resolve(text(&p, "path"), false)?,
        projects: vec![],
    };
    let path = text(b, "path");
    match op {
        "list" => w.list(path),
        "read" => w.read(path),
        "preview-asset" => w.asset(path),
        "save" => {
            if access == "read" {
                return Err(Error::new(403, "read-only project"));
            }
            w.replace(path, text(b, "sha256"), text(b, "content"))?;
            Ok(json!({"ok":true}))
        }
        _ => Err(Error::new(404, "route missing")),
    }
}
