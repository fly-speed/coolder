use libai::{
    Agent, Cancellation, Error, ExecutionSnapshot, ExecutionStore, FileExecutionStore, Hooks,
    ModelResponse, SharedExecutionStore, ToolCall, ToolResult, Workflow, WorkflowNodeResult,
    WorkflowNodeSpec, WorkflowOptions, WorkflowReceipt,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::{Arc, Condvar, Mutex},
    time::Duration,
};
fn output(patch: &str) -> WorkflowNodeResult {
    WorkflowNodeResult {
        patch_json: patch.into(),
        ..Default::default()
    }
}
fn status(result: &str, value: &str) {
    assert!(
        result.contains(&format!("\"status\":\"{value}\"")),
        "{result}"
    );
}
#[derive(Default)]
struct Store {
    snapshot: ExecutionSnapshot,
    sequence: usize,
    leased: bool,
    fail_result: bool,
}
impl ExecutionStore for Store {
    fn open(&mut self, _: &str) -> Result<ExecutionSnapshot, Error> {
        if self.leased {
            return Err(Error("leased".into()));
        }
        self.leased = true;
        Ok(ExecutionSnapshot {
            revision: self.snapshot.revision.clone(),
            state_json: self.snapshot.state_json.clone(),
        })
    }
    fn commit(&mut self, _: &str, expected: &str, state: &str) -> Result<String, Error> {
        assert!(self.leased);
        if expected != self.snapshot.revision {
            return Err(Error("CAS conflict".into()));
        }
        if self.fail_result && state.contains("\"phase\":\"produced\"") {
            self.fail_result = false;
            return Err(Error("lost result commit".into()));
        }
        self.sequence += 1;
        self.snapshot = ExecutionSnapshot {
            revision: self.sequence.to_string(),
            state_json: state.into(),
        };
        Ok(self.snapshot.revision.clone())
    }
    fn close(&mut self, _: &str) {
        self.leased = false;
    }
}
#[test]
fn parallel_branch_join_and_local_boundary() {
    let shared = Arc::new((Mutex::new(0), Condvar::new()));
    let snapshots = Arc::new(Mutex::new(Vec::new()));
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("choose"), |_, _| {
            Ok(WorkflowNodeResult {
                route: "yes".into(),
                ..Default::default()
            })
        })
        .unwrap();
    for key in ["a", "b"] {
        let gate = shared.clone();
        let snapshots = snapshots.clone();
        let writes = [key];
        let mut spec = WorkflowNodeSpec::new(key);
        spec.writes = &writes;
        graph
            .add_parallel_node(spec, move |state, context| {
                assert_eq!(context.execution_scope, format!("wf3_job_{key}"));
                assert_eq!(context.invocation.idempotency_key(), "");
                snapshots.lock().unwrap().push(state.to_string());
                let (lock, changed) = &*gate;
                let mut count = lock.lock().unwrap();
                *count += 1;
                changed.notify_all();
                let (count, timeout) = changed
                    .wait_timeout_while(count, Duration::from_secs(3), |count| *count < 2)
                    .unwrap();
                assert!(!timeout.timed_out() && *count == 2);
                Ok(output(&format!("{{\"{key}\":\"🌏\"}}")))
            })
            .unwrap();
        graph.add_edge("choose", key, "yes").unwrap();
    }
    let skipped = Rc::new(Cell::new(0));
    let counter = skipped.clone();
    graph
        .add_node(WorkflowNodeSpec::new("no"), move |_, _| {
            counter.set(1);
            Ok(Default::default())
        })
        .unwrap();
    graph.add_edge("choose", "no", "no").unwrap();
    let joins = Rc::new(Cell::new(0));
    let counter = joins.clone();
    let caller = std::thread::current().id();
    graph
        .add_node(WorkflowNodeSpec::new("join"), move |state, _| {
            assert_eq!(std::thread::current().id(), caller);
            assert!(state.contains("\"a\"") && state.contains("\"b\""));
            counter.set(counter.get() + 1);
            Ok(Default::default())
        })
        .unwrap();
    for key in ["a", "b", "no"] {
        graph.add_edge(key, "join", "").unwrap();
    }
    let options = WorkflowOptions {
        scope: "job",
        max_parallel: 2,
        ..Default::default()
    };
    let result = graph.run("{}", &options).unwrap();
    status(&result, "completed");
    assert!(
        result.contains("\"accepted\":false")
            && result.contains("\"no\":\"skipped\"")
            && result.contains("\"node_calls\":4")
    );
    assert_eq!(joins.get(), 1);
    assert_eq!(skipped.get(), 0);
    assert_eq!(*snapshots.lock().unwrap(), vec!["{}", "{}"]);
    let mut local = Workflow::new().unwrap();
    let calls = Rc::new(Cell::new(0));
    for key in ["a", "b"] {
        let count = calls.clone();
        local
            .add_node(WorkflowNodeSpec::new(key), move |_, _| {
                count.set(count.get() + 1);
                Ok(Default::default())
            })
            .unwrap();
    }
    status(&local.run("{}", &options).unwrap(), "protocol_error");
    assert_eq!(calls.get(), 0);
}
#[test]
fn registration_ownership_output_and_panic_boundaries() {
    struct Guard(Rc<Cell<usize>>);
    impl Drop for Guard {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    let dropped = Rc::new(Cell::new(0));
    let mut graph = Workflow::new().unwrap();
    let guard = Guard(dropped.clone());
    graph
        .add_node(WorkflowNodeSpec::new("a"), move |_, _| {
            let _ = &guard;
            Ok(Default::default())
        })
        .unwrap();
    assert_eq!(dropped.get(), 0);
    let guard = Guard(dropped.clone());
    assert!(graph
        .add_node(WorkflowNodeSpec::new("a"), move |_, _| {
            let _ = &guard;
            Ok(Default::default())
        })
        .is_err());
    assert_eq!(dropped.get(), 1);
    graph.close().unwrap();
    assert_eq!(dropped.get(), 2);
    assert!(graph.run("{}", &Default::default()).is_err());
    graph.close().unwrap();
    struct Payload;
    impl Drop for Payload {
        fn drop(&mut self) {
            panic!("panic payload drop");
        }
    }
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("panic"), |_, _| {
            std::panic::panic_any(Payload)
        })
        .unwrap();
    status(
        &graph.run("{}", &Default::default()).unwrap(),
        "callback_error",
    );
    for patch in ["{bad}", "{\"x\":1,\"x\":2}", "{\"undeclared\":1}"] {
        let mut graph = Workflow::new().unwrap();
        graph
            .add_node(WorkflowNodeSpec::new("a"), move |_, _| Ok(output(patch)))
            .unwrap();
        status(
            &graph.run("{}", &Default::default()).unwrap(),
            "callback_error",
        );
    }
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("a"), |_, _| {
            Ok(WorkflowNodeResult {
                status: "bad".into(),
                ..Default::default()
            })
        })
        .unwrap();
    status(
        &graph.run("{}", &Default::default()).unwrap(),
        "callback_error",
    );
    struct PanicDrop;
    impl Drop for PanicDrop {
        fn drop(&mut self) {
            std::panic::panic_any(Payload);
        }
    }
    let mut graph = Workflow::new().unwrap();
    let guard = PanicDrop;
    graph
        .add_node(WorkflowNodeSpec::new("a"), move |_, _| {
            let _ = &guard;
            Ok(Default::default())
        })
        .unwrap();
    graph.close().unwrap();
}
#[test]
fn durable_receipts_cache_and_storage_failure() {
    let store = Rc::new(RefCell::new(Store::default()));
    let effects = Rc::new(Cell::new(0));
    let count = effects.clone();
    let mut graph = Workflow::new().unwrap();
    let mut spec = WorkflowNodeSpec::new("write");
    spec.writes = &["answer"];
    graph
        .add_node(spec, move |_, _| {
            count.set(count.get() + 1);
            Err(Error("lost acknowledgement after effect".into()))
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        execution_store: Some(store.clone()),
        ..Default::default()
    };
    status(&graph.run("{}", &options).unwrap(), "recovery_required");
    status(&graph.run("{}", &options).unwrap(), "recovery_required");
    assert_eq!(effects.get(), 1);
    let revision = store.borrow().snapshot.revision.clone();
    fn receipt<'a>(revision: &'a str, node_output: &'a WorkflowNodeResult) -> WorkflowReceipt<'a> {
        WorkflowReceipt {
            scope: "job",
            initial_state_json: "{}",
            expected_revision: revision,
            node: "write",
            outcome: "applied",
            output: node_output,
            reference: "verified-receipt",
        }
    }
    assert!(graph
        .confirm_node(store.clone(), receipt("stale", &Default::default()))
        .is_err());
    assert!(graph
        .confirm_node(
            store.clone(),
            receipt(&revision, &output("{\"outside\":1}"))
        )
        .is_err());
    graph
        .confirm_node(
            store.clone(),
            receipt(&revision, &output("{\"answer\":42}")),
        )
        .unwrap();
    for _ in 0..2 {
        status(&graph.run("{}", &options).unwrap(), "completed");
    }
    assert_eq!(effects.get(), 1);
    assert!(graph
        .run("{\"changed\":true}", &options)
        .unwrap()
        .contains("\"progress_available\":false"));
    assert!(!store.borrow().leased);
    let store = Rc::new(RefCell::new(Store {
        fail_result: true,
        ..Default::default()
    }));
    let count = effects.clone();
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("write"), move |_, _| {
            count.set(count.get() + 1);
            Ok(Default::default())
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        execution_store: Some(store.clone()),
        ..Default::default()
    };
    let result = graph.run("{}", &options).unwrap();
    status(&result, "recovery_required");
    assert!(result.contains("\"progress_available\":false"));
    status(&graph.run("{}", &options).unwrap(), "recovery_required");
    assert_eq!(effects.get(), 2);
    assert!(!store.borrow().leased);
}
#[test]
fn store_panic_payload_fails_closed() {
    struct Payload;
    impl Drop for Payload {
        fn drop(&mut self) {
            panic!("payload destructor failed");
        }
    }
    struct Broken;
    impl ExecutionStore for Broken {
        fn open(&mut self, _: &str) -> Result<ExecutionSnapshot, Error> {
            std::panic::panic_any(Payload)
        }
        fn commit(&mut self, _: &str, _: &str, _: &str) -> Result<String, Error> {
            panic!("commit after failed open")
        }
        fn close(&mut self, _: &str) {
            panic!("close an unacquired lease")
        }
    }
    let store: SharedExecutionStore = Rc::new(RefCell::new(Broken));
    let count = Rc::new(Cell::new(0));
    let effects = count.clone();
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("write"), move |_, _| {
            effects.set(1);
            Ok(Default::default())
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        execution_store: Some(store.clone()),
        ..Default::default()
    };
    let result = graph.run("{}", &options).unwrap();
    status(&result, "recovery_required");
    assert!(result.contains("\"progress_available\":false"));
    assert_eq!(count.get(), 0);
    let models = count.clone();
    let mut agent = Agent::new(
        move |_, _| {
            models.set(1);
            ModelResponse {
                text: "done".into(),
                ..Default::default()
            }
        },
        "",
    )
    .unwrap();
    agent.set_execution_store("job", Some(store)).unwrap();
    status(&agent.run("input").unwrap(), "recovery_required");
    assert_eq!(count.get(), 0);
}
#[test]
fn child_agent_deadline_and_shared_cancellation() {
    let child_store = Rc::new(RefCell::new(Store::default()));
    let models = Rc::new(Cell::new(0));
    let effects = Rc::new(Cell::new(0));
    let mut graph = Workflow::new().unwrap();
    let counts = models.clone();
    let changes = effects.clone();
    let mut spec = WorkflowNodeSpec::new("agent");
    spec.writes = &["answer"];
    graph
        .add_node(spec, move |state, context| {
            assert_eq!(context.execution_scope, "wf3_job_agent");
            let count = counts.clone();
            let mut agent = Agent::new(
                move |_, _| {
                    count.set(count.get() + 1);
                    if count.get() == 1 {
                        ModelResponse {
                            calls: vec![ToolCall {
                                id: "one".into(),
                                name: "write".into(),
                                arguments: "{}".into(),
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
            )?;
            let changes = changes.clone();
            agent.add_tool("write", "", "{}", false, move |_, invocation| {
                assert_eq!(invocation.idempotency_key(), "wf3_job_agent.0");
                changes.set(changes.get() + 1);
                ToolResult {
                    text: "3".into(),
                    error: false,
                }
            })?;
            agent.set_execution_store(&context.execution_scope, Some(child_store.clone()))?;
            let (limits, token) = context.agent_options()?;
            let result =
                agent.run_with_cancellation(state, &limits, Hooks::default(), Some(&token))?;
            Ok(if result.contains("\"status\":\"completed\"") {
                output("{\"answer\":3}")
            } else {
                WorkflowNodeResult {
                    status: "recovery_required".into(),
                    ..Default::default()
                }
            })
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        ..Default::default()
    };
    for _ in 0..2 {
        status(&graph.run("{}", &options).unwrap(), "completed");
    }
    assert_eq!(models.get(), 2);
    assert_eq!(effects.get(), 1);
    status(
        &graph.run("{\"changed\":true}", &options).unwrap(),
        "recovery_required",
    );
    let token = Arc::new(Cancellation::new().unwrap());
    let stop = token.clone();
    let (send, receive) = std::sync::mpsc::channel();
    let worker = std::thread::spawn(move || {
        receive.recv_timeout(Duration::from_secs(3)).unwrap();
        stop.cancel();
    });
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("wait"), move |_, context| {
            let retained = context.invocation.retain_cancellation()?;
            send.send(()).unwrap();
            while !context.stopped() {
                std::thread::yield_now();
            }
            assert!(retained.cancelled());
            assert!(context.agent_options().is_err());
            Ok(Default::default())
        })
        .unwrap();
    let options = WorkflowOptions {
        cancellation: Some(&token),
        ..Default::default()
    };
    status(&graph.run("{}", &options).unwrap(), "cancelled");
    worker.join().unwrap();
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("wait"), |_, context| {
            while !context.stopped() {
                std::thread::yield_now();
            }
            assert!(context.agent_options().is_err());
            Ok(Default::default())
        })
        .unwrap();
    let options = WorkflowOptions {
        timeout_ms: 5,
        ..Default::default()
    };
    status(&graph.run("{}", &options).unwrap(), "deadline_exceeded");
}
#[cfg(unix)]
#[test]
fn crash_worker() {
    use std::io::Write;
    let Ok(root) = std::env::var("LIBAI_WORKFLOW_CRASH_ROOT") else {
        return;
    };
    let store: SharedExecutionStore =
        Rc::new(RefCell::new(FileExecutionStore::plaintext(&root).unwrap()));
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("write"), move |_, _| {
            let mut file = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(std::path::Path::new(&root).join("effect"))
                .unwrap();
            file.write_all(b"once\n").unwrap();
            file.sync_all().unwrap();
            std::process::exit(71)
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        execution_store: Some(store),
        ..Default::default()
    };
    graph.run("{}", &options).unwrap();
    panic!("worker failed to exit");
}
#[cfg(unix)]
#[test]
fn process_exit_no_duplicate_and_receipt() {
    use std::{
        fs,
        os::unix::fs::PermissionsExt,
        time::{SystemTime, UNIX_EPOCH},
    };
    let root = std::env::temp_dir().join(format!(
        "libai-rust-workflow-{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    fs::create_dir(&root).unwrap();
    fs::set_permissions(&root, fs::Permissions::from_mode(0o700)).unwrap();
    let exit = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "crash_worker", "--nocapture"])
        .env("LIBAI_WORKFLOW_CRASH_ROOT", &root)
        .status()
        .unwrap();
    assert_eq!(exit.code(), Some(71));
    let store: SharedExecutionStore = Rc::new(RefCell::new(
        FileExecutionStore::plaintext(root.to_str().unwrap()).unwrap(),
    ));
    let calls = Rc::new(Cell::new(0));
    let changes = calls.clone();
    let mut graph = Workflow::new().unwrap();
    graph
        .add_node(WorkflowNodeSpec::new("write"), move |_, _| {
            changes.set(changes.get() + 1);
            Ok(Default::default())
        })
        .unwrap();
    let options = WorkflowOptions {
        scope: "job",
        execution_store: Some(store.clone()),
        ..Default::default()
    };
    status(&graph.run("{}", &options).unwrap(), "recovery_required");
    assert_eq!(calls.get(), 0);
    let snapshot = store.borrow_mut().open("wfg3_job").unwrap();
    store.borrow_mut().close("wfg3_job");
    graph
        .confirm_node(
            store.clone(),
            WorkflowReceipt {
                scope: "job",
                initial_state_json: "{}",
                expected_revision: &snapshot.revision,
                node: "write",
                outcome: "applied",
                output: &Default::default(),
                reference: "effect-file",
            },
        )
        .unwrap();
    status(&graph.run("{}", &options).unwrap(), "completed");
    assert_eq!(calls.get(), 0);
    assert_eq!(fs::read_to_string(root.join("effect")).unwrap(), "once\n");
    drop(options);
    drop(store);
    drop(graph);
    fs::remove_dir_all(root).unwrap();
}

#[test]
fn child_agent_callback_panic_payload_boundaries() {
    struct Payload;
    impl Drop for Payload {
        fn drop(&mut self) {
            panic!("panic payload destructor");
        }
    }
    let mut agent = Agent::new(|_, _| std::panic::panic_any(Payload), "").unwrap();
    status(&agent.run("input").unwrap(), "model_error");
    let turns = Rc::new(Cell::new(0));
    let count = turns.clone();
    let mut agent = Agent::new(
        move |request, _| {
            count.set(count.get() + 1);
            if count.get() == 1 {
                ModelResponse {
                    calls: vec![ToolCall {
                        id: "one".into(),
                        name: "write".into(),
                        arguments: "{}".into(),
                    }],
                    ..Default::default()
                }
            } else {
                assert!(request.contains("\"tool_error\":true"));
                ModelResponse {
                    text: "done".into(),
                    ..Default::default()
                }
            }
        },
        "",
    )
    .unwrap();
    agent
        .add_tool("write", "", "{}", false, |_, _| {
            std::panic::panic_any(Payload)
        })
        .unwrap();
    status(&agent.run("input").unwrap(), "completed");
    assert_eq!(turns.get(), 2);
    let mut agent = Agent::new(
        |_, invocation| {
            let _ = invocation.emit("delta");
            ModelResponse {
                text: "done".into(),
                ..Default::default()
            }
        },
        "",
    )
    .unwrap();
    let mut stream = |_: &str| std::panic::panic_any(Payload);
    let hooks = Hooks {
        on_text: Some(&mut stream),
        ..Default::default()
    };
    status(
        &agent.run_with("input", &Default::default(), hooks).unwrap(),
        "cancelled",
    );
    let effects = Rc::new(Cell::new(0));
    let count = effects.clone();
    let mut agent = Agent::new(
        |_, _| ModelResponse {
            calls: vec![ToolCall {
                id: "one".into(),
                name: "write".into(),
                arguments: "{}".into(),
            }],
            ..Default::default()
        },
        "",
    )
    .unwrap();
    agent
        .add_tool("write", "", "{}", true, move |_, _| {
            count.set(count.get() + 1);
            ToolResult::default()
        })
        .unwrap();
    let mut authorize = |_: &str, _: &str, _: bool| std::panic::panic_any(Payload);
    let hooks = Hooks {
        authorize: Some(&mut authorize),
        ..Default::default()
    };
    status(
        &agent.run_with("input", &Default::default(), hooks).unwrap(),
        "cancelled",
    );
    assert_eq!(effects.get(), 0);
    let mut event = |_: &str| std::panic::panic_any(Payload);
    let hooks = Hooks {
        on_event: Some(&mut event),
        ..Default::default()
    };
    status(
        &agent.run_with("input", &Default::default(), hooks).unwrap(),
        "cancelled",
    );
    assert_eq!(effects.get(), 0);
}
