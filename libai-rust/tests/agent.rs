use libai::{Agent, Hooks, ModelResponse, Options, ToolCall, ToolResult};
use std::{cell::Cell, rc::Rc};
#[test]
fn unicode_tools_stream_and_limits() {
    let mut turn = 0;
    let mut agent = Agent::new(
        move |request, invocation| {
            turn += 1;
            if turn == 1 {
                ModelResponse {
                    calls: vec![ToolCall {
                        id: "编号".into(),
                        name: "echo".into(),
                        arguments: "{}".into(),
                    }],
                    ..Default::default()
                }
            } else {
                assert!(request.contains("你好🌏"));
                assert!(invocation.emit("你好🌏").unwrap());
                ModelResponse {
                    text: "你好🌏".into(),
                    ..Default::default()
                }
            }
        },
        "测试",
    )
    .unwrap();
    let count = Rc::new(Cell::new(0));
    let captured = count.clone();
    agent
        .add_tool("echo", "Echo", "{}", true, move |call, _| {
            assert_eq!(call.id, "编号");
            captured.set(captured.get() + 1);
            ToolResult {
                text: "你好🌏".into(),
                error: false,
            }
        })
        .unwrap();
    let mut chunks = String::new();
    let mut stream = |text: &str| {
        chunks.push_str(text);
        true
    };
    let mut allow = |_: &str, _: &str, required: bool| required;
    let result = agent
        .run_with(
            "开始",
            &Options::default(),
            Hooks {
                on_text: Some(&mut stream),
                authorize: Some(&mut allow),
                ..Default::default()
            },
        )
        .unwrap();
    assert!(result.contains("completed") && result.contains("你好🌏"));
    assert_eq!(chunks, "你好🌏");
    assert_eq!(count.get(), 1);
    agent.reset().unwrap();
    let options = Options {
        max_model_calls: 0,
        ..Default::default()
    };
    assert!(agent
        .run_with("budget", &options, Hooks::default())
        .unwrap()
        .contains("budget_exceeded"));
}
#[test]
fn panic_stays_inside_callback() {
    let mut agent = Agent::new(|_, _| panic!("failed"), "").unwrap();
    assert!(agent.run("start").unwrap().contains("model_error"));
}
