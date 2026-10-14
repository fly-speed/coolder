# Java / JNI binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
额外参数透传给 CMake；支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量。安装产物在 `build/install/`。
输出包括 `libai-java.jar` 和 JNI 动态库；设置 `JAVA_HOME` 选择完整 JDK，打包 JAR 需要 Python 3（可用 `PYTHON` 指定）。

Java 8+；包名 `io.coolder.libai`，类 `Agent`。桌面 JNI 需要完整 JDK 的 JNI 头文件。
Java 回调接收 JSON 文本，可选择应用已有 JSON 库。Android 打包入口见 `../Android`。

```sh
export JAVA_HOME=/path/to/full/jdk
cmake -S libai-java -B build/libai-java -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-java --parallel
javac -encoding UTF-8 -d build/java-classes libai-java/src/main/java/io/coolder/libai/Agent.java libai-java/tests/AgentTest.java
java -Djava.library.path=build/libai-java -cp build/java-classes AgentTest
jar cf build/libai-java.jar -C build/java-classes io
```

原生库 `libai_jni.so` / `libai_jni.dylib` / `ai_jni.dll` 必须与 JVM 架构一致。
CMake 可用 `-DLIBAI_JNI_INCLUDE_DIR=/path/to/jdk/include` 手动指定头文件；
macOS 可用 `-DCMAKE_OSX_ARCHITECTURES=x86_64` 对应 Intel JVM。

```java
import io.coolder.libai.Agent;

try (Agent agent = new Agent((requestJson, invocation) -> {
    return new Agent.Response("模型回答"); // 替换为应用的模型客户端
}, "你是编程助手")) {
    agent.addTool("echo", "Echo input", "{\"type\":\"object\"}", false,
        (call, invocation) -> new Agent.ToolResult(call.arguments));
    System.out.println(agent.run("hello"));
}
```

必须使用 try-with-resources 或显式 close：JNI 为活跃回调持有 Java owner 的 global reference。
执行时 close 会报错，cancel 可从另一线程调用。模型/工具 Java 异常成为模型/工具错误；
观察者异常触发取消。参数/schema 为 JSON 文本。反射调用字段必须保留；Android AAR 已带 R8 规则。

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

`CodingService.available() / new CodingService(root).call(operation, argumentsJson)` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。

JNI 构建只需要头文件，不依赖 AWT 或链接 JVM。脚本按选定 Java 的 `java.home` / `JAVA_HOME` 探测完整 JDK；macOS/Linux 上也可自动使用已安装 Android NDK 的 VM 通用 JNI 头文件，支持 Android Studio 内置 JDK 缺少 `include/` 的情况。NDK 默认位置为 macOS 的 `~/Library/Android/sdk` 或 Linux 的 `~/Android/Sdk`，也可设置 `ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME`。没有可用头文件时，配置阶段给出完整 JDK 或显式头文件路径的修复提示。无需额外指定临时目录中的 JNI 头文件。


## MCP 工具接入

`McpClient`与`Agent.addMcpTool`提供Java8+接口，协议、Schema及执行恢复复用C ABI。
不引入JSON库；初始化、目录和工具调用的参数/返回值均为UTF-8 JSON文本。

```java
try (McpClient client = McpClient.stdio(
        "{\"executable\":\"/absolute/path/to/python3\",\"arguments\":[\"/absolute/path/to/server.py\"]}")) {
    client.initialize("my-app", "1.0");
    String toolsJson = client.listTools();
    try (Agent agent = new Agent(myModel)) {
        agent.addMcpTool(client, "search", "mcp_search", policyJson);
        Agent.Hooks hooks = new Agent.Hooks();
        hooks.authorize = myAuthorization;
        String result = agent.run("搜索项目文档", new Agent.Options(), hooks);
    }
}
```

myModel/myAuthorization由宿主提供。policyJson必填parameters_schema和execution_revision，
可选output_schema、requires_approval、side_effect_free、timeout_ms；Schema字段是本地
受限Draft-04 JSON文本字符串。默认需要授权、无副作用=false，不信任服务器只读提示。
远程outputSchema存在时需宿主输出规则，不能静默转换现代Schema。

