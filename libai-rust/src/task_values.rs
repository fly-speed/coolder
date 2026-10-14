//! Private bounded JSON values. Numbers retain their raw tokens; no acceptance rules here.
use crate::{mcp::quote, Error};
use std::collections::BTreeMap;
#[derive(Debug)]
pub(crate) enum Value {
    Object(BTreeMap<String, Value>),
    Array(Vec<Value>),
    Text(String),
    Number(String),
    Bool(bool),
    Null,
}
fn bad() -> Error {
    Error("invalid/ambiguous task JSON or resource limit".into())
}
impl Value {
    pub(crate) fn object(&self) -> Result<&BTreeMap<String, Value>, Error> {
        if let Self::Object(v) = self {
            Ok(v)
        } else {
            Err(bad())
        }
    }
    pub(crate) fn array(&self) -> Result<&[Value], Error> {
        if let Self::Array(v) = self {
            Ok(v)
        } else {
            Err(bad())
        }
    }
    pub(crate) fn text(&self) -> Result<&str, Error> {
        if let Self::Text(v) = self {
            Ok(v)
        } else {
            Err(bad())
        }
    }
    pub(crate) fn boolean(&self) -> Result<bool, Error> {
        if let Self::Bool(v) = self {
            Ok(*v)
        } else {
            Err(bad())
        }
    }
    pub(crate) fn member(&self, key: &str) -> Result<&Value, Error> {
        self.object()?.get(key).ok_or_else(bad)
    }
    pub(crate) fn optional_text(&self, key: &str, default: &str) -> Result<String, Error> {
        self.object()?
            .get(key)
            .map_or_else(|| Ok(default.to_owned()), |v| v.text().map(str::to_owned))
    }
    pub(crate) fn optional_bool(&self, key: &str, default: bool) -> Result<bool, Error> {
        self.object()?.get(key).map_or(Ok(default), Value::boolean)
    }
    pub(crate) fn json(&self) -> String {
        match self {
            Self::Object(v) => format!(
                "{{{}}}",
                v.iter()
                    .map(|(k, v)| format!("{}:{}", quote(k), v.json()))
                    .collect::<Vec<_>>()
                    .join(",")
            ),
            Self::Array(v) => format!(
                "[{}]",
                v.iter().map(Value::json).collect::<Vec<_>>().join(",")
            ),
            Self::Text(v) => quote(v),
            Self::Number(v) => v.clone(),
            Self::Bool(v) => v.to_string(),
            Self::Null => "null".into(),
        }
    }
}
pub(crate) fn parse(wire: &str, limit: usize) -> Result<Value, Error> {
    if wire.len() > limit || wire.contains('\0') {
        return Err(bad());
    }
    let mut p = Parser {
        wire,
        at: 0,
        nodes: 0,
    };
    let value = p.value(0)?;
    p.space();
    if p.at != wire.len() {
        return Err(bad());
    }
    Ok(value)
}
struct Parser<'a> {
    wire: &'a str,
    at: usize,
    nodes: usize,
}
impl Parser<'_> {
    fn space(&mut self) {
        while self.at < self.wire.len() && b" \r\n\t".contains(&self.wire.as_bytes()[self.at]) {
            self.at += 1;
        }
    }
    fn take(&mut self) -> Result<u8, Error> {
        let b = self.wire.as_bytes().get(self.at).copied().ok_or_else(bad)?;
        self.at += 1;
        Ok(b)
    }
    fn expected(&mut self, b: u8) -> Result<(), Error> {
        self.space();
        if self.take()? == b {
            Ok(())
        } else {
            Err(bad())
        }
    }
    fn value(&mut self, depth: usize) -> Result<Value, Error> {
        self.space();
        self.nodes += 1;
        if depth > 64 || self.nodes > 65536 {
            return Err(bad());
        }
        let b = self.wire.as_bytes().get(self.at).copied().ok_or_else(bad)?;
        if depth == 64 && (b == b'{' || b == b'[') {
            return Err(bad());
        }
        match b {
            b'"' => return Ok(Value::Text(self.string()?)),
            b'{' => {
                self.at += 1;
                let mut out = BTreeMap::new();
                self.space();
                if self.wire.as_bytes().get(self.at) == Some(&b'}') {
                    self.at += 1;
                    return Ok(Value::Object(out));
                }
                loop {
                    let key = self.string()?;
                    if out.contains_key(&key) {
                        return Err(bad());
                    }
                    self.expected(b':')?;
                    out.insert(key, self.value(depth + 1)?);
                    self.space();
                    match self.take()? {
                        b'}' => return Ok(Value::Object(out)),
                        b',' => (),
                        _ => return Err(bad()),
                    }
                }
            }
            b'[' => {
                self.at += 1;
                let mut out = Vec::new();
                self.space();
                if self.wire.as_bytes().get(self.at) == Some(&b']') {
                    self.at += 1;
                    return Ok(Value::Array(out));
                }
                loop {
                    out.push(self.value(depth + 1)?);
                    self.space();
                    match self.take()? {
                        b']' => return Ok(Value::Array(out)),
                        b',' => (),
                        _ => return Err(bad()),
                    }
                }
            }
            _ => (),
        }
        for (word, value) in [
            ("true", Value::Bool(true)),
            ("false", Value::Bool(false)),
            ("null", Value::Null),
        ] {
            if self.wire[self.at..].starts_with(word) {
                self.at += word.len();
                return Ok(value);
            }
        }
        let start = self.at;
        if b == b'-' {
            self.at += 1;
        }
        if self.wire.as_bytes().get(self.at) == Some(&b'0') {
            self.at += 1;
        } else {
            self.digits()?;
        }
        if self.wire.as_bytes().get(self.at) == Some(&b'.') {
            self.at += 1;
            self.digits()?;
        }
        if matches!(self.wire.as_bytes().get(self.at), Some(b'e' | b'E')) {
            self.at += 1;
            if matches!(self.wire.as_bytes().get(self.at), Some(b'+' | b'-')) {
                self.at += 1;
            }
            self.digits()?;
        }
        Ok(Value::Number(self.wire[start..self.at].to_owned()))
    }
    fn digits(&mut self) -> Result<(), Error> {
        let start = self.at;
        while self
            .wire
            .as_bytes()
            .get(self.at)
            .is_some_and(u8::is_ascii_digit)
        {
            self.at += 1;
        }
        if start == self.at {
            Err(bad())
        } else {
            Ok(())
        }
    }
    fn hex(&mut self) -> Result<u32, Error> {
        let mut code = 0;
        for _ in 0..4 {
            let b = self.take()?;
            let digit = match b {
                b'0'..=b'9' => (b - b'0') as u32,
                b'a'..=b'f' => (b - b'a' + 10) as u32,
                b'A'..=b'F' => (b - b'A' + 10) as u32,
                _ => return Err(bad()),
            };
            code = code * 16 + digit;
        }
        Ok(code)
    }
    fn string(&mut self) -> Result<String, Error> {
        self.expected(b'"')?;
        let mut out = String::new();
        let mut start = self.at;
        loop {
            let at = self.at;
            let b = self.take()?;
            match b {
                b'"' => {
                    out.push_str(&self.wire[start..at]);
                    return Ok(out);
                }
                b'\\' => {
                    out.push_str(&self.wire[start..at]);
                    let escaped = self.take()?;
                    match escaped {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\x08'),
                        b'f' => out.push('\x0c'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut code = self.hex()?;
                            if (0xd800..=0xdbff).contains(&code) {
                                if self.take()? != b'\\' || self.take()? != b'u' {
                                    return Err(bad());
                                }
                                let low = self.hex()?;
                                if !(0xdc00..=0xdfff).contains(&low) {
                                    return Err(bad());
                                }
                                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                            }
                            out.push(char::from_u32(code).ok_or_else(bad)?);
                        }
                        _ => return Err(bad()),
                    }
                    start = self.at;
                }
                0..=31 => return Err(bad()),
                _ => (),
            }
        }
    }
}
