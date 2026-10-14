use libai::{
    adopt_execution_deadline, confirm_execution_effect, Agent, DeadlineAdoption, Error,
    ExecutionReceipt, ExecutionSnapshot, ExecutionStore, ModelResponse, SharedExecutionStore,
    ToolCall, ToolResult, Hooks, Options,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

#[derive(Default)]
struct Store {
    revision: u64,
    state: String,
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
            revision: if self.revision == 0 {
                String::new()
            } else {
                self.revision.to_string()
            },
            state_json: self.state.clone(),
        })
    }
    fn commit(&mut self, _: &str, expected: &str, state: &str) -> Result<String, Error> {
        assert!(self.leased);
        let current = if self.revision == 0 {
            String::new()
        } else {
            self.revision.to_string()
        };
        if current != expected {
            return Err(Error("CAS conflict".into()));
        }
        if self.fail_result && state.contains("\"state\":\"done\"") {
            self.fail_result = false;
            return Err(Error("result commit failed".into()));
        }
        self.revision += 1;
        self.state = state.into();
        Ok(self.revision.to_string())
    }
    fn close(&mut self, _: &str) {
        self.leased = false;
    }
}

#[test]
fn receipt_resume_and_completed_replay() {
    let store = Rc::new(RefCell::new(Store {
        fail_result: true,
        ..Default::default()
    }));
    let requests = Rc::new(Cell::new(0));
    let effects = Rc::new(RefCell::new(Vec::new()));
    let events=Rc::new(RefCell::new(Vec::<String>::new()));
    let run = |version: &str| {
        let requests = requests.clone();
        let effects = effects.clone();
        let mut agent = Agent::new(
            move |request, _| {
                requests.set(requests.get() + 1);
                if request.contains("\"role\":\"tool\"") {
                    ModelResponse {
                        text: "done".into(),
                        ..Default::default()
                    }
                } else {
                    ModelResponse {
                        calls: vec![
                            ToolCall {
                                id: "a".into(),
                                name: "write".into(),
                                arguments: "{}".into(),
                            },
                            ToolCall {
                                id: "b".into(),
                                name: "write".into(),
                                arguments: "{}".into(),
                            },
                        ],
                        ..Default::default()
                    }
                }
            },
            "",
        )
        .unwrap();
        agent
            .add_tool_versioned("write", "", "{}", false, version, move |_, invocation| {
                effects.borrow_mut().push(invocation.idempotency_key());
                ToolResult {
                    text: "written".into(),
                    error: false,
                }
            })
            .unwrap();
        agent
            .set_execution_store("task", Some(store.clone()))
            .unwrap();
        let mut observe=|event:&str|events.borrow_mut().push(event.to_string());
        agent.run_with("input",&Options::default(),Hooks{on_event:Some(&mut observe),..Default::default()}).unwrap()
    };
    assert!(run("1").contains("recovery_required"));
    assert!(!store.borrow().leased);
    assert_eq!(*effects.borrow(), vec!["task.0"]);
    assert!(events.borrow().iter().any(|e|e.contains("tool_intent_committed")&&e.contains("\"idempotency_key\":\"task.0\"")&&e.contains("\"attempts\":1")));
    assert!(!events.borrow().iter().any(|e|e.contains("tool_result_committed")));
    {
        let mut saved = store.borrow_mut();
        assert!(saved.state.starts_with("{\"time_budget\":{"));
        saved.state = format!("{{{}", &saved.state[saved.state.find("},").unwrap() + 2..]);
    }
    let legacy = store.borrow().state.clone();
    let migrate = |revision: &str, remaining_ms: i64, reference: &str| {
        adopt_execution_deadline(
            store.clone(),
            DeadlineAdoption {
                scope: "task",
                input: "input",
                instructions: "",
                expected_revision: revision,
                remaining_ms,
                reference,
            },
        )
    };
    let revision = store.borrow().revision.to_string();
    for (rev, ms, reference) in [
        ("stale", 1000, "audit"),
        (revision.as_str(), -1, "audit"),
        (revision.as_str(), i64::MAX, "audit"),
        (revision.as_str(), 1000, ""),
    ] {
        assert!(migrate(rev, ms, reference).is_err());
        assert!(!store.borrow().leased && store.borrow().state == legacy);
    }
    migrate(&revision, 300000, "host-budget-audit").unwrap();
    assert!(store.borrow().state.contains("host_adoption"));
    let current = store.borrow().revision.to_string();
    assert!(migrate(&current, 300000, "audit").is_err());
    assert!(run("1").contains("recovery_required"));
    assert_eq!(requests.get(), 1);
    let output = ToolResult {
        text: "written".into(),
        error: false,
    };
    let confirm = |revision: &str| {
        confirm_execution_effect(
            store.clone(),
            ExecutionReceipt {
                scope: "task",
                input: "input",
                instructions: "",
                expected_revision: revision,
                key: "task.0",
                output: &output,
                outcome: "applied",
                reference: "receipt/1",
            },
        )
    };
    assert!(confirm("stale").is_err());
    assert!(!store.borrow().leased);
    let revision = store.borrow().revision.to_string();
    confirm(&revision).unwrap();
    assert!(run("2").contains("recovery_required"));
    assert_eq!(effects.borrow().len(), 1);
    assert!(run("1").contains("completed"));
    assert_eq!(*effects.borrow(), vec!["task.0", "task.1"]);
    assert_eq!(requests.get(), 2);
    assert!(events.borrow().iter().any(|e|e.contains("tool_result_committed")&&e.contains("\"idempotency_key\":\"task.1\"")));
    events.borrow_mut().clear();
    assert!(run("1").contains("done"));
    let cached=events.borrow();assert_eq!(cached.len(),3);
    assert!(cached[0].contains("tool_result_reused")&&cached[1].contains("tool_result_reused")&&cached[2].contains("finished"));
    assert!(cached[0].contains("\"effect_state\":\"done\"")&&cached[0].contains(&format!("\"journal_revision\":\"{}\"",store.borrow().revision)));
    assert_eq!(requests.get(), 2);
    assert_eq!(effects.borrow().len(), 2);
}