自定义`McpClient.Transport`实现request/notify/close。request返回完整JSON-RPC响应，
contextJson含request_id/protocol_version。Invocation只在该回调线程与回调期间有效，
可检查stopped()和复制idempotencyKey()；网络传输必须配合取消/期限。close须可靠释放
且不抛异常。JNI缓存回调类/方法并持有传输全局引用，不复用创建线程的JNIEnv；
异常转换成有界原生错误，最后一个绑定/连接释放后删除全局引用。

`close()`/try-with-resources只释放外层句柄，Agent绑定仍保持连接可用；
`closeConnection()`显式关闭所有共享绑定。请显式关闭Agent和客户端，不依赖GC回收
原生句柄。使用外层句柄直接调用期间，关闭/绑定的并行或重入操作被拒绝；绑定配置要求Agent空闲。
Agent独立持有底层连接，外层句柄释放不使绑定悬空。

initialize/listTools/callTool可指定timeoutMs和Agent.Cancellation，调用保留独立令牌直到
返回，即使外层令牌随后close也不会使请求引用失效。直接callTool不获得Agent授权、
预算或副作用日志；需要这些规则时使用addMcpTool。未知写操作重连后仍等待收据确认。

stdioAvailable查询编译目标；iOS/Windows不可用，Android仅面向API28+构建时支持stdio，
API21可用宿主传输。内置HTTP仍待完成。详情见 [MCP.md](../libai/docs/MCP.md)。


本阶段主目录轻量JNI、完整APP JNI及仅使用安装JAR的独立消费项目均以-Xcheck:jni
运行McpTest通过，安装JAR已核对包含McpClient及Transport接口。既有五组Java回归
通过，coolder-java重建完成。完整JNI仍出现此前记录的SIGPIPE处理器提示，退出码0，
本阶段未修改该信号行为。Android API21 JNI交叉编译通过，无设备端运行验收。

## 通用工作流

`Workflow`提供NodeSpec、NodeResult、Context、Options及宿主confirmNode接口，复用
同一C ABI的状态、条件分支、并行、合流与图级持久执行，无需额外JSON库。

```java
try (Workflow graph = new Workflow()) {
    Workflow.NodeSpec spec = new Workflow.NodeSpec("answer");
    spec.writes = new String[]{"answer"};
    graph.addNode(spec, (state, context) -> new Workflow.NodeResult("{\"answer\":42}"));
    String resultJson = graph.run("{}");
    // Check root status/state_json; task_contract.accepted remains false.
}
```

NodeSpec.executionRevision默认1，parallelSafe/sideEffectFree默认false。writes声明
可写顶层键；NodeResult.patchJson为JSON对象文本，route/status/error为字符串，status
默认completed。null结果、Java异常、无效Unicode/JSON、重复键、未声明写入或超限
均失败关闭，不把回调返回错误当作成果完成。库不会根据模型文字补造验收证据。

Options提供scope、executionStore、cancellation、maxParallel、maxNodeCalls、maxStateBytes、
timeoutMs，默认与SDK一致。返回JSON含status/state_json/error/nodes/node_calls/
progress_available/journal_revision/task_contract。输入/结果与patch均为JSON文本，宿主
可选自己的JSON库；完整限制和恢复规则见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

默认节点在调用线程执行；maxParallel>1同轮节点必须parallelSafe=true且writes不冲突。
并行节点在原生工作线程附加到JVM后回调Java，宿主负责捕获状态的线程安全；同图
重入、并发运行/修改/关闭立即拒绝。JNI缓存Workflow类和dispatch方法引用，避免
工作线程ClassLoader查找；JNIEnv按当前线程取得，仅主动附加的线程会在返回时分离。
成功注册的Node全局引用保留至close，失败注册立即释放，回调局部引用在有界frame中。
必须使用try-with-resources或显式close，JVM退出前关闭图，不依赖GC回收原生资源。

Context.stopped()/remainingMs()/retainCancellation()只在回调线程与回调内有效；返回
后即过期，executionScope字符串可复制。remainingMs为-1无限、0到期；复制取消令牌
独立拥有，需close。子Agent可在回调内创建，并使用不同存储owner：

```java
try (Agent agent = new Agent(model)) {
    agent.addTool("lookup", "", schema, false, lookup);
    agent.setExecutionStore(context.executionScope, childStore);
    try (Workflow.ChildOptions child = context.agentOptions()) {
        String result = agent.run(stateJson, child.options, new Agent.Hooks(), child.cancellation);
        // Map child status/validated output into NodeResult, preserving recovery_required.
    }
}
```

