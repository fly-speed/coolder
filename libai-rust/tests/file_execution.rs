#![cfg(unix)]
use libai::{
    confirm_execution_effect, Agent, ExecutionReceipt, ExecutionStore, FileExecutionStore,
    ModelResponse, SharedExecutionStore, ToolCall, ToolResult,
};
use std::{
    cell::{Cell, RefCell},
    fs,
    io::Write,
    os::unix::fs::PermissionsExt,
    path::PathBuf,
    rc::Rc,
};
static DIRECTORY_SEQUENCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "libai-rust-file-{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            DIRECTORY_SEQUENCE.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        Self(path)
    }
    fn text(&self) -> &str {
        self.0.to_str().unwrap()
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
#[test]
fn ownership_json_and_conflicts() {
    let root = Directory::new();
    assert!(FileExecutionStore::available());
    let mut a = FileExecutionStore::plaintext(root.text()).unwrap();
    let mut b = FileExecutionStore::plaintext(root.text()).unwrap();
    assert!(a.open("task").unwrap().revision.is_empty());
    assert!(b.open("task").is_err());
    assert_eq!(a.commit("task", "", "{\"text\":\"你好🌏\"}").unwrap(), "1");
    assert!(a.release("wrong").is_err());
    assert!(b.open("task").is_err());
    drop(a); // The sole owner releases its lease before freeing the C handle.
    let snapshot = b.open("task").unwrap();
    assert_eq!(snapshot.revision, "1");
    assert_eq!(snapshot.state_json, "{\"text\":\"你好🌏\"}");
    assert!(b.commit("task", "stale", "{}").is_err());
    b.close("task");
    assert!(fs::read_to_string(root.0.join("task.state"))
        .unwrap()
        .starts_with("{\"version\":1,"));
    assert!(b.open("../invalid").is_err());
    assert!(b.open("valid").is_ok());
    b.close("valid");
}
// A fresh process avoids forking the multithreaded Rust test harness.
#[test]
fn crash_worker() {
    let Ok(root) = std::env::var("LIBAI_TEST_FILE_SCOPE") else {
        return;
    };
    let store: SharedExecutionStore =
        Rc::new(RefCell::new(FileExecutionStore::plaintext(&root).unwrap()));
    let mut agent = Agent::new(
        |_, _| ModelResponse {
            calls: vec![ToolCall {
                id: "a".into(),
                name: "write".into(),
                arguments: "{}".into(),
            }],
            ..Default::default()
        },
        "",
    )
    .unwrap();
    agent
        .add_tool("write", "", "{}", false, move |_, invocation| {
            assert_eq!(invocation.idempotency_key(), "crash.0");
            let mut output = fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(PathBuf::from(&root).join("effect"))
                .unwrap();
            output.write_all(b"x").unwrap();
            output.sync_all().unwrap();
            std::process::exit(77);
        })
        .unwrap();
    agent.set_execution_store("crash", Some(store)).unwrap();
    agent.run("input").unwrap();
    panic!("worker did not exit");
}
#[test]
fn process_exit_and_receipt_resume() {
    let root = Directory::new();
    let status = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "crash_worker", "--nocapture"])
        .env("LIBAI_TEST_FILE_SCOPE", root.text())
        .status()
        .unwrap();
    assert_eq!(status.code(), Some(77));
    let store: SharedExecutionStore = Rc::new(RefCell::new(
        FileExecutionStore::plaintext(root.text()).unwrap(),
    ));
    let requests = Rc::new(Cell::new(0));
    let run = || {
        let counter = requests.clone();
        let mut agent = Agent::new(
            move |_, _| {
                counter.set(counter.get() + 1);
                ModelResponse {
                    text: "done".into(),
                    ..Default::default()
                }
            },
            "",
        )
        .unwrap();
        agent
            .add_tool("write", "", "{}", false, |_, _| panic!("duplicate effect"))
            .unwrap();
        agent
            .set_execution_store("crash", Some(store.clone()))
            .unwrap();
        agent.run("input").unwrap()
    };
    assert!(run().contains("recovery_required"));
    assert_eq!(requests.get(), 0);
    let snapshot = store.borrow_mut().open("crash").unwrap();
    store.borrow_mut().close("crash");
    confirm_execution_effect(
        store.clone(),
        ExecutionReceipt {
            scope: "crash",
            input: "input",
            instructions: "",
            expected_revision: &snapshot.revision,
            key: "crash.0",
            output: &ToolResult {
                text: "written".into(),
                error: false,
            },
            outcome: "applied",
            reference: "effect/one-byte",
        },
    )
    .unwrap();
    assert!(run().contains("completed"));
    assert!(run().contains("done"));
    assert_eq!(requests.get(), 1);
    assert_eq!(fs::read(root.0.join("effect")).unwrap(), b"x");
}
