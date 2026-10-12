//! Independent Rust HTTP application. Only generic libai Agent/CodingService cross FFI.
use crate::{auth, projects, runs, store::*};
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    fs,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
};
#[derive(Debug)]
pub struct Error {
    pub status: u16,
    pub message: String,
}
pub type Result<T> = std::result::Result<T, Error>;
impl Error {
    pub fn new(status: u16, message: &str) -> Self {
        Self {
            status,
            message: message.into(),
        }
    }
    pub fn bad(message: &str) -> Self {
        Self::new(400, message)
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Self::bad(&e.to_string())
    }
}
pub struct Core {
    pub state: Mutex<State>,
    authority: String,
    pub helper: String,
    stopped: AtomicBool,
    _lock: fs::File,
}
#[derive(Clone)]
pub struct App(pub Arc<Core>);
fn decode(s: &str) -> Result<String> {
    let mut b = Vec::new();
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' => {
                if i + 2 >= bytes.len() {
                    return Err(Error::bad("invalid URL escape"));
                }
                let n = u8::from_str_radix(&s[i + 1..i + 3], 16)
                    .map_err(|_| Error::bad("invalid URL escape"))?;
                if n == 0 {
                    return Err(Error::bad("NUL URL"));
                }
                b.push(n);
                i += 3;
            }
            b'+' => {
                b.push(b' ');
                i += 1;
            }
            c => {
                b.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8(b).map_err(|_| Error::bad("invalid URL UTF-8"))
}
fn query(target: &str) -> Result<Value> {
    let mut b = json!({});
    if let Some((_, q)) = target.split_once('?') {
        for part in q.split('&') {
            let (k, v) = part.split_once('=').unwrap_or((part, ""));
            let k = decode(k)?;
            if b.get(&k).is_some() {
                return Err(Error::bad("duplicate parameter"));
            }
            b[&k] = json!(decode(v)?);
        }
    }
    Ok(b)
}
fn fields(h: &HashMap<String, String>) -> String {
    h.iter().map(|(k, v)| format!("{k}: {v}\r\n")).collect()
}
impl App {
    pub fn new(config: &str) -> Result<Self> {
        let cfg: Value = serde_json::from_str(config).map_err(|_| Error::bad("invalid config"))?;
        let data = std::env::current_dir()?.join(text(&cfg, "data"));
        private_dir(&data)?;
        let data = data.canonicalize()?;
        let workspace = if text(&cfg, "workspace").is_empty() {
            data.join("workspace")
        } else {
            std::env::current_dir()?.join(text(&cfg, "workspace"))
        };
        private_dir(&workspace)?;
        let workspace = workspace.canonicalize()?;
        for p in ["auth", "users", "shared"] {
            if workspace.starts_with(data.join(p)) || data.join(p).starts_with(&workspace) {
                return Err(Error::bad("workspace overlaps private account storage"));
            }
        }
        let lock = fs::OpenOptions::new()
            .create(true)
            .write(true)
            .open(data.join("server.lock"))?;
        #[cfg(unix)]
        {
            use std::os::fd::AsRawFd;
            extern "C" {
                fn flock(fd: i32, operation: i32) -> i32;
            }
            if unsafe { flock(lock.as_raw_fd(), 2 | 4) } != 0 {
                return Err(Error::bad("data directory already in use"));
            }
        }
        if !std::path::Path::new(text(&cfg, "html"))
            .join("index.html")
            .is_file()
        {
            return Err(Error::bad("UI files missing"));
        }
        projects::coding(&data, "storage.migrate", &json!({}))?;
        let mut state = State::new(
            data,
            workspace,
            std::env::current_dir()?.join(text(&cfg, "html")),
        )?;
        if state.policy.get("allow_browser_debug").is_none() {
            state.policy["allow_browser_debug"] = json!(false);
        }
        let mut effective =
            projects::coding(&state.workspace, "runtime.policy.configure", &state.policy)?;
        effective.as_object_mut().unwrap().remove("ok");
        effective
            .as_object_mut()
            .unwrap()
            .extend(state.policy.as_object().unwrap().clone());
        state.policy = effective;
        write(
            &state.data.join(".webcool_settings/ai-policy.v1"),
            &state.policy,
        )?;
        state.save()?;
        Ok(Self(Arc::new(Core {
            state: Mutex::new(state),
            authority: text(&cfg, "authority").into(),
            helper: text(&cfg, "sandbox_helper").into(),
            stopped: AtomicBool::new(false),
            _lock: lock,
        })))
    }
    pub fn stop(&self) {
        self.0.stopped.store(true, Ordering::Relaxed);
        let mut s = self.0.state.lock().unwrap();
        for j in s.jobs.values() {
            j.cancellation.cancel();
        }
        crate::sandbox::stop(&mut s);
    }
    fn send(
        &self,
        sink: &mut impl FnMut(u16, &str, &[u8]) -> bool,
        status: u16,
        v: &Value,
        h: &mut HashMap<String, String>,
    ) {
        let body = v.to_string();
        h.insert(
            "Content-Type".into(),
            "application/json; charset=utf-8".into(),
        );
        h.insert("Content-Length".into(), body.len().to_string());
        sink(status, &fields(h), body.as_bytes());
    }
    fn asset(
        &self,
        path: &str,
        h: &mut HashMap<String, String>,
        sink: &mut impl FnMut(u16, &str, &[u8]) -> bool,
    ) -> Result<bool> {
        let known = [
            "/",
            "/coolder.js",
            "/app.js",
            "/admin.js",
            "/i18n.js",
            "/personal.js",
            "/code-editor.js",
            "/attachments.js",
            "/style.css",
        ];
        if !known.contains(&path) && !path.starts_with("/vendor/") && !path.starts_with("/i18n/") {
            return Ok(false);
        }
        let rel = if path == "/" {
            "index.html"
        } else {
            &path[1..]
        };
        if rel.contains("..")
            || !rel
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"_./-".contains(&c))
        {
            return Err(Error::new(404, "asset not found"));
        }
        let mut f = self.0.state.lock().unwrap().html.clone();
        for c in rel.split('/') {
            f.push(c);
            if fs::symlink_metadata(&f)
                .map(|m| m.file_type().is_symlink())
                .unwrap_or(false)
            {
                return Err(Error::new(404, "asset not found"));
            }
        }
        if !f.is_file() {
            return Err(Error::new(404, "asset not found"));
        }
        let ext = f.extension().and_then(|s| s.to_str()).unwrap_or("");
        let mime = match ext {
            "js" => "text/javascript; charset=utf-8",
            "css" => "text/css; charset=utf-8",
            "html" => "text/html; charset=utf-8",
            "json" => "application/json; charset=utf-8",
            "woff" => "font/woff",
            "woff2" => "font/woff2",
            "ttf" => "font/ttf",
            _ => return Err(Error::new(404, "asset not found")),
        };
        if path.starts_with("/vendor/") && ext == "json" {
            return Err(Error::new(404, "asset not found"));
        }
        let mut bytes = fs::read(f)?;
        if path == "/" {
            let csp = &h["Content-Security-Policy"];
            let nonce = csp
                .split("nonce-")
                .nth(1)
                .unwrap()
                .split('\'')
                .next()
                .unwrap();
            bytes = String::from_utf8(bytes)
                .map_err(|_| Error::bad("invalid HTML"))?
                .replace(
                    "</head>",
                    &format!("<meta name=\"preview-script-nonce\" content=\"{nonce}\">\n</head>"),
                )
                .into_bytes();
        }
        h.insert("Content-Type".into(), mime.into());
        h.insert("Content-Length".into(), bytes.len().to_string());
        sink(200, &fields(h), &bytes);
        Ok(true)
    }
    pub fn dispatch(
        &self,
        method: &str,
        target: &str,
        headers: &str,
        body: &[u8],
        mut sink: impl FnMut(u16, &str, &[u8]) -> bool,
    ) -> Result<()> {
        let mut out=HashMap::from([("X-Content-Type-Options".into(),"nosniff".into()),("Cache-Control".into(),"no-store".into()),("Content-Security-Policy".into(),format!("default-src 'self'; script-src 'self' 'nonce-{}'; style-src 'self' 'unsafe-inline'; worker-src 'self'; font-src 'self' data:; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'",&id()[..32]))]);
        let result = (|| -> Result<()> {
            if self.0.stopped.load(Ordering::Relaxed) {
                return Err(Error::new(503, "stopped"));
            }
            let h = headers
                .split("\r\n")
                .filter_map(|l| l.split_once(':'))
                .map(|(k, v)| (k.to_ascii_lowercase(), v.trim().to_string()))
                .collect::<HashMap<_, _>>();
            if h.get("host") != Some(&self.0.authority) {
                return Err(Error::new(403, "invalid Host"));
            }
            if h.get("origin")
                .map(|o| o != &format!("http://{}", self.0.authority))
                .unwrap_or(false)
            {
                return Err(Error::new(403, "cross-origin request denied"));
            }
            let path = decode(target.split('?').next().unwrap())?;
            verify_method(&path, method)?;
            let b = if method == "POST" {
                let ct = h
                    .get("content-type")
                    .map(String::as_str)
                    .unwrap_or("")
                    .split(';')
                    .next()
                    .unwrap()
                    .trim()
                    .to_ascii_lowercase();
                if ct != "application/json" && ct != "text/json" {
                    return Err(Error::bad("JSON Content-Type required"));
                }
                let v: Value =
                    serde_json::from_slice(body).map_err(|_| Error::bad("invalid JSON body"))?;
                if !v.is_object()
                    || body.contains(&0)
                    || String::from_utf8_lossy(body).contains("\\u0000")
                {
                    return Err(Error::bad("invalid JSON object"));
                }
                v
            } else {
                query(target)?
            };
            if path.starts_with("/api/v1/auth/") || path == "/api/login" || path == "/api/logout" {
                let op = if path.starts_with("/api/v1/auth/") {
                    &path[13..]
                } else {
                    &path[5..]
                };
                let v = auth::route(
                    &mut self.0.state.lock().unwrap(),
                    op,
                    method,
                    &b,
                    &h,
                    &mut out,
                )?;
                self.send(&mut sink, 200, &v, &mut out);
                return Ok(());
            }
            if method == "GET" && self.asset(&path, &mut out, &mut sink)? {
                return Ok(());
            }
            let actor = auth::current(&self.0.state.lock().unwrap(), &h)
                .ok_or_else(|| Error::new(401, "authentication required"))?;
            if path == "/api/health" {
                self.send(
                    &mut sink,
                    200,
                    &json!({"ok":true,"app":"coolder-rust","version":"1.0.0"}),
                    &mut out,
                );
                return Ok(());
            }
            if path == "/api/v1/ai/runs/events" {
                return self.stream(&actor, text(&b, "id"), &mut sink, &mut out);
            }
            let (status, v) =
                self.route(&mut self.0.state.lock().unwrap(), &actor, &path, method, &b)?;
            self.send(&mut sink, status, &v, &mut out);
            Ok(())
        })();
        if let Err(e) = result {
            self.send(
                &mut sink,
                e.status,
                &json!({"ok":false,"error":e.message}),
                &mut out,
            );
        }
        Ok(())
    }
    fn route(
        &self,
        s: &mut State,
        a: &Value,
        path: &str,
        method: &str,
        b: &Value,
    ) -> Result<(u16, Value)> {
        let mut status = 200;
        if path == "/api/v1/admin/ai-policy" {
            auth::admin(a)?;
            if method == "POST" {
                let mut policy = s.policy.clone();
                for (k, v) in b.as_object().unwrap() {
                    policy[k] = v.clone();
                }
                validate_policy(&policy)?;
                projects::coding(&s.workspace, "runtime.policy.validate", &policy)?;
                write(&s.data.join(".webcool_settings/ai-policy.v1"), &policy)?;
                projects::coding(&s.workspace, "runtime.policy.configure", &policy)?;
                s.policy = policy;
            }
            return Ok((200, s.policy.clone()));
        }
        if path.starts_with("/api/v1/collaborate/") {
            return Ok((200, projects::collaborate(s, a, &path[20..], method, b)?));
        }
        let op = path
            .strip_prefix("/api/v1/ai/")
            .ok_or_else(|| Error::new(404, "route missing"))?;
        if !op.starts_with("providers")
            && (!flag(&s.policy, "enabled", true)
                || (!flag(a, "admin", false) && !flag(&s.policy, "allow_users", true)))
        {
            return Err(Error::new(403, "AI access disabled"));
        }
        let v = match op {
            "providers" => projects::coding(&s.workspace, "providers.list", b)?,
            "providers/save" | "providers/delete" | "providers/test" => {
                auth::admin(a)?;
                let args = b.clone();
                let native = match op {
                    "providers/delete" => "providers.remove",
                    "providers/test" => "providers.test",
                    _ => "providers.save",
                };
                let r = projects::coding(&s.workspace, native, &args)?;
                if op == "providers/save" {
                    json!({"ok":true,"provider":r})
                } else {
                    r
                }
            }
            "agents" => projects::coding(&s.workspace, "agents.list", &json!({}))?,
            "attachments" | "attachments/discard" => {
                projects::attachments(s, a, b, op.ends_with("discard"))?
            }
            "workspace/project/create" | "projects/ensure" | "projects/import-git" => {
                projects::create(s, a, b, op == "workspace/project/create")?
            }
            "projects/directories" => {
                if !flag(a, "admin", false) && !flag(&s.policy, "allow_users_local_projects", false)
                {
                    return Err(Error::new(403, "local access disabled"));
                }
                let p = if text(b, "path").is_empty() {
                    std::env::var("HOME").unwrap_or_else(|_| "/".into())
                } else {
                    text(b, "path").into()
                };
                let dir = PathBuf::from(p);
                if !dir.is_absolute()
                    || !dir.is_dir()
                    || fs::symlink_metadata(&dir)?.file_type().is_symlink()
                {
                    return Err(Error::bad("invalid directory"));
                }
                let mut entries = Vec::new();
                for e in fs::read_dir(&dir)? {
                    let e = e?;
                    if e.file_type()?.is_dir() && !e.file_type()?.is_symlink() {
                        entries.push(json!({"name":e.file_name().to_string_lossy(),"path":e.path().to_string_lossy(),"directory":true}));
                    }
                }
                json!({"path":dir.canonicalize()?.to_string_lossy(),"entries":entries})
            }
            "projects" => {
                if text(b, "id").is_empty() {
                    json!({"ok":true,"projects":s.rows("projects",a),"local_project_location_allowed":flag(a,"admin",false)||flag(&s.policy,"allow_users_local_projects",false),"shared_project_location_allowed":false})
                } else {
                    s.get("projects", a, text(b, "id"))?
                }
            }
            "projects/delete" => {
                let p = s.get("projects", a, text(b, "project_id"))?;
                s.rows("projects", a).retain(|r| r != &p);
                s.save()?;
                json!({"ok":true})
            }
            "projects/plan" | "projects/plan/propose" => {
                projects::plan(s, a, b, op.ends_with("propose"))?
            }
            "projects/tasks/status" => {
                let mut p = s.get("projects", a, text(b, "project_id"))?;
                if number(b, "plan_version", -1) != number(&p, "plan_version", 1) {
                    return Err(Error::new(409, "plan changed"));
                }
                for t in p["tasks"].as_array_mut().unwrap() {
                    if text(t, "id") == text(b, "task_id") {
                        t["status"] = json!(text(b, "status"));
                    }
                }
                p["plan_version"] = json!(number(&p, "plan_version", 1) + 1);
                projects::replace(s, "projects", a, &p)?;
                p
            }
            "projects/workflow" => crate::planning::workflow(s, a, b, method)?,
            "projects/index" | "projects/index/refresh" => {
                crate::planning::index(s, a, b, op.ends_with("refresh"))?
            }
            "workspace/list" => projects::workspace(s, a)?.list(text(b, "path"))?,
            "workspace/read" => projects::workspace(s, a)?.read(text(b, "path"))?,
            "workspace/search" => {
                projects::workspace(s, a)?.search(text(b, "path"), text(b, "query"))?
            }
            "workspace/directory/create" => {
                fs::create_dir(projects::workspace(s, a)?.resolve(text(b, "path"), false)?)?;
                json!({"ok":true})
            }
            "workspace/patch/preview" | "workspace/patch/apply" => {
                projects::patch(s, a, b, op.ends_with("apply"))?
            }
            "workspace/change-set/preview" | "workspace/change-set/apply" => {
                let root = s.root(a)?;
                let args = if op.ends_with("preview") {
                    b.clone()
                } else {
                    json!({"id":text(b,"change_set_id")})
                };
                let r = projects::coding(
                    &root,
                    if op.ends_with("preview") {
                        "changes.preview"
                    } else {
                        "changes.apply"
                    },
                    &args,
                );
                match r {
                    Ok(v) => v,
                    Err(mut e) => {
                        if op.ends_with("apply") {
                            e.status = 409;
                        }
                        return Err(e);
                    }
                }
            }
            "runs/start" => {
                status = 202;
                runs::start(self, s, a, b)?
            }
            "runs/status" => runs::status(s, a, text(b, "id"))?,
            "runs" => json!({"ok":true,"runs":s.rows("runs",a)}),
            op if op.starts_with("runs/") => {
                if op == "runs/cancel" || op == "runs/pause" {
                    status = 202;
                }
                runs::control(s, a, &op[5..], b)?
            }
            "sessions" => {
                json!({"ok":true,"sessions":s.rows("sessions",a).iter().filter(|r|text(b,"project_id").is_empty()||text(r,"project_id")==text(b,"project_id")).collect::<Vec<_>>()})
            }
            "sessions/save" => {
                let mut r = s.get("sessions", a, text(b, "session_id"))?;
                r["title"] = json!(text(b, "title"));
                projects::replace(s, "sessions", a, &r)?;
                r
            }
            "sessions/delete" => {
                let r = s.get("sessions", a, text(b, "session_id"))?;
                s.rows("sessions", a).retain(|v| v != &r);
                s.save()?;
                json!({"ok":true})
            }
            op if op.starts_with("sandbox/") => {
                if op == "sandbox/execute" || op == "sandbox/runs/cancel" {
                    status = 202;
                }
                crate::sandbox::route(self, s, a, &op[8..], b)?
            }
            _ => return Err(Error::new(404, "route not found")),
        };
        Ok((status, v))
    }
    fn stream(
        &self,
        a: &Value,
        id: &str,
        sink: &mut impl FnMut(u16, &str, &[u8]) -> bool,
        out: &mut HashMap<String, String>,
    ) -> Result<()> {
        self.0.state.lock().unwrap().get("runs", a, id)?;
        out.insert("Content-Type".into(), "text/event-stream".into());
        out.insert("Cache-Control".into(), "no-cache".into());
        if !sink(200, &fields(out), &[]) {
            return Ok(());
        }
        while !self.0.stopped.load(Ordering::Relaxed) {
            let r = runs::status(&mut self.0.state.lock().unwrap(), a, id)?;
            let text = format!("event: status\ndata: {r}\n\n");
            if !sink(200, "", text.as_bytes()) || crate::store::text(&r, "status") != "running" {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        Ok(())
    }
}
fn validate_policy(p: &Value) -> Result<()> {
    if flag(p, "allow_browser_debug", false)
        || flag(p, "allow_users_shared_projects", false)
        || number(p, "sandbox_memory_mib", 512) < 64
        || number(p, "read_chunk_kib", 16) > number(p, "read_file_limit_kib", 512)
        || p.as_object()
            .unwrap()
            .values()
            .any(|v| v.as_i64().map(|n| n < 0).unwrap_or(false))
    {
        return Err(Error::bad("unsupported or invalid policy"));
    }
    Ok(())
}

fn verify_method(path: &str, method: &str) -> Result<()> {
    let both = [
        "/api/v1/auth/preferences",
        "/api/v1/admin/ai-policy",
        "/api/v1/collaborate/settings",
        "/api/v1/ai/projects/workflow",
    ]
    .contains(&path);
    let post = [
        "/api/login",
        "/api/logout",
        "/api/v1/auth/register",
        "/api/v1/auth/login",
        "/api/v1/auth/logout",
        "/api/v1/auth/password",
        "/api/v1/auth/users/create",
        "/api/v1/auth/users/update",
        "/api/v1/collaborate/save",
        "/api/v1/ai/attachments",
        "/api/v1/ai/attachments/discard",
    ]
    .contains(&path)
        || [
            "/api/v1/ai/workspace/patch/",
            "/api/v1/ai/workspace/change-set/",
            "/api/v1/ai/runs/result/",
        ]
        .iter()
        .any(|p| path.starts_with(p))
        || [
            "providers/save",
            "providers/delete",
            "providers/test",
            "workspace/project/create",
            "workspace/directory/create",
            "runs/start",
            "runs/cancel",
            "runs/pause",
            "runs/reasoning/save",
            "sessions/save",
            "sessions/delete",
            "projects/delete",
            "projects/import-git",
            "projects/ensure",
            "projects/plan",
            "projects/plan/propose",
            "projects/tasks/status",
            "projects/index/refresh",
            "sandbox/plan",
            "sandbox/execute",
            "sandbox/runs/cancel",
        ]
        .iter()
        .any(|p| path == format!("/api/v1/ai/{p}"));
    if !both && method != if post { "POST" } else { "GET" } {
        return Err(Error::new(405, "method not allowed"));
    }
    Ok(())
}
