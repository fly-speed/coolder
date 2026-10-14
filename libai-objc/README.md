# Objective-C binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
额外参数透传给 CMake；支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量。安装产物在 `build/install/`。

Foundation + ARC 的 Objective-C/Objective-C++ 接口，公共头文件 `LAIAgent.h`。
支持 macOS 和 iOS，包含 Clang module `LibAI`；iOS 分发见 `../iOS`。
静态库将 C bridge 和 C++ runtime 一起打包。

```sh
cmake -S libai-objc -B build/libai-objc -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-objc --parallel
ctest --test-dir build/libai-objc --output-on-failure
```

```objc
#import <LibAI/LAIAgent.h>
NSError *error = nil;
LAIAgent *agent = [[LAIAgent alloc] initWithModel:
    ^LAIModelResponse *(NSString *requestJSON, LAIInvocation *invocation) {
        LAIModelResponse *response = [LAIModelResponse new];
        response.text = @"你好"; // 替换为应用自己的模型客户端
        return response;
    } instructions:@"你是编程助手" error:&error];
NSDictionary *result = [agent run:@"hello" options:nil hooks:nil error:&error];
NSLog(@"%@", result[@"text"]);
[agent close:&error];
```

引用为普通 include 目录时使用 `#import "LAIAgent.h"`；作为 module 可用 `@import LibAI`。
运行结果是 NSDictionary；NSError 表示桥接调用失败，模型失败用结果的 status/error 表达。
reset 清空会话；cancel 可跨线程；close/dealloc 释放原生资源。模型/工具异常会转成运行错误，
观察者异常触发取消。block 避免强引用捕获 agent 自身，以免构成 retain cycle。
链接静态库时启用 ARC，并链接 Foundation 与 libc++；纯 Objective-C 应用增加 `-lc++`。

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

`[LAICodingService available] / initWithRoot: / call:arguments:error:` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。


## MCP 工具接入

`LAIMcpClient`、`LAIMcpTransport`与`LAIAgent addMcpTool:...`复用C ABI协议和治理，
公开头文件只使用Foundation与绑定自身类型，原生对象和桥接实现留在src。

```objc
NSError *error = nil;
LAIMcpClient *client = [LAIMcpClient stdio:@{
    @"executable": @"/absolute/path/to/python3",
    @"arguments": @[@"/absolute/path/to/server.py"]
} error:&error];
[client call:@"initialize" arguments:@{@"name":@"my-app", @"version":@"1.0"}
    cancellation:nil error:&error];
NSArray *tools = [client call:@"list_tools" arguments:@{} cancellation:nil error:&error];
// agent 是绑定了宿主模型的 LAIAgent。
[agent addMcpTool:client remoteName:@"search" localName:@"mcp_search" policy:@{
    @"parameters_schema": @{@"type":@"object", @"required":@[@"query"],
        @"properties":@{@"query":@{@"type":@"string"}}, @"additionalProperties":@NO},
    @"execution_revision":@"host-reviewed-search-v1"
} error:&error];
// 运行时仍须提供宿主授权；检查各返回值及 NSError。
```

策略必填parameters_schema/execution_revision，可选output_schema、requires_approval、
side_effect_free、timeout_ms；默认授权=true、无副作用=false。Schema可为原始JSON文本
或字典；字典按排序键编码（macOS10.13/iOS11+），便于同一规则持久恢复。现代远程
Schema不被静默转换为受限Draft-04。直接call属于宿主操作，不获得Agent日志或授权。

自定义传输实现request/notify/close：收到原始JSON-RPC文本、context字典和回调期间
有效的Invocation。context含request_id/protocol_version；网络传输必须配合stopped。
返回的response为完整JSON-RPC字符串；NSError/NSException/C++异常转为有界错误，
Invocation在finally过期。close须可靠释放资源且不抛异常。

