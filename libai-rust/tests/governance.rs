use libai::{governance_call, Agent, ModelResponse, ToolCall, ToolResult};
use std::{cell::Cell, rc::Rc};
#[test]
fn portable_schema_and_execution_guard() {
    assert!(governance_call("capabilities", "{}")
        .unwrap()
        .contains("draft4"));
    let args = r#"{"schema_json":"{\"type\":\"integer\"}","instance_json":"\"bad\""}"#;
    assert!(governance_call("schema.validate", args)
        .unwrap()
        .contains("\"valid\":false"));
    let calls = Rc::new(Cell::new(0));
    let counted = calls.clone();
    let mut turns = 0;
    let mut agent = Agent::new(
        move |_, _| {
            turns += 1;
            if turns == 1 {
                ModelResponse {
                    calls: vec![ToolCall {
                        id: "a".into(),
                        name: "write".into(),
                        arguments: r#"{"n":"bad"}"#.into(),
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
    .unwrap();
    agent
        .add_tool(
            "write",
            "write",
            r#"{"type":"object","required":["n"],"properties":{"n":{"type":"integer"}}}"#,
            false,
            move |_, _| {
                counted.set(counted.get() + 1);
                ToolResult {
                    text: "ok".into(),
                    error: false,
                }
            },
        )
        .unwrap();
    assert!(agent.run("write").unwrap().contains("completed"));
    assert_eq!(calls.get(), 0);
}

#[test]
fn model_profile_limits() {
    let mut agent = Agent::new(
        |q, _| {
            assert!(q.contains("\"tools\":[]"));
            assert!(q.contains("\"max_output_tokens\":7"));
            ModelResponse {
                text: "done".into(),
                ..Default::default()
            }
        },
        "",
    )
    .unwrap();
    agent
        .configure_model(r#"{"version":1,"tool_calling":false,"max_output_tokens":7}"#)
        .unwrap();
    assert!(agent.configure_model(r#"{"version":2}"#).is_err());
    assert!(agent.run("hello").unwrap().contains("completed"));
}

#[test]
fn portable_task_contract() {
    assert!(governance_call(
        "tasks.evaluate",
        r#"{"loop_finished":true,"loop_outcome":"completed"}"#
    )
    .unwrap()
    .contains("\"accepted\":false"));
    assert!(governance_call("tasks.evaluate",r#"{"loop_finished":true,"loop_outcome":"completed","requirements":{"generation":false,"build":false,"tests":false,"review":false}}"#).unwrap().contains("\"accepted\":true"));
}
