//! Bounded HTTP/1.1 transport. Authentication and coding routes are native Rust modules.
use crate::app::App;
use std::collections::HashSet;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::net::{Shutdown, TcpStream};
use std::time::Duration;

const MAX_BODY: usize = 24 * 1024 * 1024;
struct Request {
    method: String,
    target: String,
    headers: String,
    body: Vec<u8>,
}
fn bad(text: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, text)
}
fn line(reader: &mut impl BufRead, limit: usize) -> io::Result<String> {
    let mut bytes = Vec::new();
    reader
        .take((limit + 1) as u64)
        .read_until(b'\n', &mut bytes)?;
    if bytes.len() > limit || !bytes.ends_with(b"\r\n") {
        return Err(bad("invalid HTTP line"));
    }
    bytes.truncate(bytes.len() - 2);
    if bytes.iter().any(|c| *c < 32 && *c != b'\t' || *c == 127) {
        return Err(bad("invalid HTTP character"));
    }
    String::from_utf8(bytes).map_err(|_| bad("header must be UTF-8"))
}
fn chunked(reader: &mut impl BufRead) -> io::Result<Vec<u8>> {
    let mut body = Vec::new();
    loop {
        let header = line(reader, 128)?;
        let value = header.split(';').next().unwrap_or("");
        if value.is_empty() || !value.bytes().all(|c| c.is_ascii_hexdigit()) {
            return Err(bad("invalid chunk length"));
        }
        let size = usize::from_str_radix(value, 16).map_err(|_| bad("invalid chunk length"))?;
        if size == 0 {
            // Browser JSON requests need no trailer metadata. Bound and ignore trailers.
            let mut total = 0;
            loop {
                let trailer = line(reader, 8192)?;
                total += trailer.len() + 2;
                if total > 8192 {
                    return Err(bad("trailers exceed limit"));
                }
                if trailer.is_empty() {
                    return Ok(body);
                }
            }
        }
        if size > MAX_BODY - body.len() {
            return Err(bad("body exceeds limit"));
        }
        let begin = body.len();
        body.resize(begin + size, 0);
        reader.read_exact(&mut body[begin..])?;
        let mut end = [0; 2];
        reader.read_exact(&mut end)?;
        if end != *b"\r\n" {
            return Err(bad("invalid chunk terminator"));
        }
    }
}
fn parse(reader: &mut impl BufRead) -> io::Result<Request> {
    let first = line(reader, 8192)?;
    let parts: Vec<_> = first.split(' ').collect();
    if parts.len() != 3 || parts[2] != "HTTP/1.1" || !parts[1].starts_with('/') {
        return Err(bad("invalid request line"));
    }
    let mut headers = String::new();
    let mut names = HashSet::new();
    let mut length = None;
    let mut transfer = false;
    loop {
        let item = line(reader, 8192)?;
        if item.is_empty() {
            break;
        }
        let (name, value) = item.split_once(':').ok_or_else(|| bad("invalid header"))?;
        if name.is_empty()
            || !name
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"!#$%&'*+-.^_`|~".contains(&c))
        {
            return Err(bad("invalid header name"));
        }
        let key = name.to_ascii_lowercase();
        let value = value.trim();
        if !names.insert(key.clone()) {
            return Err(bad("duplicate header"));
        }
        match key.as_str() {
            "content-length" => {
                if value.is_empty() || !value.bytes().all(|c| c.is_ascii_digit()) {
                    return Err(bad("invalid content length"));
                }
                let size = value
                    .parse::<usize>()
                    .map_err(|_| bad("invalid content length"))?;
                if size > MAX_BODY {
                    return Err(bad("body exceeds limit"));
                }
                length = Some(size);
            }
            "transfer-encoding" => {
                if !value.eq_ignore_ascii_case("chunked") {
                    return Err(bad("unsupported transfer encoding"));
                }
                transfer = true;
            }
            "expect" => return Err(bad("expect header is unsupported")),
            _ => {}
        }
        headers.push_str(&item);
        headers.push_str("\r\n");
        if headers.len() > 65536 {
            return Err(bad("headers exceed limit"));
        }
    }
    if !names.contains("host") || transfer && length.is_some() {
        return Err(bad("invalid request framing"));
    }
    let body = if transfer {
        chunked(reader)?
    } else {
        let mut bytes = vec![0; length.unwrap_or(0)];
        reader.read_exact(&mut bytes)?;
        bytes
    };
    Ok(Request {
        method: parts[0].into(),
        target: parts[1].into(),
        headers,
        body,
    })
}
fn error(stream: &mut TcpStream, status: u16, message: &str) -> io::Result<()> {
    let body = format!("{{\"error\":\"{message}\"}}");
    write!(stream,"HTTP/1.1 {status} Error\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: {}\r\n\r\n{body}",body.len())
}
/// Serve one connection and close after its response, including SSE streams.
pub fn serve(app: &App, mut stream: TcpStream) -> io::Result<()> {
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(Duration::from_secs(30)))?;
    stream.set_write_timeout(Some(Duration::from_secs(5)))?;
    let request = match parse(&mut BufReader::new(stream.try_clone()?)) {
        Ok(request) => request,
        Err(_) => {
            error(&mut stream, 400, "invalid request")?;
            return Ok(());
        }
    };
    if request.method != "GET" && request.method != "POST" {
        error(&mut stream, 405, "method not allowed")?;
        return Ok(());
    }
    let mut started = false;
    let result = app.dispatch(
        &request.method,
        &request.target,
        &request.headers,
        &request.body,
        |status, headers, bytes| {
            if !started {
                started = true;
                if write!(
                    stream,
                    "HTTP/1.1 {status} Response\r\n{headers}Connection: close\r\n\r\n"
                )
                .is_err()
                {
                    return false;
                }
            }
            stream.write_all(bytes).is_ok()
        },
    );
    if result.is_err() && !started {
        error(&mut stream, 503, "application unavailable")?;
    }
    let _ = stream.shutdown(Shutdown::Both);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_ambiguous_framing() {
        for headers in [
            "Content-Length: 1\r\nContent-Length: 1\r\n",
            "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n",
            "Content-Length: -1\r\n",
            "Host: evil\r\n",
            "X-Test : value\r\n",
        ] {
            let request = format!("POST / HTTP/1.1\r\nHost: 127.0.0.1:1\r\n{headers}\r\nx");
            assert!(parse(&mut io::Cursor::new(request)).is_err());
        }
    }
    #[test]
    fn preserves_chunked_binary_body() {
        let request=b"POST /x?q=%E4%BD%A0 HTTP/1.1\r\nHost: 127.0.0.1:1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\na\0b\r\n0\r\n\r\n";
        assert_eq!(parse(&mut io::Cursor::new(request)).unwrap().body, b"a\0b");
    }
}