ARC桥接把传输owner交给原生release，最后一个连接/绑定释放才回收。dispose/dealloc
只释放外层句柄；closeConnection关闭所有共享绑定。使用外层句柄直接调用期间拒绝
关闭/配置重入；Agent自身配置必须空闲，不能让销毁与调用并行。未知写入仍须收据核对。
stdioAvailable在iOS返回NO，自定义传输入口仍可编译；macOS可使用本地子进程。
协议和边界见 [MCP.md](../libai/docs/MCP.md)。


本阶段主目录8项回归通过，iOS arm64模拟器交叉编译通过。独立消费者只使用安装
目录的LibAI头文件、模块映射及静态库，以@import LibAI编译并运行相同MCP/stdio/
恢复/生命周期测试通过；Objective-C++模块导入需同时启用-fmodules和-fcxx-modules。
安装包不包含原始libai头文件。无设备运行验收，未重启APP或修改运行数据。

## 通用工作流

`LAIWorkflow`提供独立Foundation类型：NodeSpec、NodeResult、Context、Options，以及
宿主confirmStore收据接口。公开头文件不引用libai或C桥接头文件，复用同一DAG引擎。

```objc
LAIWorkflow *graph = [LAIWorkflow new];
LAIWorkflowNodeSpec *spec = [LAIWorkflowNodeSpec new];
spec.name = @"answer"; spec.writes = @[@"answer"];
NSError *error = nil;
[graph addNode:spec handler:^LAIWorkflowNodeResult *(NSDictionary *state,
        LAIWorkflowContext *context, NSError **error) {
    LAIWorkflowNodeResult *result = [LAIWorkflowNodeResult new];
    result.patchJSON = @"{\"answer\":42}";
    return result;
} error:&error];
NSDictionary *result = [graph run:@{} options:[LAIWorkflowOptions new] error:&error];
// Check result/status; task_contract.accepted remains NO.
[graph dispose:&error];
```

NodeSpec.executionRevision默认1，parallelSafe/sideEffectFree默认NO；writes声明可写顶层
键。NodeResult.patchJSON是JSON对象文本，route/status/errorMessage为字符串，status默认
completed。回调状态是独立解析的NSDictionary；只有返回patch参与状态合并，不能用
修改输入对象替代返回结果。nil/NSError、NSException、C++异常、非法JSON/UTF-8、
重复键、未知写入或超限失败关闭，效果未知时按宿主收据规则停止。

默认在调用线程执行节点；maxParallel>1时同轮节点须parallelSafe=YES且writes不冲突，
原生工作线程各有autoreleasepool，宿主负责block捕获状态的线程安全。成功注册后
原生图拥有复制block，失败注册释放本次保留；dispose/dealloc释放图。若block捕获
graph形成ARC环，必须显式dispose或改用weak引用。并发运行/修改/活跃dispose拒绝。

Options包含scope、executionStore、cancellation、maxParallel、maxNodeCalls、maxStateBytes、
timeoutMs。返回Foundation字典包含status/state_json/error/nodes/node_calls/
progress_available/journal_revision/task_contract，图完成不能证明构建/测试/人工验收。
run:字典输入采用排序键编码，要求macOS10.13/iOS11+；runJSON:原始JSON入口可保留
其它语言保存的输入文本。完整边界见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

Context的stopped/remainingMs/retainCancellation:只在回调线程与回调内有效，返回后
失效；executionScope字符串可复制，为子Agent日志范围，不是统一业务幂等键。
remainingMs为-1无限、0到期；retainCancellation返回独立ARC owner。agentOptions:
提供保留父取消状态与剩余期限的LAIRunOptions，过期拒绝启动；子Agent需使用独立
存储owner。取消与期限是协作式，不能强制终止任意阻塞宿主代码。