#[test]
fn configuration_ownership_and_busy_lease() {
    let store = Rc::new(RefCell::new(Store::default()));
    let mut agent = Agent::new(
        |_, _| ModelResponse {
            text: "done".into(),
            ..Default::default()
        },
        "",
    )
    .unwrap();
    agent
        .set_execution_store("valid", Some(store.clone()))
        .unwrap();
    assert!(agent
        .set_execution_store("invalid/path", Some(store.clone()))
        .is_err());
    store.borrow_mut().leased = true;
    assert!(agent.run("input").unwrap().contains("recovery_required"));
    assert!(store.borrow().leased);
    store.borrow_mut().leased = false;
    // A caller-held RefCell borrow fails closed without panic or releasing the lease.
    let borrowed = store.borrow_mut();
    assert!(agent.run("input").unwrap().contains("recovery_required"));
    drop(borrowed);
    assert!(agent.run("input").unwrap().contains("completed"));
    assert!(store.borrow().state.contains("\"scope\":\"valid\""));
    agent.set_execution_store("", None).unwrap();
    assert!(agent.run("other").unwrap().contains("completed"));
}

#[test]
fn store_panic_stays_inside_abi() {
    struct Panics;
    impl ExecutionStore for Panics {
        fn open(&mut self, _: &str) -> Result<ExecutionSnapshot, Error> {
            panic!("injected");
        }
        fn commit(&mut self, _: &str, _: &str, _: &str) -> Result<String, Error> {
            unreachable!();
        }
        fn close(&mut self, _: &str) {
            unreachable!();
        }
    }
    let store: SharedExecutionStore = Rc::new(RefCell::new(Panics));
    let mut agent = Agent::new(|_, _| panic!("model must not run"), "").unwrap();
    agent.set_execution_store("panic", Some(store)).unwrap();
    assert!(agent.run("input").unwrap().contains("recovery_required"));
}

#[test]
fn host_side_effect_free_replay() {
    let store = Rc::new(RefCell::new(Store {
        fail_result: true,
        ..Default::default()
    }));
    let reads = Rc::new(Cell::new(0));
    let run = |policy: bool| {
        let count = reads.clone();
        let mut agent = Agent::new(
            |request, _| {
                if request.contains("\"role\":\"tool\"") {
                    ModelResponse {
                        text: "handled".into(),
                        ..Default::default()
                    }
                } else {
                    ModelResponse {
                        calls: vec![ToolCall {
                            id: "r".into(),
                            name: "read".into(),
                            arguments: "{}".into(),
                        }],
                        ..Default::default()
                    }
                }
            },
            "",
        )
        .unwrap();
        agent
            .add_tool_governed("read", "", "{}", false, "1", policy, move |_, _| {
                count.set(count.get() + 1);
                ToolResult {
                    text: "not found".into(),
                    error: true,
                }
            })
            .unwrap();
        agent
            .set_execution_store("read", Some(store.clone()))
            .unwrap();
        agent.run("input").unwrap()
    };
    assert!(run(true).contains("recovery_required"));
    assert!(run(false).contains("recovery_required"));
    assert_eq!(reads.get(), 1);
    assert!(run(true).contains("completed"));
    assert_eq!(reads.get(), 2);
}
