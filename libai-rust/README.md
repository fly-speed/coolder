# Rust binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
额外参数透传给 `cargo build`；默认离线 release 构建，产物 `build/target/release/liblibai.rlib`，原生静态库在 target 的 build 子目录。

默认 rag feature 使用 serde_json 编解码独立Rust值；关闭默认feature后基础Agent/MCP/工作流不依赖JSON crate。
build.rs 用 CMake 将 C++ runtime 静态链接到应用。
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

## 清理构建产物

运行 `./clean.sh` 清理默认构建输出；自定义输出目录使用
`./clean.sh /absolute/output/path` 或 `./clean.sh --output /absolute/output/path`。
只删除本绑定生成的目录和文件，保留输出目录中的其他文件；重复执行也可使用。

## 完整编码功能

`CodingService::new(root)`、`CodingService::available()` 和 `.call(operation, arguments_json)`
与其他语言提供相同操作协议。使用下列环境变量启用桌面 backend：

```sh
LIBAI_CODING_FULL=ON ACL_PATH=/absolute/acl OPENSSL_ROOT_DIR=/absolute/openssl ./build.sh
```

完整模式生成并链接 `libai_c` 动态库，开发构建写入其 rpath；发布 APP 时将 native build 的
install/lib 内动态库一并部署，并配置系统动态库搜索路径。默认模式仍链接 portable 静态桥接库。
`available` 在轻量和移动端构建返回 false。详见 [统一操作协议](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。


## MCP 工具接入

公开 `McpClient` / `McpTransport` / `SharedMcpTransport`、`McpOptions` 与 `McpBinding`。
协议、工具治理及执行恢复仍复用C ABI，不在Rust复制实现；MCP模块本身不需要JSON或网络crate。

```rust
use libai::{Agent, McpClient, McpOptions, McpBinding};
fn connect(agent: &mut Agent) -> Result<(), libai::Error> {
let mut client = McpClient::stdio(
    r#"{"executable":"/absolute/path/to/python3","arguments":["/absolute/path/to/server.py"]}"#)?;
client.initialize("my-app", "1.0", &McpOptions::default())?;
let catalog_json = client.list_tools(&McpOptions::default())?;
agent.add_mcp_tool(&client, "search", "mcp_search", &McpBinding {
    parameters_schema: r#"{"type":"object","required":["query"],"properties":{"query":{"type":"string"}},"additionalProperties":false}"#,
    execution_revision: "host-reviewed-search-v1",
    ..McpBinding::default()
})?;
// 默认需要授权；Agent运行时用Hooks提供宿主授权。
Ok(())
}
```

上例connect的外层client在返回时Drop；Agent仍保留底层连接，直到最后一个绑定释放。
`close()`显式关闭所有共享绑定。调用需要可变借用；客户端及共享传输不提供Send/Sync，
独立Cancellation可跨线程取消。不要用unsafe绕过Rust借用或把原生句柄移往其它线程。

自定义传输使用 `Rc<RefCell<dyn McpTransport>>`。request/notify接收原始JSON-RPC文本、
context_json(request_id/protocol_version)和仅当前回调有效的Invocation；request返回
完整响应JSON。检查stopped()，复制idempotency_key()后才能在回调外使用。
close必须可靠释放资源；不能在请求期间保留阻止回调的外部RefCell借用。借用冲突
失败关闭；此时宿主仍须自行清理无法借用的外部资源，不承诺被阻止的close已执行。

普通unwind panic转为传输错误，不穿越C ABI；release/析构panic也被捕获。
错误信息最多4KiB。panic payload自身析构再次panic时捕获第二次unwind并放弃该
嵌套payload，避免越过FFI；不将panic=abort或双重unwind进程终止当作可恢复异常。
这些机制不构成任意宿主代码的进程隔离。

initialize/list_tools/tool返回原生JSON文本；call_tool返回包含json字符串与error布尔值
的JSON对象。可由应用自己的JSON库解析。call(operation, arguments_json, cancellation)
提供完整C操作入口；直接工具调用不经过Agent授权、预算或副作用日志。
绑定默认授权=true、无副作用=false，Schema为宿主审查过的受限Draft-04，远程现代
Schema不被静默转换。工具定义固定；未知写操作重连后仍需收据核对，不自动重发。

stdio_available查询编译目标：iOS/Windows不可用，Android仅API28+构建包含stdio；
其它平台可由宿主提供传输。内置HTTP仍待完成。详情见 [MCP.md](../libai/docs/MCP.md)。


本阶段主目录轻量/完整模式各20项测试通过，独立Cargo消费项目仅依赖公开crate，
运行真实stdio及外层client先Drop后的Agent调用通过。coolder-rust重建通过；没有
重启服务或更改运行数据。本阶段未执行Windows/Linux/移动设备运行验收。

## 通用工作流

`Workflow`、`WorkflowNodeSpec`、`WorkflowNodeResult`、`WorkflowContext`、`WorkflowOptions`
与`WorkflowReceipt`封装共享C ABI图引擎，无需JSON crate。输入、patch和运行结果仍为
JSON文本字符串；执行规则见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

```rust
use libai::{Workflow, WorkflowNodeSpec, WorkflowNodeResult, WorkflowOptions};
let mut graph = Workflow::new()?;
let mut spec = WorkflowNodeSpec::new("answer");
spec.writes = &["answer"];
graph.add_node(spec, |_, _| Ok(WorkflowNodeResult {
    patch_json: "{\"answer\":42}".into(), ..Default::default()
}))?;
let result_json = graph.run("{}", &WorkflowOptions::default())?;
// Check root status/state_json; task_contract.accepted remains false.
graph.close()?;
```

该片段在返回Result的宿主函数内使用。节点返回Result<WorkflowNodeResult, Error>，
patch_json必须是JSON对象，route/status/error为字符串，status默认completed。
节点读取只读状态文本，仅返回的patch参加原子合并；writes明确允许写入的顶层键。
结果包含status/state_json/error/nodes/node_calls/progress_available/journal_revision/
task_contract，不将图结束升级为构建、测试或人工接受。

`add_node`允许普通非Send闭包，包括Rc/RefCell状态，并固定parallel_safe=false；
回调必须在创建线程执行，封装也检查调用线程。`add_parallel_node`要求闭包Send，
用Mutex管理FnMut并固定parallel_safe=true；只有该方法能允许原生工作线程执行。
两者的并行标志由封装派生，宿主不能通过NodeSpec将非Send回调伪装成并行安全。
显式max_parallel>1仍需同轮writes不冲突；普通节点参与多节点并行轮时执行前拒绝。
并行回调可捕获Arc/Mutex状态，不能捕获Rc/RefCell或外部非Send Agent/存储owner；
如需子Agent，可在工作线程回调内部创建本地Agent和独立存储owner。

Workflow自身不是Send/Sync，方法使用可变借用；回调Context/Invocation不能超出回调
或发送至其它线程。节点闭包由原生图拥有，注册失败回收本次Box，close/Drop释放
成功注册的闭包；不能通过捕获图形成Rc环后依赖Drop，需显式close。已关闭对象
拒绝后续操作。Mutex被panic毒化后回调拒绝访问，应核对效果并以相同定义重建图。

context.execution_scope提供子Agent稳定日志范围，Invocation幂等键为空。
context.invocation.remaining_ms为-1无限、0到期，retain_cancellation返回独立RAII令牌。
`context.agent_options()`返回(Options, Cancellation)，复制父取消状态并设置剩余期限，
到期时拒绝启动子Agent。保持返回令牌活到child.run_with_cancellation结束；取消/期限
是协作式，不能强制中断任意宿主代码。

WorkflowOptions.execution_store使用既有SharedExecutionStore；回调在图调用线程执行，
可继续使用Rc/RefCell适配器。图与子Agent使用不同lease owner，同一任务沿用scope、
初始状态、节点/边、执行版本及安全声明。未知效果恢复默认不重跑，宿主核对完整
节点后通过confirm_node(store, WorkflowReceipt {...})提供CAS revision与none/applied
收据；输出仍受声明/路由校验。不得将该确认方法暴露为无需授权的模型工具。

Node、存储、模型、工具与观察器回调的panic在FFI边界捕获；panic payload自身Drop
再次panic也在Rust内处理，只有这种损坏的二次payload会被遗忘，避免跨C展开。
panic=abort、OOM或正在展开时再次panic不能由catch_unwind恢复；宿主析构须可靠。
现有Agent/MCP API仍兼容，但部署原生动态库必须包含工作流及Invocation新入口。

本阶段主目录轻量与完整原生库各28项回归、非Send闭包不能注册并行的编译拒绝用例
通过；独立Cargo消费项目8项工作流测试与coolder-rust APP重建通过。测试覆盖实际
并行/分支/合流、未知效果/CAS收据、子Agent/取消、失败注册/析构、panic载荷析构，
以及新进程写入并fsync效果后退出、恢复不重写和确认后完成。未验证本阶段移动设备
或Windows运行，不将进程退出测试作为物理断电证明。

## 可插拔 RAG

默认 `rag` feature 提供独立Rust数据类型 `Document/DocumentChunk/RetrievalRequest/
RetrievalHit/RetrievalResult/VectorRecord/VectorQuery/PreparedIndex`，宿主trait为
`DocumentLoader/TextSplitter/EmbeddingModel/VectorStore/Retriever`。拥有组件是
`RagLoader/RagSplitter/RagEmbedding/RagVectorStore/RagRetriever`，close/Drop释放本次
句柄，组合检索器和Agent保留底层宿主。所有者非Send/Sync，回调在创建线程运行；
不能通过捕获组件形成Rc环后依赖Drop，需显式close打破环。Invocation只在回调有效，
Rust借用不能跨回调保留或发送到其它线程；取消令牌可以独立保留并跨线程触发。

```rust
use libai::*;
struct ExampleEmbedding;
impl EmbeddingModel for ExampleEmbedding {
    fn space_id(&mut self) -> Result<String, Error> {
        Ok("example-constant-vector-v1".into())
    }
    fn embed(&mut self, texts: &[String], context: &RetrievalContext<'_>)
        -> Result<Vec<Vec<f64>>, Error> {
        // 离线接口演示；真实使用替换为Embedding服务，并配合取消/期限。
        if context.stopped() { return Err(Error("stopped".into())); }
        Ok(vec![vec![1.0, 0.0]; texts.len()])
    }
}
fn example() -> Result<(), Error> {
    let limits = RagLimits::default();
    let options = RagOptions::new("tenant_a");
    let mut embedding = RagEmbedding::new(ExampleEmbedding, &limits)?;
    let mut splitter = RagSplitter::utf8(1024, 128, &limits)?;
    let mut store = RagVectorStore::memory(4096, 16*1024*1024, &limits)?;
    let batch = splitter.prepare_index(&mut embedding,
        &[Document::new("manual", "manual.txt", "r1", "Installation instructions")],
        &options, &limits)?;
    // prepare不写库；可保存batch.to_json()?或审核后再提交。
    store.commit_index(&batch, &options)?;
    let mut retriever = RagRetriever::vector(&embedding, &store, &limits)?;
    let result = retriever.retrieve(&RetrievalRequest::new("installation"), &options)?;
    for hit in result.hits { println!("{} {}", hit.chunk.source, hit.chunk.text); }
    Ok(())
}
```

`RagSplitter::new`支持宿主分块器，`RagVectorStore::new`支持外部向量库，
`RagRetriever::new`可直接接SQL、全文或混合检索，不强制Embedding。没有内置模型
厂商网络调用，也不绑定数据库。空间、来源、版本、过滤、维数、有限值、跨度和总量
使用共享C ABI/SDK验证。metadata_json/filter_json是JSON对象文本；span为原文UTF-8
字节位置。space_id是纯配置读取，向量语义改变必须改它。

`PreparedIndex::to_json`保存准备结果；`commit_index_json`原样提交保存的JSON，先由
C ABI拒绝重复键、未知字段、错误类型或越界，再调用存储。Rust不先解析/归一化保存
文本以掩盖错误。commit失败/取消可能已写入，宿主负责原子性、收据核对和去重，
库不会自动重试。内存库为有界临时精确cosine索引，不是持久数据库。

```rust
let mut policy = RagBinding::new("corpus-v1-acl-v1-embedding-v1", "tenant_a");
policy.filter_json = r#"{"kind":"manual"}"#.into();
agent.add_retrieval_tool(&retriever, "knowledge_search", &policy)?;
// 使用agent.run_with和宿主authorize回调授权。
```

绑定冻结scope/filter/revision，模型只有query/top_k。默认requires_approval=true、
side_effect_free=false，沿用工具Schema、授权、预算、事件、取消及持久日志。引用和
整条命中截断不代表答案事实已证实，也不自动改变构建、测试或人工接受证据。
execution_revision须覆盖语料、ACL、Embedding、后端和过滤语义。未知效果恢复停止，
等待宿主收据，不允许模型绕开效果核对重新规划。

`context.with_options(|options| ...)`在闭包内提供同一停止状态和重新读取的父期限，
适合不同组件的嵌套操作；期限过期不会变成不限时。取消/期限是协作式，宿主网络和
长任务须配合，不会强行中断任意回调。所有Rust回调及析构panic留在Rust边界内，
包括panic载荷析构再panic；panic=abort等进程终止方式不提供恢复保证。

RAG仅用serde_json编解码独立值，协议、权限、检索与恢复逻辑仍在共享C ABI。
新环境需先缓存Cargo依赖，再使用默认离线构建；不需要RAG时使用
`cargo build --offline --no-default-features`，或path dependency设置
`default-features = false`。完整编程后端仍由LIBAI_CODING_FULL控制，与rag feature独立。
协议及边界见 [RAG.md](../libai/docs/RAG.md)和[C ABI说明](../libai-c/README.md#可插拔-rag-c-abi)。

本阶段主目录轻量/完整库各37项回归及3个编译拒绝用例通过；独立Cargo消费项目
9项RAG测试通过。关闭RAG feature后28项及1个编译拒绝用例通过；默认/关闭feature
的全部target通过clippy零警告。coolder-rust重建和CLI启动检查成功，APP锁文件只
新增对已有serde_json的依赖边，保留离线locked构建。实际工具链为Rust1.97.1；
依赖最低版本声明已核对，尚未运行Rust1.77、设备或Windows验证。


## 独立任务契约与证据类型

`TaskContract`、`TaskEvidence`、`TaskRequirements`、`TaskChecks`和`TaskEvaluation`为
独立Rust值类型，默认及`--no-default-features`均提供，不增加依赖。
`evaluate()`调用共享核心tasks.evaluate，不复制接受规则，不需要完整编程后端。

```rust
use libai::TaskContract;
let mut contract = TaskContract {
    loop_finished: true,
    loop_outcome: "completed".into(),
    artifact_revision: "revision-1".into(),
    ..Default::default()
};
// Host verifies actual reports and the human decision before recording facts.
contract.record_generation("revision-1", "draft-index")
    .record_build("revision-1", "compiler-report", true)
    .record_tests("revision-1", "test-report", true)
    .record_review("revision-1", "human-review", true);
let result = contract.evaluate()?;
assert!(result.accepted && result.checks.build && result.checks.tests);
```

默认四项要求均需要，证据均unknown。修改成果版本使旧证据失效；生成/构建/测试
需host来源，审核需human来源。record方法不运行工具、不验证引用真实性，不应
暴露为模型证据修改工具。布尔开关由Rust类型约束；契约可Clone，纯值支持Send/Sync，
不可变共享评估不持有Agent或回调句柄，修改需要宿主的正常可变借用/同步。

`from_json/to_json`保存事实，忽略输入accepted/checks等派生注解；原始JSON先交核心
校验，不归一化1e0/1.0、重复键或非法Unicode。`from_run_result(&agent.run(...)?)`
读取实际Agent契约，不把模型文字转换成证据；缺少契约的旧结果明确拒绝。
结果包络有8MiB、64层容器与65,536个值限制，契约保持核心2MiB字节上限。
私有codec仅用std，保留数字原token，避免关闭RAG时引入serde_json依赖。
旧原生库缺少tasks.evaluate时明确报错，不回退到另一套Rust接受逻辑。