图的executionStore在调用线程同步open/commit/close，保留至运行返回。恢复同一
scope须沿用初始状态、图/版本及安全声明。核对完整节点效果后，宿主通过
confirmStore:scope:initialJSON:expectedRevision:node:outcome:output:reference:error:
提交none/applied收据，CAS、声明写入与路由仍由核心校验。不要把确认方法直接暴露
为模型工具。Options/NodeResult/store必须由宿主保持稳定，避免并行修改或关闭资源。

本阶段主目录轻量/完整库各9项回归通过；独立消费项目以@import LibAI仅使用安装
头文件和静态库运行相同工作流测试通过，无需原生SDK头文件。覆盖真实并行、子
Agent、ARC生命周期、两类异常、取消、收据及进程退出不重写。iOS arm64模拟器
交叉编译通过，未验证设备或Windows，不将进程退出作为物理断电证明。

## 可插拔 RAG

`LAIRag.h`提供独立Foundation值（Document、Chunk/Span、Request、Hit/Result、
VectorRecord/VectorQuery、PreparedIndex、Limits、Options及Binding），以及五类
宿主协议LAIRagDocumentLoader/TextSplitter/EmbeddingModel/VectorStore/Retriever。
公开头文件仅依赖Foundation和LAIAgent，不包含C/C++类型。LAIRag factory拥有宿主，
内置UTF-8分块、临时内存库和组合检索器复用共享C ABI，不绑定模型厂商或数据库。

```objc
NSError *error = nil;
LAIRagLimits *limits = [LAIRagLimits new];
LAIRagOptions *options = [LAIRagOptions new];
options.accessScope = @"tenant_a";
// hostEmbedding是应用实现LAIRagEmbeddingModel的对象。
LAIRag *embedding = [LAIRag embedding:hostEmbedding limits:limits error:&error];
LAIRag *splitter = [LAIRag utf8Splitter:1024 overlap:128 limits:limits error:&error];
LAIRag *store = [LAIRag memoryStore:4096 bytes:16*1024*1024 limits:limits error:&error];
LAIRagDocument *doc = [LAIRagDocument new];
doc.identifier = @"manual"; doc.source = @"manual.txt"; doc.revision = @"r1";
doc.text = @"Installation instructions";
LAIRagPreparedIndex *batch = [splitter prepareIndex:@[doc] embedding:embedding
    options:options limits:limits error:&error];
// prepare不写库；可先保存[batch toJSON:&error]或审核，再显式提交。
BOOL ok = batch && [store commitIndex:batch options:options error:&error];
LAIRag *retriever = ok ? [LAIRag vectorRetriever:embedding store:store limits:limits error:&error] : nil;
LAIRagRequest *request = [LAIRagRequest new]; request.query = @"installation";
LAIRagResult *result = [retriever retrieve:request options:options error:&error];
// 用毕dispose组件；组合检索器和Agent会保留底层原生依赖。
```

metadataJSON/filterJSON为JSON对象文本，span为原文UTF-8字节位置。宿主Embedding
返回NSNumber向量（有限数值，不含Boolean），spaceID应为纯、快速、稳定的配置读取；
向量语义改变须更新它。VectorStore正常返回YES，不自动重试错误/取消后的写入，
宿主负责真实ACL、外部存储原子性、去重和收据。内存库不是持久数据库。
`commitIndexJSON`原样传入保存的JSON，先经C ABI拒绝重复键、未知字段、错误类型、
来源范围和向量错误，再调用宿主存储；不先用Foundation解析/归一化掩盖错误。

```objc
LAIRagBinding *policy = [LAIRagBinding new];
policy.executionRevision = @"corpus-v1-acl-v1-embedding-v1";
policy.accessScope = @"tenant_a";
policy.filterJSON = @"{\"kind\":\"manual\"}";
[agent addRetrievalTool:retriever name:@"knowledge_search" binding:policy error:&error];
```

