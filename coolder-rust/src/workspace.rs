//! Rust path confinement, binary assets and optimistic workspace edits.
use crate::{
    app::{Error, Result},
    store::*,
};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
};
pub struct Workspace {
    pub root: PathBuf,
    pub projects: Vec<Value>,
}
pub fn normalized(p: &str, empty: bool) -> Result<()> {
    if p.is_empty() && empty {
        return Ok(());
    }
    if p.is_empty()
        || p.starts_with('/')
        || p.contains('\\')
        || p.contains('\0')
        || p.split('/').any(|c| {
            [
                "",
                ".",
                "..",
                ".libai_agent",
                ".git",
                ".env",
                "secrets",
                "private",
            ]
            .contains(&c)
        })
    {
        return Err(Error::bad("private or invalid path"));
    }
    Ok(())
}
impl Workspace {
    pub fn resolve(&self, p: &str, empty: bool) -> Result<PathBuf> {
        normalized(p, empty)?;
        let mut base = self.root.clone();
        let mut relative = p;
        for pr in &self.projects {
            let alias = text(pr, "path");
            let ext = text(pr, "external");
            if !ext.is_empty() && (p == alias || p.starts_with(&format!("{alias}/"))) {
                base = PathBuf::from(ext);
                relative = if p == alias {
                    ""
                } else {
                    &p[alias.len() + 1..]
                };
                break;
            }
        }
        if fs::symlink_metadata(&base)
            .map(|m| m.file_type().is_symlink())
            .unwrap_or(false)
        {
            return Err(Error::bad("symlink root"));
        }
        let mut f = base;
        for c in Path::new(relative) {
            f.push(c);
            if fs::symlink_metadata(&f)
                .map(|m| m.file_type().is_symlink())
                .unwrap_or(false)
            {
                return Err(Error::bad("symlink path"));
            }
        }
        Ok(f)
    }
    pub fn read(&self, p: &str) -> Result<Value> {
        let f = self.resolve(p, false)?;
        if !f.is_file() || fs::metadata(&f)?.len() > 1024 * 1024 {
            return Err(Error::bad("invalid file or file too large"));
        }
        let b = fs::read(f)?;
        let content = String::from_utf8(b.clone()).map_err(|_| Error::bad("file is not UTF-8"))?;
        Ok(json!({"ok":true,"path":p,"content":content,"sha256":hash(&b),"truncated":false}))
    }
    pub fn list(&self, p: &str) -> Result<Value> {
        let d = self.resolve(p, true)?;
        if !d.is_dir() {
            return Err(Error::bad("directory missing"));
        }
        let mut entries = Vec::new();
        for e in fs::read_dir(d)? {
            let e = e?;
            let name = e.file_name().to_string_lossy().into_owned();
            let t = e.file_type()?;
            if name.starts_with('.') || t.is_symlink() {
                continue;
            }
            entries.push(json!({"name":name,"path":if p.is_empty(){name}else{format!("{p}/{name}")},"directory":t.is_dir(),"size":if t.is_dir(){0}else{e.metadata()?.len()}}));
        }
        entries.sort_by_key(|e| text(e, "path").to_string());
        Ok(json!({"ok":true,"path":p,"entries":entries}))
    }
    pub fn replace(&self, p: &str, expected: &str, content: &str) -> Result<()> {
        let f = self.resolve(p, false)?;
        if !f.is_file() || !equal(&hash(&fs::read(&f)?), expected) {
            return Err(Error::new(409, "file changed"));
        }
        fs::write(f, content)?;
        Ok(())
    }
    pub fn asset(&self, p: &str) -> Result<Value> {
        let f = self.resolve(p, false)?;
        let mime = mime(p).ok_or_else(|| Error::bad("invalid preview asset"))?;
        if !f.is_file() || fs::metadata(&f)?.len() > 2 * 1024 * 1024 {
            return Err(Error::bad("invalid preview asset"));
        }
        Ok(json!({"ok":true,"mime":mime,"base64":STANDARD.encode(fs::read(f)?)}))
    }
    pub fn search(&self, p: &str, q: &str) -> Result<Value> {
        let d = self.resolve(p, true)?;
        let mut matches = Vec::new();
        self.search_dir(&d, q, 0, &mut matches)?;
        Ok(json!({"ok":true,"truncated":matches.len()>=500,"matches":matches}))
    }
    fn search_dir(&self, d: &Path, q: &str, depth: usize, m: &mut Vec<Value>) -> Result<()> {
        if depth > 12 || m.len() >= 500 {
            return Ok(());
        }
        for e in fs::read_dir(d)? {
            let e = e?;
            if e.file_name().to_string_lossy().starts_with('.') || e.file_type()?.is_symlink() {
                continue;
            }
            if e.file_type()?.is_dir() {
                self.search_dir(&e.path(), q, depth + 1, m)?;
            } else if e.metadata()?.len() < 1024 * 1024 {
                if let Ok(s) = fs::read_to_string(e.path()) {
                    for (i, l) in s.lines().enumerate() {
                        if l.contains(q) && m.len() < 500 {
                            m.push(json!({"path":e.path().strip_prefix(&self.root).unwrap_or(&e.path()).to_string_lossy(),"line":i+1,"text":l}));
                        }
                    }
                }
            }
        }
        Ok(())
    }
}
pub fn mime(p: &str) -> Option<&'static str> {
    match Path::new(p)
        .extension()?
        .to_str()?
        .to_ascii_lowercase()
        .as_str()
    {
        "png" => Some("image/png"),
        "jpg" | "jpeg" => Some("image/jpeg"),
        "gif" => Some("image/gif"),
        "webp" => Some("image/webp"),
        "svg" => Some("image/svg+xml"),
        "ico" => Some("image/x-icon"),
        "woff" => Some("font/woff"),
        "woff2" => Some("font/woff2"),
        "ttf" => Some("font/ttf"),
        _ => None,
    }
}
