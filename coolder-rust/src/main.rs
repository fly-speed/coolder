//! Independent Rust listener and CLI, using only the public libai-rust API.
mod accounts;
mod app;
mod auth;
mod http;
mod planning;
mod projects;
mod runs;
mod sandbox;
mod store;
mod validation;
mod workspace;
use app::App;
use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::{self, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicBool, AtomicUsize, Ordering},
    Arc, Mutex,
};
use std::thread;
use std::time::Duration;

static STOP: AtomicBool = AtomicBool::new(false);
#[cfg(unix)]
unsafe extern "C" fn stop(_: i32) {
    STOP.store(true, Ordering::Relaxed);
}
#[cfg(unix)]
fn signals() {
    extern "C" {
        fn signal(number: i32, handler: usize) -> usize;
    }
    unsafe {
        signal(2, stop as *const () as usize);
        signal(15, stop as *const () as usize);
        signal(13, 1);
    }
}
#[cfg(windows)]
fn signals() {
    unsafe extern "system" fn handler(event: u32) -> i32 {
        if matches!(event, 0 | 1 | 2 | 5 | 6) {
            STOP.store(true, Ordering::Relaxed);
            1
        } else {
            0
        }
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn SetConsoleCtrlHandler(handler: unsafe extern "system" fn(u32) -> i32, add: i32) -> i32;
    }
    unsafe {
        SetConsoleCtrlHandler(handler, 1);
    }
}
#[cfg(not(any(unix, windows)))]
fn signals() {}
fn quote(text: &str) -> String {
    let mut result = String::from("\"");
    for c in text.chars() {
        match c {
            '"' | '\\' => {
                result.push('\\');
                result.push(c);
            }
            c if c < ' ' => result.push_str(&format!("\\u{:04x}", c as u32)),
            _ => result.push(c),
        }
    }
    result.push('"');
    result
}
struct Options {
    port: u16,
    data: String,
    workspace: String,
    html: String,
    log: String,
    help: bool,
}
impl Options {
    fn parse() -> Result<Self, String> {
        let executable = std::env::current_exe().map_err(|e| e.to_string())?;
        let html_candidate = executable
            .parent()
            .and_then(|p| p.parent())
            .and_then(|p| p.parent())
            .unwrap_or(std::path::Path::new("."))
            .join("html");
        let html = if html_candidate.join("index.html").is_file() {
            html_candidate
        } else {
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("html")
        };
        let mut value = Self {
            port: 18097,
            data: "var-rust".into(),
            workspace: String::new(),
            html: html.to_string_lossy().into(),
            log: "coolder-rust.log".into(),
            help: false,
        };
        let mut args = std::env::args().skip(1);
        while let Some(key) = args.next() {
            if key == "--help" {
                value.help = true;
                return Ok(value);
            }
            let argument = args
                .next()
                .ok_or_else(|| format!("option requires a value: {key}"))?;
            match key.as_str() {
                "--port" => value.port = argument.parse().map_err(|_| "invalid port")?,
                "--data" => value.data = argument,
                "--workspace" => value.workspace = argument,
                "--html" => value.html = argument,
                "--log-file" => value.log = argument,
                _ => return Err(format!("unknown option: {key}")),
            }
        }
        if value.port == 0 {
            return Err("invalid port".into());
        }
        Ok(value)
    }
    fn configuration(&self) -> String {
        let mut result = format!(
            "{{\"app\":\"coolder-rust\",\"cookie_name\":\"coolder_rust_session\",\"data\":{},\"html\":{},\"authority\":{},\"log_file\":{}",
            quote(&self.data),
            quote(&self.html),
            quote(&format!("127.0.0.1:{}", self.port)),
            quote(&self.log)
        );
        if !self.workspace.is_empty() {
            result.push_str(&format!(",\"workspace\":{}", quote(&self.workspace)));
        }
        let helper = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|p| p.join("webcool-sandbox-helper")));
        if let Some(helper) = helper {
            result.push_str(&format!(
                ",\"sandbox_helper\":{}",
                quote(&helper.to_string_lossy())
            ));
        }
        result.push('}');
        result
    }
}
fn run(mut options: Options) -> Result<(), Box<dyn std::error::Error>> {
    if options.log.is_empty() || options.log.contains('|') {
        return Err("invalid log file path".into());
    }
    options.log = std::env::current_dir()?
        .join(PathBuf::from(options.log))
        .to_string_lossy()
        .into();
    let file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&options.log)
        .map_err(|error| {
            io::Error::new(
                error.kind(),
                format!("cannot open log file: {}", options.log),
            )
        })?;
    #[cfg(unix)]
    {
        use std::os::fd::AsRawFd;
        extern "C" {
            fn dup2(old: i32, new: i32) -> i32;
        }
        if unsafe { dup2(file.as_raw_fd(), 1) } < 0 || unsafe { dup2(file.as_raw_fd(), 2) } < 0 {
            return Err(io::Error::last_os_error().into());
        }
    }
    let log = Arc::new(Mutex::new(file));
    let listener = TcpListener::bind(("127.0.0.1", options.port))?;
    listener.set_nonblocking(true)?;
    writeln!(log.lock().unwrap(), "Logging to {}", options.log)?;
    let app = Arc::new(App::new(&options.configuration()).map_err(|error| {
        let _ = writeln!(log.lock().unwrap(), "coolder-rust: {error}");
        error
    })?);
    signals();
    writeln!(
        log.lock().unwrap(),
        "coolder-rust: http://127.0.0.1:{}",
        options.port
    )?;
    let active = Arc::new(AtomicUsize::new(0));
    let mut workers = Vec::new();
    let connections: Arc<Mutex<HashMap<usize, TcpStream>>> = Arc::new(Mutex::new(HashMap::new()));
    let mut connection_id = 0;
    let workspace = if options.workspace.is_empty() {
        format!("{}/workspace", options.data)
    } else {
        options.workspace.clone()
    };
    writeln!(
        log.lock().unwrap(),
        "Workspace: {}",
        std::env::current_dir()?.join(workspace).display()
    )?;
    while !STOP.load(Ordering::Relaxed) {
        match listener.accept() {
            Ok((stream, _)) => {
                if active.load(Ordering::Relaxed) >= 128 {
                    drop(stream);
                    continue;
                }
                connection_id += 1;
                let id = connection_id;
                connections.lock().unwrap().insert(id, stream.try_clone()?);
                let connections = connections.clone();
                active.fetch_add(1, Ordering::Relaxed);
                let app = app.clone();
                let active = active.clone();
                let log = log.clone();
                workers.push(thread::spawn(move || {
                    if let Err(error) = http::serve(&app, stream) {
                        let _ = writeln!(log.lock().unwrap(), "HTTP transport: {}", error.kind());
                    }
                    connections.lock().unwrap().remove(&id);
                    active.fetch_sub(1, Ordering::Relaxed);
                }));
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(10))
            }
            Err(error) => {
                app.stop();
                return Err(error.into());
            }
        }
        let mut index = 0;
        while index < workers.len() {
            if workers[index].is_finished() {
                let _ = workers.swap_remove(index).join();
            } else {
                index += 1;
            }
        }
    }
    app.stop();
    for stream in connections.lock().unwrap().values() {
        let _ = stream.shutdown(Shutdown::Both);
    }
    for worker in workers {
        let _ = worker.join();
    }
    Ok(())
}
fn main() {
    let options = match Options::parse() {
        Ok(value) => value,
        Err(error) => {
            eprintln!("coolder-rust: {error}");
            std::process::exit(1);
        }
    };
    if options.help {
        println!("coolder-rust [--port 18097] [--data DIR] [--workspace DIR] [--html DIR] [--log-file FILE]");
        return;
    }
    if let Err(error) = run(options) {
        eprintln!("coolder-rust: {error}");
        std::process::exit(1);
    }
}
