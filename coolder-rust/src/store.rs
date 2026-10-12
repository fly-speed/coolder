//! Native Rust private storage and password verification. No application FFI.
use crate::app::{Error, Result};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};
pub fn text<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(Value::as_str).unwrap_or("")
}
pub fn number(v: &Value, k: &str, d: i64) -> i64 {
    v.get(k).and_then(Value::as_i64).unwrap_or(d)
}
pub fn flag(v: &Value, k: &str, d: bool) -> bool {
    v.get(k).and_then(Value::as_bool).unwrap_or(d)
}
pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs()
}
pub fn hex(b: &[u8]) -> String {
    b.iter().map(|v| format!("{v:02x}")).collect()
}
pub fn id() -> String {
    let mut b = [0; 32];
    getrandom::getrandom(&mut b).expect("secure OS randomness");
    hex(&b)
}
pub fn hash(b: &[u8]) -> String {
    hex(&Sha256::digest(b))
}
pub fn password(p: &str, s: &str) -> String {
    let mut b = [0; 32];
    pbkdf2::pbkdf2_hmac::<Sha256>(p.as_bytes(), s.as_bytes(), 210000, &mut b);
    hex(&b)
}
pub fn equal(a: &str, b: &str) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.bytes().zip(b.bytes()).fold(0u8, |v, (a, b)| v | (a ^ b)) == 0
}
pub fn private_dir(p: &Path) -> Result<()> {
    if fs::symlink_metadata(p)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(Error::bad("private directory is a symlink"));
    }
    fs::create_dir_all(p)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(p, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}
pub fn read(p: &Path, default: Value) -> Result<Value> {
    if fs::symlink_metadata(p)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(Error::bad("private file is a symlink"));
    }
    if !p.exists() {
        return Ok(default);
    }
    Ok(serde_json::from_slice(&fs::read(p)?)
        .map_err(|_| Error::bad("invalid account store or app state"))?)
}
pub fn shared(canonical: &Path, first: &Path, second: &Path, default: Value) -> Result<Value> {
    if fs::symlink_metadata(canonical).is_ok() {
        return read(canonical, default);
    }
    let a = read(first, Value::Null)?;
    let b = read(second, Value::Null)?;
    if !a.is_null() && !b.is_null() && a != b {
        return Err(Error::bad("conflicting legacy JSON state files"));
    }
    Ok(if !a.is_null() {
        a
    } else if !b.is_null() {
        b
    } else {
        default
    })
}
pub fn write(p: &Path, v: &Value) -> Result<()> {
    private_dir(p.parent().unwrap())?;
    if fs::symlink_metadata(p)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(Error::bad("private file is a symlink"));
    }
    let temp = p.with_file_name(format!(".save-{}", id()));
    let mut file = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temp)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        file.set_permissions(fs::Permissions::from_mode(0o600))?;
    }
    file.write_all(v.to_string().as_bytes())?;
    file.sync_all()?;
    fs::rename(temp, p)?;
    Ok(())
}
pub struct State {
    pub accounts: Value,
    pub docs: Value,
    pub policy: Value,
    pub tokens: HashMap<String, (String, u64)>,
    pub attempts: HashMap<String, (usize, u64)>,
    pub jobs: HashMap<String, std::sync::Arc<crate::runs::Job>>,
    pub data: PathBuf,
    pub workspace: PathBuf,
    pub html: PathBuf,
}
impl State {
    pub fn new(data: PathBuf, workspace: PathBuf, html: PathBuf) -> Result<Self> {
        private_dir(&data.join("auth"))?;
        let accounts = crate::accounts::load(&data.join("auth/accounts.v1"))?;
        let mut docs = shared(
            &data.join("auth/app-state.v1"),
            &data.join("auth/app-state-rust.v1"),
            &data.join("auth/app-state-java.v1"),
            json!({"projects":{},"sessions":{},"runs":{},"grants":{},"patches":{}}),
        )?;
        for category in ["projects", "sessions", "runs", "grants", "patches"] {
            if !docs[category].is_object() {
                return Err(Error::bad("invalid app state category"));
            }
        }
        for rows in docs["runs"]
            .as_object_mut()
            .ok_or_else(|| Error::bad("invalid run store"))?
            .values_mut()
        {
            for r in rows
                .as_array_mut()
                .ok_or_else(|| Error::bad("invalid run records"))?
            {
                if text(r, "status") == "running" {
                    r["status"] = json!("interrupted");
                    r["recovery_available"] = json!(r.get("checkpoint").is_some());
                }
            }
        }
        write(&data.join("auth/app-state.v1"), &docs)?;
        let policy = shared(
            &data.join(".webcool_settings/ai-policy.v1"),
            &data.join("auth/ai-policy-rust.v1"),
            &data.join("auth/ai-policy-java.v1"),
            json!({"enabled":true,"allow_users":true,"allow_users_local_projects":false,"max_active_runs_per_user":3}),
        )?;
        Ok(Self {
            accounts,
            docs,
            policy,
            tokens: HashMap::new(),
            attempts: HashMap::new(),
            jobs: HashMap::new(),
            data,
            workspace,
            html,
        })
    }
    pub fn save(&self) -> Result<()> {
        write(&self.data.join("auth/app-state.v1"), &self.docs)
    }
    pub fn rows(&mut self, t: &str, a: &Value) -> &mut Vec<Value> {
        let owner = text(a, "id");
        let all = self.docs[t].as_object_mut().expect("record category");
        all.entry(owner.to_string())
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .expect("record array")
    }
    pub fn get(&mut self, t: &str, a: &Value, id: &str) -> Result<Value> {
        self.rows(t, a)
            .iter()
            .find(|r| text(r, "id") == id || text(r, "project_id") == id)
            .cloned()
            .ok_or_else(|| Error::new(404, "record not found"))
    }
    pub fn root(&self, a: &Value) -> Result<PathBuf> {
        let r = if flag(a, "admin", false) {
            self.workspace.clone()
        } else {
            self.data
                .join("users")
                .join(text(a, "id"))
                .join("workspace")
        };
        private_dir(&r)?;
        private_dir(&r.join(".webcool_agent/attachments"))?;
        Ok(r)
    }
}
