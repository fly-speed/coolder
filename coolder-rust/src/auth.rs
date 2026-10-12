//! Account, session, preference and login policies are implemented in Rust.
use crate::{
    app::{Error, Result},
    store::*,
};
use serde_json::{json, Value};
use std::collections::HashMap;
pub const COOKIE: &str = "coolder_rust_session";
pub fn current(s: &State, h: &HashMap<String, String>) -> Option<Value> {
    for part in h.get("cookie").map(String::as_str).unwrap_or("").split(';') {
        let Some((k, v)) = part.trim().split_once('=') else {
            continue;
        };
        if k != COOKIE {
            continue;
        }
        let (name, expires) = s.tokens.get(v)?;
        if now() > *expires {
            return None;
        }
        let a = s.accounts.get(name)?;
        return if flag(a, "enabled", true) {
            Some(a.clone())
        } else {
            None
        };
    }
    None
}
pub fn admin(a: &Value) -> Result<()> {
    if !flag(a, "admin", false) {
        return Err(Error::new(403, "administrator required"));
    }
    Ok(())
}
fn validate(name: &str, password: &str) -> Result<()> {
    if name.is_empty()
        || name.len() > 32
        || !name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-')
        || password.len() < 8
        || password.len() > 1024
    {
        return Err(Error::bad("invalid username or password"));
    }
    Ok(())
}
fn session(s: &mut State, a: &Value, out: &mut HashMap<String, String>) -> Value {
    let token = id();
    s.tokens
        .insert(token.clone(), (text(a, "username").into(), now() + 86400));
    out.insert(
        "Set-Cookie".into(),
        format!("{COOKIE}={token}; Path=/; HttpOnly; SameSite=Strict"),
    );
    json!({"ok":true,"username":a["username"],"admin":a["admin"],"authenticated":true,"initialized":true})
}
fn invalidate(s: &mut State, name: &str) {
    s.tokens.retain(|_, (n, _)| n != name);
}
pub fn route(
    s: &mut State,
    op: &str,
    method: &str,
    b: &Value,
    h: &HashMap<String, String>,
    out: &mut HashMap<String, String>,
) -> Result<Value> {
    let actor = current(s, h);
    let name = text(b, "username");
    let pass = text(b, "password");
    if op == "status" {
        return Ok(
            json!({"initialized":!s.accounts.as_object().unwrap().is_empty(),"authenticated":actor.is_some(),"username":actor.as_ref().map(|a|text(a,"username")).unwrap_or(""),"admin":actor.as_ref().map(|a|flag(a,"admin",false)).unwrap_or(false),"ai_agent_allowed":actor.as_ref().map(|a|flag(&s.policy,"enabled",true) && flag(&s.policy,if flag(a,"admin",false) {"allow_admin"} else {"allow_users"},true)).unwrap_or(false)}),
        );
    }
    if op == "register" && method == "POST" {
        if !s.accounts.as_object().unwrap().is_empty() {
            return Err(Error::new(409, "already initialized"));
        }
        validate(name, pass)?;
        let salt = id();
        let a = json!({"username":name,"id":id(),"admin":true,"enabled":true,"salt":salt,"digest":password(pass,&salt)});
        s.accounts[name] = a.clone();
        write(&s.data.join("auth/accounts.v1"), &s.accounts)?;
        s.root(&a)?;
        return Ok(session(s, &a, out));
    }
    if op == "login" && method == "POST" {
        let attempt = s.attempts.entry(name.into()).or_insert((0, now()));
        if now() - attempt.1 > 60 {
            *attempt = (0, now());
        }
        if attempt.0 >= 10 {
            return Err(Error::new(429, "too many attempts"));
        }
        let a = s.accounts.get(name).cloned();
        if a.is_none()
            || !flag(a.as_ref().unwrap(), "enabled", true)
            || !equal(
                text(a.as_ref().unwrap(), "digest"),
                &password(pass, text(a.as_ref().unwrap(), "salt")),
            )
        {
            attempt.0 += 1;
            return Err(Error::new(401, "invalid credentials"));
        }
        s.attempts.remove(name);
        return Ok(session(s, &a.unwrap(), out));
    }
    let a = actor.ok_or_else(|| Error::new(401, "authentication required"))?;
    if op == "logout" && method == "POST" {
        for part in h.get("cookie").map(String::as_str).unwrap_or("").split(';') {
            if let Some((k, v)) = part.trim().split_once('=') {
                if k == COOKIE {
                    s.tokens.remove(v);
                }
            }
        }
        out.insert(
            "Set-Cookie".into(),
            format!("{COOKIE}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"),
        );
        return Ok(json!({"ok":true}));
    }
    if op == "preferences" {
        let file = s.data.join(format!("auth/ui-{}.v1", text(&a, "id")));
        let mut p = crate::accounts::preferences(&file)?;
        if method == "POST" {
            for k in ["language", "theme", "font_size"] {
                if let Some(v) = b.get(k) {
                    let value = v.as_str().ok_or_else(|| Error::bad("invalid preference"))?;
                    let valid = match k {
                        "language" => {
                            value.len() <= 24
                                && value
                                    .bytes()
                                    .all(|c| c.is_ascii_alphanumeric() || c == b'-')
                                && value != "template"
                                && value != "manifest"
                                && s.html.join(format!("i18n/{value}.json")).is_file()
                        }
                        "theme" => [
                            "default", "blue", "green", "purple", "sand", "gray", "pink", "ocean",
                        ]
                        .contains(&value),
                        _ => ["sm", "md", "lg"].contains(&value),
                    };
                    if !valid {
                        return Err(Error::bad("invalid preference"));
                    }
                    p[k] = v.clone();
                }
            }
            write(&file, &p)?;
        }
        return Ok(p);
    }
    if op == "password" && method == "POST" {
        if !equal(
            text(&a, "digest"),
            &password(text(b, "current_password"), text(&a, "salt")),
        ) {
            return Err(Error::new(403, "wrong password"));
        }
        validate(text(&a, "username"), pass)?;
        let mut updated = a.clone();
        let salt = id();
        updated["salt"] = json!(salt);
        updated["digest"] = json!(password(pass, &salt));
        s.accounts[text(&a, "username")] = updated.clone();
        write(&s.data.join("auth/accounts.v1"), &s.accounts)?;
        invalidate(s, text(&a, "username"));
        return Ok(session(s, &updated, out));
    }
    admin(&a)?;
    if op == "users" && method == "GET" {
        return Ok(
            json!({"ok":true,"users":s.accounts.as_object().unwrap().values().map(|a|json!({"username":a["username"],"id":a["id"],"admin":a["admin"],"enabled":a["enabled"]})).collect::<Vec<_>>()}),
        );
    }
    if op == "users/create" && method == "POST" {
        validate(name, pass)?;
        if flag(b, "admin", false) {
            return Err(Error::bad("only bootstrap administrator allowed"));
        }
        if s.accounts.get(name).is_some() {
            return Err(Error::new(409, "account exists"));
        }
        let salt = id();
        let u = json!({"username":name,"id":id(),"admin":false,"enabled":true,"salt":salt,"digest":password(pass,&salt)});
        s.accounts[name] = u.clone();
        write(&s.data.join("auth/accounts.v1"), &s.accounts)?;
        s.root(&u)?;
        return Ok(json!({"ok":true}));
    }
    if op == "users/update" && method == "POST" {
        let mut u = s
            .accounts
            .get(name)
            .cloned()
            .ok_or_else(|| Error::new(404, "account missing"))?;
        if flag(&u, "admin", false) && !flag(b, "enabled", true) {
            return Err(Error::bad("cannot disable administrator"));
        }
        if let Some(v) = b.get("enabled") {
            if !v.is_boolean() {
                return Err(Error::bad("invalid enabled"));
            }
            u["enabled"] = v.clone();
        }
        if !pass.is_empty() {
            validate(name, pass)?;
            let salt = id();
            u["salt"] = json!(salt);
            u["digest"] = json!(password(pass, &salt));
        }
        s.accounts[name] = u;
        write(&s.data.join("auth/accounts.v1"), &s.accounts)?;
        invalidate(s, name);
        return Ok(json!({"ok":true}));
    }
    Err(Error::new(404, "auth route missing"))
}
