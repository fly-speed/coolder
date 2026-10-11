# Rust binding

没有第三方 crate 依赖，build.rs 用 CMake 将 C++ runtime 静态链接到应用。
目前按仓库内 path dependency 使用，构建需要相邻 `libai-c` 和 `libai` 源码，不能仅复制 crate 目录。
需要 Rust 1.77+（2021 edition）、CMake 和 C++11 编译器。

```toml
[dependencies]
libai = { package = "coolder-libai", path = "../coolder/libai-rust" }
```

```rust
use libai::{Agent, ModelResponse, ToolResult};
fn main() -> Result<(), libai::Error> {
    let mut agent = Agent::new(|request_json, invocation| {
        // 将应用的模型客户端输出转换成 ModelResponse。
        let _ = (request_json, invocation);
        ModelResponse { text: "你好".into(), ..Default::default() }
    }, "你是编程助手")?;
    agent.add_tool("echo", "Echo", "{}", false, |call, _| {
        ToolResult { text: call.arguments, error: false }
    })?;
    println!("{}", agent.run("hello")?);
    Ok(())
}
```

```sh
cargo test --offline --manifest-path libai-rust/Cargo.toml
cargo clippy --offline --manifest-path libai-rust/Cargo.toml --all-targets -- -D warnings
```

Agent 由 Drop 释放；可变借用串行化 run/reset/工具注册。Invocation 生命周期不能逃逸回调。
回调 panic 被捕获为运行错误（panic=abort 构建仍会退出进程）。Agent 刻意没有 Send/Sync，
可在应用工作线程内创建并运行，通过 on_text 返回 false 或 Invocation 截止时间协作停止。
模型请求/结果是 JSON 文本，应用可自行引入 serde_json 等解析器。
交叉编译必须显式设置 `LIBAI_CMAKE_TOOLCHAIN_FILE`，确保 toolchain 构建的是对应 Cargo target。

## 共享语义

所有绑定都执行同一个 `libai/sdk` C++ agent 循环，支持多轮会话、工具调用、
流式文本、逐次权限检查、执行预算、重试、取消和截止时间。
应用提供 model 回调，在回调中使用自己的 HTTP/模型 SDK；这些绑定没有内置网络客户端，
也没有打包依赖 ACL/OpenSSL 的旧版 provider、工作区或进程工具。

`run` 同步执行，请放在工作线程。回调也在该线程执行，Invocation 只在回调期间且在
该线程有效。取消是协作式的：网络请求和长任务需要检查 `stopped()` 并设置自己的超时。
同一个 agent 只有一个会话；并行会话应使用不同实例。执行时不能修改工具、reset 或关闭实例。

模型请求 JSON 包含 `instructions`、`max_output_tokens`、`messages` 和 `tools`。
消息字段为 `role/text/call_id/reasoning/tool_error/calls`，角色为 `user/assistant/tool`；
`calls` 中的 `arguments` 和 `tools` 中的 `parameters` **是 JSON 字符串**。
结果包含 `status/text/error/model_calls/tool_calls/input_tokens/output_tokens`；
状态包括 `completed/cancelled/deadline_exceeded/budget_exceeded/model_error/protocol_error/callback_error`。
工具失败会作为工具消息返回给模型；它不一定意味着整个 run 失败。
需要审批的工具在缺少授权回调时默认拒绝。所有跨 ABI 字符串为无内嵌 NUL 的 UTF-8。