Agent冻结scope/filter/revision，模型只能提交query/top_k；默认要求审批、不允许安全
重放，沿用Schema、授权、预算、事件、取消和执行日志。引用和整条命中截断不代表
答案事实已证实，不自动改变构建/测试/人工接受证据。revision须覆盖语料、ACL、
Embedding、后端与过滤语义；未知效果恢复停止等待宿主核对，不能让模型绕过确认。

ARC在最后一个原生组件/Agent引用释放时回收宿主；失败factory释放本次引用，原生
创建后Objective-C分配失败也清理。捕获宿主/组件形成ARC环时必须显式dispose，不能
只依赖dealloc；活跃、并发或重入关闭失败。回调有autoreleasepool，NSException及
C++异常保持失败语义，不跨C边界传播；Context在所有路径过期，只能在回调线程使用。
可独立retainCancellation，`operationOptions:`保留取消并在嵌套调用前重新读取父
期限；过期不转为不限时，options不能留到回调之外。协作式期限不能强行中断阻塞回调。

协议和边界见 [RAG.md](../libai/docs/RAG.md)与[C ABI](../libai-c/README.md#可插拔-rag-c-abi)。

本阶段主目录轻量/完整库各10项回归通过；独立消费用例只使用安装后的LibAI模块、
Foundation头文件和静态库，运行相同RAG验收用例通过。纯Objective-C公开头文件
编译与iOS arm64模拟器交叉编译通过，尚未验证设备或Windows运行。SDK与六语言
RAG入口已接入，平台其它未完成项仍保留。


## 独立任务契约与证据类型

`LAITaskContract/Evidence/Requirements/Checks/Evaluation`提供Foundation接口，公共头文件
`LAITaskContract.h`只引用Foundation，模块`LibAI`导出全部类型。SDK类型及转换仅在
私有src中。evaluateWithError使用共享核心，不复制接受规则，不需要项目目录或模型。

```objective-c
LAITaskContract *contract = [LAITaskContract new];
contract.loopFinished = YES;
contract.loopOutcome = @"completed";
contract.artifactRevision = @"revision-1";
NSError *error = nil;
// Record only after the host verifies actual reports and the human decision.
[contract recordGenerationRevision:@"revision-1" reference:@"draft-index" error:&error];
[contract recordBuildRevision:@"revision-1" reference:@"compiler-report" passed:YES error:&error];
[contract recordTestsRevision:@"revision-1" reference:@"test-report" passed:YES error:&error];
[contract recordReviewRevision:@"revision-1" reference:@"human-review" accepted:YES error:&error];
LAITaskEvaluation *result = [contract evaluateWithError:&error];
NSAssert(result && !error && result.accepted, @"Host evidence is incomplete");
```

默认四项要求均需要，证据unknown。证据、checks及评估结果只读，字符串/列表拥有
不可变副本；不同契约不共享默认要求。成果版本变化后旧证据保留但失效，只有当前
版本的host生成/构建/测试及human审核才可接受。record方法不运行工具、不核实引用，
不可作为模型证据修改工具。修改/评估期间的并发访问由宿主同步。

`JSONWithError`保存事实；`contractFromJSON`先由核心校验原始字节再构造Foundation
对象，输入accepted/checks注解不成为证据。raw重复键、1e0/1.0版本、非法Unicode/NUL
不归一化成有效数据。`contractFromRunJSON`验证完整原始结果后读取task_contract，
包络8MiB/64层/65,536值，契约仍为核心2MiB上限；模型最终文字不证明其它阶段通过。

`contractFromRunResult`用于实际LAIAgent返回的字典，缺少契约的旧结果明确拒绝。
`contractFromDictionary`面向可信宿主值，拒绝可识别的浮点版本与错误类型。外部先用
Foundation解析时已经丢失的重复键/数字形式无法找回，应从原始JSON入口导入。
方法失败返回nil/NO及NSError，record失败保留旧证据；Evidence初始化器拒绝非法
文本时抛NSException，便利记录方法将其转换为NSError。类型需要新版Objective-C库。
