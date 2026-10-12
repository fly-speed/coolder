//! Common JSON account schema and validated migration of C++ text records.
use crate::{
    app::{Error, Result},
    store::*,
};
use serde_json::{json, Value};
use std::{collections::HashSet, fs, path::Path};
pub fn backup(path: &Path) -> Result<()> {
    let dest = path.with_file_name(format!(
        "{}.legacy",
        path.file_name().unwrap().to_string_lossy()
    ));
    if fs::symlink_metadata(&dest)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(Error::bad("migration backup is a symlink"));
    }
    if !dest.exists() {
        fs::copy(path, &dest)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(dest, fs::Permissions::from_mode(0o600))?;
        }
    }
    Ok(())
}
pub fn migrate_members(path: &Path) -> Result<()> {
    if !path.exists() {
        return Ok(());
    }
    if fs::symlink_metadata(path)?.file_type().is_symlink() {
        return Err(Error::bad("membership file is a symlink"));
    }
    let source = fs::read_to_string(path)?;
    if source.trim_start().starts_with('{') {
        read(path, json!({}))?;
        return Ok(());
    }
    let mut rows = Vec::new();
    for line in source.lines().filter(|l| !l.trim().is_empty()) {
        let fields = line
            .split_whitespace()
            .map(|v| serde_json::from_str::<String>(v))
            .collect::<std::result::Result<Vec<_>, _>>()
            .map_err(|_| Error::bad("invalid legacy membership"))?;
        if fields.len() != 4 || !["read", "write"].contains(&fields[3].as_str()) {
            return Err(Error::bad("invalid membership record"));
        }
        rows.push(json!({"owner":fields[0],"project":fields[1],"member":fields[2],"permission":fields[3]}));
    }
    backup(path)?;
    write(path, &json!({"version":1,"grants":rows}))?;
    Ok(())
}
pub fn load(path: &Path) -> Result<Value> {
    if fs::symlink_metadata(path)
        .map(|m| m.file_type().is_symlink())
        .unwrap_or(false)
    {
        return Err(Error::bad("account file is a symlink"));
    }
    if !path.exists() {
        return Ok(json!({}));
    }
    let source = fs::read_to_string(path)?;
    let legacy = !source.trim_start().starts_with('{');
    let mut accounts = if legacy {
        json!({})
    } else {
        serde_json::from_str::<Value>(&source).map_err(|_| Error::bad("invalid account JSON"))?
    };
    if legacy {
        let words: Vec<_> = source.split_whitespace().collect();
        if words.len() < 2
            || words[0] != "coolder-accounts-v1"
            || words[1] != "210000"
            || (words.len() - 2) % 6 != 0
        {
            return Err(Error::bad("invalid account store: legacy header"));
        }
        for row in words[2..].chunks(6) {
            if accounts.get(row[0]).is_some()
                || !["0", "1"].contains(&row[2])
                || !["0", "1"].contains(&row[3])
            {
                return Err(Error::bad("invalid account store: legacy record"));
            }
            accounts[row[0]] = json!({"username":row[0],"id":row[1],"admin":row[2]=="1","enabled":row[3]=="1","salt":row[4],"digest":row[5]});
        }
    }
    let rows = accounts
        .as_object()
        .ok_or_else(|| Error::bad("invalid account object"))?;
    let mut ids = HashSet::new();
    let mut admins = 0;
    for (name, a) in rows {
        if name.is_empty()
            || name.len() > 32
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b))
            || name != text(a, "username")
            || !a["admin"].is_boolean()
            || !a["enabled"].is_boolean()
        {
            return Err(Error::bad("invalid account record"));
        }
        for k in ["id", "salt", "digest"] {
            let t = text(a, k);
            if t.len() != 64
                || !t
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            {
                return Err(Error::bad("invalid account verifier"));
            }
        }
        if !ids.insert(text(a, "id")) {
            return Err(Error::bad("duplicate account identity"));
        }
        if flag(a, "admin", false) {
            admins += 1;
            if !flag(a, "enabled", false) {
                return Err(Error::bad("administrator disabled"));
            }
        }
    }
    if admins > 1 {
        return Err(Error::bad(
            "account store cannot contain multiple administrators",
        ));
    }
    remember_admin(path, &accounts)?;
    migrate_members(&path.parent().unwrap().join("project-members.v1"))?;
    for a in rows.values() {
        preferences(
            &path
                .parent()
                .unwrap()
                .join(format!("ui-{}.v1", text(a, "id"))),
        )?;
    }
    if legacy {
        backup(path)?;
        write(path, &accounts)?;
    }
    Ok(accounts)
}
pub fn preferences(path: &Path) -> Result<Value> {
    if !path.exists() {
        return Ok(json!({"language":"zh","theme":"default","font_size":"md"}));
    }
    if fs::symlink_metadata(path)?.file_type().is_symlink() {
        return Err(Error::bad("preferences file is a symlink"));
    }
    let source = fs::read_to_string(path)?;
    if source.trim_start().starts_with('{') {
        return read(path, json!({}));
    }
    let w: Vec<_> = source.split_whitespace().collect();
    if w.len() != 3
        || w[0].is_empty()
        || w[0].len() > 24
        || !w[0].bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
        || ![
            "default", "blue", "green", "purple", "sand", "gray", "pink", "ocean",
        ]
        .contains(&w[1])
        || !["sm", "md", "lg"].contains(&w[2])
    {
        return Err(Error::bad("invalid legacy preferences"));
    }
    let v = json!({"language":w[0],"theme":w[1],"font_size":w[2]});
    backup(path)?;
    write(path, &v)?;
    Ok(v)
}