该片段中的model/schema/lookup/childStore由宿主提供。agentOptions保留父取消状态并
继承剩余期限，过期时拒绝启动；退出后释放令牌，不得继续使用。取消仍是协作式，
不能强制中断任意阻塞Java代码。模型/工具/存储回调可在上述已附加的工作线程执行。

持久化使用Options.executionStore及稳定scope，Store的open/commit/close在图调用
线程同步执行。图与子Agent必须使用不同lease owner，恢复须沿用相同图定义、版本、
安全声明及初始状态。效果未知默认停止，宿主核对完整节点后调用confirmNode，提供
当前CAS revision、none/applied结论、NodeResult和非空receipt reference；输出继续
受写入与路由校验。不要把该接口暴露为无需授权的模型工具。Options/NodeResult在
调用过程中应由宿主保持稳定，不能在其它线程修改这些配置或关闭正在使用的store。

本阶段主目录coolder-java重建成功；仅使用安装JAR的独立消费测试在轻量/完整JNI
上各8组通过，包含工作流、Agent、治理、持久执行、文件存储、结构化输出、MCP和
Coding。工作流覆盖实际线程并行、子Agent、引用回收与新JVM退出后的不重写/收据
恢复。Android arm64 API21交叉编译通过，未验证设备或Windows运行。完整JNI仍有
既有SIGPIPE处理器提示，测试退出码为0；信号行为尚未修复，日志保留。

## 可插拔 RAG

`Rag`提供独立Java值：Document、Chunk/Span、Request、Hit/Result、VectorRecord/
VectorQuery、PreparedIndex、Limits、Options及Binding；公共接口不包含C/C++类型。
宿主接口为DocumentLoader、TextSplitter、EmbeddingModel、VectorStore及Retriever。
factory loader/splitter/embedding/vectorStore/retriever拥有宿主，utf8Splitter/
memoryVectorStore/vectorRetriever复用共享SDK的内置组件，不绑定厂商或数据库。

```java
Rag.EmbeddingModel host = new Rag.EmbeddingModel() {
    public String spaceId() { return "example-constant-vector-v1"; }
    public double[][] embed(String[] texts, Rag.Context context) {
        // 离线接口演示；真实使用替换为Embedding服务并配合取消/期限。
        if (context.stopped()) throw new IllegalStateException("stopped");
        double[][] result = new double[texts.length][2];
        for (double[] vector : result) vector[0] = 1;
        return result;
    }
};
Rag.Options options = new Rag.Options("tenant_a");
try (Rag embedding = Rag.embedding(host);
     Rag splitter = Rag.utf8Splitter(1024, 128);
     Rag store = Rag.memoryVectorStore()) {
    Rag.PreparedIndex batch = splitter.prepareIndex(embedding,
        new Rag.Document[]{new Rag.Document("manual", "manual.txt", "r1", "Installation instructions")}, options);
    // prepare不写库；可先保存batch.toJson()或审核，再显式提交。
    store.commitIndex(batch, options);
    try (Rag retriever = Rag.vectorRetriever(embedding, store)) {
        Rag.Result result = retriever.retrieve(new Rag.Request("installation"), options);
        for (Rag.Hit hit : result.hits) System.out.println(hit.chunk.source + ": " + hit.chunk.text);
    }
}
```

metadataJson/filterJson是JSON对象文本；Span是原文UTF-8字节位置。Shared C ABI
检查来源、版本、metadata、跨度、范围、过滤、维数、有限值及总量。Java私有codec
只编解码Native输入/输出和独立值；`commitIndexJson`原样提交保存JSON，在调用存储
前由C ABI拒绝重复键、未知字段和错误类型，不先归一化JSON掩盖错误。

外部store.replaceDocuments正常返回void，错误/取消可能已写入，宿主负责原子性、
去重和收据，不自动重试；内存库是有界临时精确cosine索引。spaceId应为纯、快速、
稳定的配置读取；向量语义改变必须更新标识，不隐含网络操作。

```java
Rag.Binding policy = new Rag.Binding("corpus-v1-acl-v1-embedding-v1", "tenant_a");
policy.filterJson = "{\"kind\":\"manual\"}";
agent.addRetrievalTool(retriever, "knowledge_search", policy);
// 使用Agent.Hooks.authorize审批。
```

