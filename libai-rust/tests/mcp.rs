use libai::{
    Agent, Cancellation, Error, ExecutionSnapshot, ExecutionStore, Invocation, McpBinding,
    McpClient, McpOptions, McpTransport, ModelResponse, SharedExecutionStore, ToolCall,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};
#[derive(Default)]
struct State {
    initialized: Cell<bool>,
    called: Cell<usize>,
    closed: Cell<usize>,
    dropped: Cell<usize>,
    lost: Cell<bool>,
    panic: Cell<bool>,
    payload_panic: Cell<bool>,
    drop_panic: Cell<bool>,
    key: RefCell<String>,
}
struct Transport(Rc<State>);
impl Drop for Transport {
    fn drop(&mut self) {
        self.0.dropped.set(self.0.dropped.get() + 1);
        if self.0.drop_panic.get() {
            std::panic::panic_any(PanicPayload);
        }
    }
}
impl McpTransport for Transport {
    fn request(
        &mut self,
        wire: &str,
        context: &str,
        invocation: &Invocation<'_>,
    ) -> Result<String, Error> {
        assert!(!invocation.stopped());
        let id = context
            .split("\"request_id\":")
            .nth(1)
            .unwrap()
            .split(|c: char| !c.is_ascii_digit())
            .next()
            .unwrap();
        let value = if wire.contains("\"method\":\"initialize\"") {
            r#"{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"rust-fixture","version":"1"}}"#
        } else if wire.contains("\"method\":\"tools/list\"") {
            assert!(self.0.initialized.get());
            assert!(context.contains("2025-11-25"));
            r#"{"tools":[{"name":"echo","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}}]}"#
        } else {
            self.0.called.set(self.0.called.get() + 1);
            *self.0.key.borrow_mut() = invocation.idempotency_key();
            if self.0.panic.get() {
                panic!("injected transport panic");
            }
            if self.0.payload_panic.get() {
                std::panic::panic_any(PanicPayload);
            }
            if self.0.lost.get() {
                return Err(Error("lost response after effect".into()));
            }
            r#"{"content":[{"type":"text","text":"echo"}],"_meta":{"private":"secret"}}"#
        };
        Ok(format!(
            "{{\"jsonrpc\":\"2.0\",\"id\":{id},\"result\":{value}}}"
        ))
    }
    fn notify(&mut self, wire: &str, context: &str, _: &Invocation<'_>) -> Result<(), Error> {
        if wire.contains("notifications/initialized") {
            assert!(context.contains("2025-11-25"));
            self.0.initialized.set(true);
        }
        Ok(())
    }
    fn close(&mut self) {
        self.0.closed.set(self.0.closed.get() + 1);
    }
}
struct PanicPayload;
impl Drop for PanicPayload {
    fn drop(&mut self) {
        panic!("panic payload destructor failed");
    }
}
#[test]
fn panic_payload_and_transport_destructors_stay_inside_rust() {
    let s = Rc::new(State::default());
    let mut client = connect(s.clone());
    s.payload_panic.set(true);
    assert!(client
        .call_tool("echo", "{}", &Default::default())
        .unwrap_err()
        .0
        .contains("Rust MCP transport panicked"));
    drop(client);
    assert_eq!(s.closed.get(), 1);
    assert_eq!(s.dropped.get(), 1);
    let s = Rc::new(State::default());
    let client = connect(s.clone());
    s.drop_panic.set(true);
    drop(client);
    assert_eq!(s.closed.get(), 1);
    assert_eq!(s.dropped.get(), 1);
}
fn connect(s: Rc<State>) -> McpClient {
    let mut client = McpClient::new(Rc::new(RefCell::new(Transport(s)))).unwrap();
    client
        .initialize("test", "1", &McpOptions::default())
        .unwrap();
    assert!(client
        .list_tools(&McpOptions::default())
        .unwrap()
        .contains("input_schema"));
    client
}
fn policy() -> McpBinding<'static> {
    McpBinding {
        parameters_schema: r#"{"type":"object","additionalProperties":false}"#,
        execution_revision: "host-fixture-1",
        ..Default::default()
    }
}
fn agent(count: Rc<Cell<usize>>, invalid: bool) -> Agent {
    Agent::new(
        move |request, _| {
            let n = count.get() + 1;
            count.set(n);
            if n == 1 {
                assert!(request.contains("\"requires_approval\":true"));
                ModelResponse {
                    calls: vec![ToolCall {
                        id: "call1".into(),
                        name: "local_echo".into(),
                        arguments: if invalid {
                            r#"{"bad":true}"#.into()
                        } else {
                            "{}".into()
                        },
                    }],
                    ..Default::default()
                }
            } else {
                ModelResponse {
                    text: "done".into(),
                    ..Default::default()
                }
            }
        },
        "",
    )
    .unwrap()
}
#[derive(Default)]
struct Store {
    revision: String,
    state: String,
    sequence: usize,
    leased: bool,
}
impl ExecutionStore for Store {
    fn open(&mut self, _: &str) -> Result<ExecutionSnapshot, Error> {
        assert!(!self.leased);
        self.leased = true;
        Ok(ExecutionSnapshot {
            revision: self.revision.clone(),
            state_json: self.state.clone(),
        })
    }
    fn commit(&mut self, _: &str, expected: &str, state: &str) -> Result<String, Error> {
        assert!(self.leased);
        assert_eq!(expected, self.revision);
        self.sequence += 1;
        self.revision = self.sequence.to_string();
        self.state = state.into();
        Ok(self.revision.clone())
    }
    fn close(&mut self, _: &str) {
        self.leased = false;
    }
}
fn run(a: &mut Agent, input: &str) -> String {
    let mut approve = |_: &str, _: &str, required: bool| {
        assert!(required);
        true
    };
    a.run_with(
        input,
        &Default::default(),
        libai::Hooks {
            authorize: Some(&mut approve),
            ..Default::default()
        },
    )
    .unwrap()
}
#[test]
fn lifetime_governance_and_cache() {
    let s = Rc::new(State::default());
    let client = connect(s.clone());
    {
        let mut a = agent(Rc::new(Cell::new(0)), false);
        a.add_mcp_tool(&client, "echo", "local_echo", &policy())
            .unwrap();
        assert!(a.run("echo").unwrap().contains("completed"));
        assert_eq!(s.called.get(), 0);
    }
    {
        let mut a = agent(Rc::new(Cell::new(0)), true);
        a.add_mcp_tool(&client, "echo", "local_echo", &policy())
            .unwrap();
        let mut authorizations = 0;
        let mut approve = |_: &str, _: &str, _: bool| {authorizations += 1;true};
        let result = a.run_with(
            "echo",
            &Default::default(),
            libai::Hooks {
                authorize: Some(&mut approve),
                ..Default::default()
            },
        )
        .unwrap();
        assert!(result.contains("\"status\":\"completed\""));
        assert_eq!(authorizations, 0);
        assert_eq!(s.called.get(), 0);
    }
    let count = Rc::new(Cell::new(0));
    let mut a = agent(count.clone(), false);
    a.add_mcp_tool(&client, "echo", "local_echo", &policy())
        .unwrap();
    let store: SharedExecutionStore = Rc::new(RefCell::new(Store::default()));
    a.set_execution_store("rust-mcp", Some(store)).unwrap();
    drop(client);
    assert_eq!(s.closed.get(), 0);
    assert_eq!(s.dropped.get(), 0);
    assert!(run(&mut a, "echo").contains("completed"));
    assert_eq!(s.called.get(), 1);
    assert_eq!(*s.key.borrow(), "rust-mcp.0");
    assert!(run(&mut a, "echo").contains("completed"));
    assert_eq!(count.get(), 2);
    assert_eq!(s.called.get(), 1);
    assert!(!a.messages().unwrap().contains("secret"));
    drop(a);
    assert_eq!(s.closed.get(), 1);
    assert_eq!(s.dropped.get(), 1);
}
#[test]
fn unknown_effect_is_not_repeated_after_reconnect() {
    let count = Rc::new(Cell::new(0));
    let store: SharedExecutionStore = Rc::new(RefCell::new(Store::default()));
    let s = Rc::new(State::default());
    s.lost.set(true);
    {
        let client = connect(s.clone());
        let mut a = agent(count.clone(), false);
        a.add_mcp_tool(&client, "echo", "local_echo", &policy())
            .unwrap();
        a.set_execution_store("rust-unknown", Some(store.clone()))
            .unwrap();
        assert!(run(&mut a, "write").contains("recovery_required"));
        assert_eq!(s.called.get(), 1);
    }
    let next = Rc::new(State::default());
    let client = connect(next.clone());
    let mut a = agent(count.clone(), false);
    a.add_mcp_tool(&client, "echo", "local_echo", &policy())
        .unwrap();
    a.set_execution_store("rust-unknown", Some(store)).unwrap();
    assert!(run(&mut a, "write").contains("recovery_required"));
    assert_eq!(next.called.get(), 0);
    assert_eq!(count.get(), 1);
}
#[test]
fn panic_borrow_and_cancel_boundaries() {
    let s = Rc::new(State::default());
    let mut client = connect(s.clone());
    s.panic.set(true);
    assert!(client
        .call_tool("echo", "{}", &McpOptions::default())
        .unwrap_err()
        .0
        .contains("Rust MCP transport panicked"));
    drop(client);
    assert_eq!(s.closed.get(), 1);
    assert_eq!(s.dropped.get(), 1);
    let s = Rc::new(State::default());
    let shared = Rc::new(RefCell::new(Transport(s.clone())));
    let mut client = McpClient::new(shared.clone()).unwrap();
    let borrow = shared.borrow_mut();
    assert!(client.initialize("test", "1", &Default::default()).is_err());
    drop(borrow);
    drop(client);
    drop(shared);
    assert_eq!(s.dropped.get(), 1);
    let s = Rc::new(State::default());
    let mut client = connect(s.clone());
    let cancellation = Cancellation::new().unwrap();
    cancellation.cancel();
    assert!(client
        .call_tool(
            "echo",
            "{}",
            &McpOptions {
                cancellation: Some(&cancellation),
                ..Default::default()
            }
        )
        .is_err());
    assert_eq!(s.called.get(), 0);
    let options = McpOptions {
        timeout_ms: u64::MAX,
        ..Default::default()
    };
    assert!(client.list_tools(&options).is_err());
}
#[cfg(all(unix, not(target_os = "ios"), not(target_os = "android")))]
#[test]
fn stdio_real_service() {
    use std::process::Command;
    let python = Command::new("/usr/bin/python3")
        .args(["-c", "import sys;print(sys.executable)"])
        .output()
        .unwrap();
    assert!(python.status.success());
    let executable = String::from_utf8(python.stdout).unwrap().trim().to_string();
    let script = format!(
        "{}/../libai-c++/tests/mcp_stdio_fixture.py",
        env!("CARGO_MANIFEST_DIR")
    );
    let config = format!("{{\"executable\":\"{executable}\",\"arguments\":[\"{script}\"]}}");
    assert!(McpClient::stdio_available());
    let mut client = McpClient::stdio(&config).unwrap();
    client
        .initialize("rust-app", "1", &Default::default())
        .unwrap();
    client.list_tools(&Default::default()).unwrap();
    assert!(client
        .call_tool("echo", "{}", &Default::default())
        .unwrap()
        .contains("from stdio"));
    let mut a = agent(Rc::new(Cell::new(0)), false);
    a.add_mcp_tool(&client, "echo", "local_echo", &policy())
        .unwrap();
    assert!(run(&mut a, "echo").contains("completed"));
    client.close().unwrap();
    client.close().unwrap();
    assert!(client.list_tools(&Default::default()).is_err());
}