pub fn has_admin(accounts: &Value) -> bool {
    accounts
        .as_object()
        .unwrap()
        .values()
        .any(|a| flag(a, "admin", false))
}
pub fn remember_admin(path: &Path, accounts: &Value) -> Result<()> {
    if let Some(a) = accounts
        .as_object()
        .unwrap()
        .values()
        .find(|a| flag(a, "admin", false))
    {
        write(
            &path.parent().unwrap().join("administrator-identity.v1"),
            &json!({"id":a["id"]}),
        )?;
    }
    Ok(())
}
pub fn recovery_id(path: &Path, accounts: &Value) -> Result<String> {
    let marker = path.parent().unwrap().join("administrator-identity.v1");
    let mut candidate = String::new();
    if marker.exists() {
        candidate = text(&read(&marker, json!({}))?, "id").into();
    } else {
        let backup = path.with_file_name("accounts.v1.legacy");
        if backup.exists() {
            if fs::symlink_metadata(&backup)?.file_type().is_symlink() {
                return Err(Error::bad("account backup is a symlink"));
            }
            let source = fs::read_to_string(backup)?;
            if source.trim_start().starts_with('{') {
                let old: Value = serde_json::from_str(&source)
                    .map_err(|_| Error::bad("invalid account backup"))?;
                if let Some(a) = old
                    .as_object()
                    .and_then(|rows| rows.values().find(|a| flag(a, "admin", false)))
                {
                    candidate = text(a, "id").into();
                }
            } else {
                let fields: Vec<_> = source.split_whitespace().collect();
                if fields.len() >= 2 && fields[0] == "coolder-accounts-v1" && fields[1] == "210000"
                {
                    for row in fields[2..].chunks_exact(6) {
                        if row[2] == "1" {
                            candidate = row[1].into();
                            break;
                        }
                    }
                }
            }
        }
    }
    if candidate.is_empty() {
        return Ok(id());
    }
    if candidate.len() != 64
        || !candidate
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        || accounts
            .as_object()
            .unwrap()
            .values()
            .any(|a| text(a, "id") == candidate)
    {
        return Err(Error::bad(
            "invalid or occupied administrator recovery identity",
        ));
    }
    Ok(candidate)
}