Agent冻结scope/filter/revision，模型只能提交query/top_k；默认requiresApproval=true、
sideEffectFree=false，沿用工具Schema、授权、预算、事件、取消与持久日志。输出有
citation/source/revision，按完整命中截断；引用格式正确不代表答案事实已证实，也
不会自动改变构建/测试/人工接受证据。revision须覆盖语料、ACL、Embedding、后端
和过滤语义；未知效果恢复停止，等待宿主核对，不让模型重新检索绕过确认。

组件是AutoCloseable，回调/运行/关闭重入或并发使用会失败；关闭外层输入组件后，
组合检索器和Agent仍保留原生依赖及Java宿主全局引用至最后释放。失败factory释放
本次全局引用；JNI缓存类/方法，当前线程取得自己的JNIEnv，本地引用帧在所有路径
释放。不得依赖GC释放句柄，必须try-with-resources或显式close；宿主在多个组件中
共享时负责线程安全，不在活跃回调中破坏外部资源。

Context是借用对象，返回后或在其它线程使用均失败。可读取accessScope/幂等键、
剩余期限、停止状态，并独立retainCancellation。`try (Rag.ChildOptions child =
context.operationOptions())`为不同组件的嵌套调用保留取消，调用前重新读取父期限；
过期不会转成不限时，不能保存child.options供上下文外使用。协作期限不能强行
中断任意宿主网络/阻塞回调。保存caller取消令牌在运行中close不释放已保留状态。
模型或Java异常不会穿过C ABI；即使异常toString失败也保留失败语义。

用法与边界见 [RAG.md](../libai/docs/RAG.md)和[C ABI说明](../libai-c/README.md#可插拔-rag-c-abi)。

本阶段主目录轻量/完整JNI各9组回归通过，独立消费目录只使用安装JAR编译和运行。
覆盖真实native工作流并行调用、最大向量维数、精确浮点、Unicode跨度、取消/父期限、
GC引用释放、异常toString失败、授权、缓存及未知效果不重放。coolder-java重建成功，
Android arm64 API21 JNI交叉编译通过；未验证设备或Windows。完整JNI既有SIGPIPE
处理器提示仍存在，测试退出码为0并保留日志，未声称修复。


## 独立任务契约与证据类型

`TaskContract`及嵌套`Evidence/Requirements/Checks/Evaluation`提供Java值类型，不引用
C++类型或完整编程后端。`evaluate()`调用共享核心tasks.evaluate，不复制接受规则。

```java
TaskContract contract = new TaskContract();
contract.loopFinished = true;
contract.loopOutcome = "completed";
contract.artifactRevision = "revision-1";
// Host supplies actual verified report revisions and the human decision.
contract.recordGeneration("revision-1", "draft-index")
        .recordBuild("revision-1", "compiler-report", true)
        .recordTests("revision-1", "test-report", true)
        .recordReview("revision-1", "human-review", true);
TaskContract.Evaluation result = contract.evaluate();
assert result.accepted && result.checks.build && result.checks.tests;
```

四项要求默认需要，证据默认unknown。证据和评估结果不可变，unmet是不可修改列表；
契约/要求由可信宿主维护。record方法不运行工具、不打开报告、不核实身份，不应
暴露为模型证据修改工具。改成果版本后，旧证据保留但不再有效；失败/拒绝开关为
Java boolean，generation/build/tests需host来源，review需human来源。

`fromJson/toJson`导入/保存事实，不保存输入accepted/checks注解。原始JSON先交给核心
校验；重复键、非法版本/类型、NUL和Unicode不能因解码而消失。未知注解允许null或
大整数，但不成为证据。`fromRunResult(agent.run(...))`读取实际Agent结果，模型文字
不升级为生成/构建/测试/审核证据；缺少契约的旧结果明确拒绝。运行结果包络有
8Mi UTF-16字符、64层和65,536节点限制，契约仍受核心2MiB字节限制。

私有JSON codec不依赖第三方库，数字保留原token，不能把1e0/1.0归一化为整数版本1。
新版类型需要支持tasks.evaluate的原生库；旧库明确抛出UnsupportedOperationException，
不会回退为Java自算接受规则。宿主应避免在评估期间并发修改契约。
