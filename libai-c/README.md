# libai C ABI

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
额外参数透传给 CMake；支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量。安装产物在 `build/install/`。

该目录为 Python、Java、Rust、Objective-C 绑定提供统一的原生桥接层。
公共头文件为 `include/libai.h`，当前 `libai_abi_version()` 为 1。

```sh
cmake -S libai-c -B build/libai-c -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-c --parallel
ctest --test-dir build/libai-c --output-on-failure
cmake --install build/libai-c --prefix "$PWD/build/install"
```

共享库为 Linux `libai_c.so`、macOS `libai_c.dylib`、Windows `ai_c.dll`。
`-DBUILD_SHARED_LIBS=OFF` 生成静态库。只依赖 C++11 标准库、线程支持和 CMake。
C 程序链接共享库即可；静态链接还需相应 C++ 运行库。
安装后可使用 `find_package(libai_c CONFIG REQUIRED)` 和 `libai::c`。

完整工具循环见 `tests/bridge_test.c`。创建 agent 时传入模型函数及 user 数据，
`libai_agent_add_tool` 注册能力，`libai_agent_run` 返回 JSON，最后 `libai_agent_destroy`。
模型用 `libai_response_*` 填充输出；工具用 `libai_tool_result_set` 填充结果。
构建器复制输入字符串，不接管回调的内存。

所有 opaque builder/invocation 指针仅在当前回调有效。应用必须保持 callback/user 数据
存活至 agent 销毁，且不能让异常跨越 C ABI。run 返回的字符串必须用 `libai_string_free`
释放。错误返回 NULL/-1，调用 `libai_last_error()` 读取当前线程的诊断。
销毁必须由调用方与所有其他 API 调用（包括 cancel）串行化；不能在活跃 run 中销毁。
模型/工具回调中的分配失败会转成运行错误，不允许 C++ 异常穿过外语回调。

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

`libai_coding_available()/libai_coding_call(root, operation, arguments_json)` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。


## MCP C ABI

`libai_mcp_create(callback, user, release)` 创建可插拔传输客户端。callback 同步接收
request/notify/close；request 用 `libai_mcp_result_response(result, wire, bytes)` 复制
最多2MiB的完整JSON-RPC响应，用 `libai_mcp_result_error` 报告传输错误。响应不能设置
两次。notify不需要响应；close必须幂等且不抛异常。回调中的JSON、builder和invocation
只在回调内有效，后者可用 `libai_invocation_stopped` 检查期限/取消，以及读取幂等键。
context_json 包含 request_id 和 protocol_version，HTTP宿主据此发送协商版本头。

create成功才转移user回收责任；release在最后一个连接/Agent绑定释放时调用一次。
create失败不调用release。不提供release时，宿主须自行让user与回调活到所有绑定释放。
API及回调不得抛异常或访问已释放对象；不能让destroy与使用该句柄的API并行。
请求回调重入同一句柄的call/destroy会被拒绝。

`libai_mcp_call(handle, operation, arguments_json, cancellation)` 返回拥有的UTF-8 JSON，
用 `libai_string_free` 释放；失败返回NULL并设置 `libai_last_error`。

| operation | 参数 | 返回 |
| --- | --- | --- |
| initialize | name、version；可选timeout_ms | serverInfo对象 |
| list_tools | 可选timeout_ms | 工具数组 |
| tool | name | 单个工具对象 |
| call_tool | name、arguments_json；可选timeout_ms、idempotency_key | json文本字符串与error布尔值 |

工具对象包含name、description、input_schema/output_schema原始JSON字符串及
requires_tasks布尔值。arguments_json是工具参数的JSON文本字符串，不是嵌套对象。
参数未知字段、重复键和错误类型被拒绝；timeout_ms默认30000，范围1..86400000。
直接call_tool是受信任宿主调用，不经过Agent授权、预算和副作用日志。

`libai_agent_add_mcp_tool(agent, client, remote, local, policy_json)` 接入SDK执行规则。
policy必填parameters_schema、execution_revision，可选output_schema、requires_approval、
side_effect_free、timeout_ms。Schema字段为本地受限Draft-04的JSON文本；默认授权=true、
无副作用=false，远程现代Schema不被静默转换。Agent必须空闲，工具定义会被固定。

`libai_mcp_destroy` 只释放外层句柄；Agent保留连接及回调所有权。
`libai_mcp_close` 显式关闭连接，影响所有共享绑定；忙碌连接会拒绝close。
效果未知的工具不因为重连而重新执行，仍按宿主收据规则恢复。

`libai_mcp_create_stdio(options_json)` 提供桌面POSIX本地服务，配置字段executable、
arguments、inherit_environment、environment，数组项为字符串、最多256项。
executable为绝对路径，不使用shell或PATH搜索；环境默认继承，也可显式完整替换。
用 `libai_mcp_stdio_available` 查询；iOS/Windows暂不可用。Android仅在面向API28+
构建时包含stdio；API21等低版本仍可使用自定义传输。
协议、权限及存储边界见 [MCP.md](../libai/docs/MCP.md)。


安装包首次配置先初始化LIBAI_CODING_FULL，再导出CMake配置；完整/共享选项带引号，
空值不产生非法if语句。主目录静态/动态各4项回归，以及仅使用安装目录的C消费者
均通过；完整JNI(x86_64)与Rust原生库(arm64)也运行同一组C ABI MCP测试通过。
iOS arm64模拟器、Android arm64 API21/28交叉编译通过，不作为设备运行验收。

## 通用工作流 C ABI

`libai_workflow_create/add_node/add_edge/run/confirm_node/destroy` 封装同一SDK图执行器。
公开头文件只含C类型，独立于libai内部头文件。图支持JSON对象状态、条件分支、
显式声明安全的并行、合流和持久恢复；完整规则见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

```c
static void step(void *user, const char *state, const char *context,
                 libai_workflow_result *output, libai_invocation *control) {
    const char *value = "{\"patch_json\":\"{\\\"answer\\\":42}\"}";
    if (libai_invocation_stopped(control)) return;
    libai_workflow_result_set(output, value, strlen(value));
}
libai_workflow *graph = libai_workflow_create();
libai_workflow_add_node(graph, "{\"name\":\"answer\",\"writes\":[\"answer\"]}", step, NULL, NULL);
char *result = libai_workflow_run(graph, "{}", "{}", NULL, NULL, NULL);
/* Check result/status; NULL means libai_last_error(). */
libai_string_free(result);
libai_workflow_destroy(graph);
```

示例需包含libai.h与string.h。节点descriptor只允许name、writes、execution_revision、
parallel_safe、side_effect_free；后两项默认为false，版本默认1。回调必须调用
result_set一次，输出为对象，字段patch_json/route/status/error；patch_json是JSON文本
字符串，默认{}，status默认completed。输出缺失、重复设置、未知字段、重复JSON键、
错误类型、非法UTF-8、超限均失败关闭，不把未返回结果当成成功。

run参数options只允许scope/max_parallel/max_node_calls/max_state_bytes/timeout_ms，
默认值与SDK一致。返回status、state_json（JSON文本字符串）、error、nodes（节点状态）、
node_calls、progress_available、journal_revision和共享task_contract。图完成不能推出
构建/测试通过或人工接受。返回字符串必须用libai_string_free释放。

回调输入、context、result和Invocation只活到回调返回；不得保留指针或抛异常。
context.execution_scope用于子Agent持久日志，Invocation的业务幂等键为空。
`libai_invocation_retain_cancellation`复制拥有句柄，共享父取消状态；有令牌时要用
libai_cancellation_destroy释放。`libai_invocation_remaining_ms`返回剩余期限，-1为无限、
0为到期；子Agent不能把0设置成无限期限，启动前应检查stopped。长期回调须协作停止。

默认在运行线程执行节点；max_parallel>1的同轮节点会在工作线程并发回调，宿主负责
所有捕获状态、FFI运行时和回调的线程安全。节点callback/user活到图销毁；传入release时
仅成功注册转移回收责任，销毁时调用一次，注册失败不调用。未传release则由宿主保活。
不得销毁仍被其它线程使用的句柄；运行内同图重入、修改和销毁立即拒绝。

持久化使用run的store/user同步回调，复用已有open/commit/close契约；宿主在调用返回
之前保活。图存储与子Agent存储使用不同lease owner。恢复默认不重跑效果未知的节点；
confirm_node只能由核对完整节点外部效果的宿主调用，匹配CAS revision与receipt reference。
传入none允许原输入重试；applied保存完整patch/route结果后继续，结果仍受图定义校验。
回调输出wire上限16MiB（容纳转义后的2MiB patch），不扩大SDK状态/增量/结构限制。
Java/Python/Rust/Objective-C的独立工作流对象尚待接入，本阶段只提供共享C入口。

本阶段主目录静态/动态各5项回归、两个仅使用安装包的C消费项目通过；完整JNI
(x86_64)与Rust原生库(arm64)运行同一工作流C测试通过。Java/Rust APP已重建，
Objective-C既有8项和Python既有21项回归通过。iOS arm64模拟器及Android arm64
API21 JNI交叉编译成功；不作为设备运行、Windows或断电验收证明。

## 可插拔 RAG C ABI

C ABI 提供独立 opaque 组件、JSON 值和回调；公共头文件不暴露 SDK 类型。
`libai_rag_create(kind, callback, user, release, config)` 的 kind 为 loader、splitter、
embedding、store、retriever。成功创建才接管 user 回收责任；最后一个组件、组合检索器
或 Agent 引用释放时调用 release 一次。失败创建不调用 release。

内置 `libai_rag_create_splitter` 配置 bytes/overlap（默认1024/128），
`libai_rag_create_memory_store` 配置 max_records/max_bytes（默认4096/16MiB）。
`libai_rag_create_vector_retriever(embedding, store, config)` 保留底层组件，外层输入
句柄之后可以销毁。所有 config 可含 limits 对象，字段及上限沿用 [RAG.md](../libai/docs/RAG.md)。
调用、配置和销毁不能并行；组件回调重入或销毁正在使用的自身句柄会被拒绝。
回调函数、JSON、builder、invocation 只在当前同步回调有效；API不允许异常穿过C边界。

```c
libai_rag *splitter = libai_rag_create_splitter("{\"bytes\":1024,\"overlap\":128}");
libai_rag *store = libai_rag_create_memory_store(NULL);
/* embedding 由宿主通过 libai_rag_create("embedding", ...) 提供。 */
const char *context = "{\"access_scope\":\"tenant_a\",\"timeout_ms\":300000}";
char *batch = libai_rag_prepare_index(splitter, embedding, documents_json,
                                    context, NULL, NULL);
if (batch) {
    /* 此前未写入 store；这里显式提交，错误/取消不能证明没有副作用。 */
    int rc = libai_rag_commit_index(store, batch, context, NULL);
    libai_string_free(batch);
    /* rc != 0 时由宿主核对存储收据，不自动重试。 */
}
libai_rag *retriever = libai_rag_create_vector_retriever(embedding, store, NULL);
char *hits = libai_rag_retrieve(retriever,
    "{\"query\":\"installation\",\"top_k\":3}", context, NULL);
if (hits) libai_string_free(hits);
/* 用毕 libai_rag_destroy 各句柄；NULL 可安全销毁。 */
```

各调用的返回字符串由 `libai_string_free` 释放；失败返回NULL/-1，诊断读取
`libai_last_error()`。Context 的 access_scope 必填；timeout_ms 默认300000、最大24小时、
0表示不限，idempotency_key 可选。取消/期限为协作式，不会强行中断宿主网络回调。
独立调用不是 Agent 工具，不自动提供授权、执行日志或 exactly-once 保证。

| callback operation | arguments_json | result JSON |
| --- | --- | --- |
| load | `{source:string}` | Document 数组 |
| split | Document 数组 | Chunk 数组 |
| space_id | `{}` | 非空 JSON 字符串，稳定标识向量语义 |
| embed | 文本字符串数组 | double 向量数组 |
| replace_documents | PreparedIndex | `{}`，确认本次回调正常返回 |
| search | `{request:Request,embedding_space:string,vector:number[]}` | RetrievalResult |
| retrieve | Request | RetrievalResult |

space_id 是纯配置读取，应快速返回、不得依赖隐含网络操作；其 context 为 `{}`、
invocation 为 NULL。其余回调 context 含宿主 access_scope，invocation 携带取消、剩余
期限及幂等键。`libai_rag_result_set(builder, wire, bytes)` 必须恰好调用一次，复制
完整 JSON；重复、缺失、损坏、重复键、未知字段、越界或错误类型均失败关闭。
`libai_rag_result_error` 返回非空错误（最多4096字节），即使先设成功结果也按错误处理。

Wire 类型：

- Document：id/source/revision/text，metadata_json 默认字符串 `"{}"`。
- Chunk：id/document_id/source/revision/text/metadata_json；has_span 默认 false。
  为 true 时必须有 begin/end（原文 UTF-8 字节位置），为 false 不允许 span 字段。
- Request：query、filter_json 默认字符串 `"{}"`、top_k 默认8。
- RetrievalResult：hits 数组、truncated 默认 false；Hit 为 `{chunk:Chunk,access_scope:string,score:number}`。
- PreparedIndex：documents（待替换文档ID数组）、records（`{chunk:Chunk,vector:number[]}`数组）、embedding_space。

prepare_index 的 limits_json 是限制对象（NULL默认）；准备结果可保存/审核后显式提交。
宿主可插入自己的存储/混合检索，SDK仍校验来源、范围、过滤及结果；宿主负责真实ACL、
外部存储原子性和来源真实性。JSON桥接包络硬上限128MiB/5,242,880节点/深度64，
以容纳SDK最大256×16384的向量批次；metadata/filter/model/Schema继续使用原有更紧
限制，没有放宽模型输入边界。数组、文档、向量及总文本在转换与核心执行中另行校验。

`libai_agent_add_retrieval_tool(agent, retriever, name, binding_json)` 保留检索器及依赖；
binding 必填 execution_revision、access_scope；可选 filter_json、requires_approval、
side_effect_free、max_top_k、max_result_bytes、limits。默认要求授权、不允许安全重放。
模型只能提交query/top_k，不能覆盖租户/过滤；正常工具Schema、授权、预算、取消、
事件与执行日志继续生效。Agent输出含citation/source/revision等引用，按完整命中截断。
检索结束不自动证明任务构建、测试或人工接受通过。测试见 `tests/rag_test.c`。

本阶段主目录验证：静态/动态库各6项、仅使用安装包的两个独立C消费项目通过；
同一RAG用例在实际Java/JNI（x86_64）和Rust APP（arm64）原生库通过。三个APP
重建成功，完整SDK20项、C++封装11项、Objective-C9项通过；Android API21 JNI
和iOS模拟器交叉编译成功。尚未验证设备/Windows运行，其它语言RAG对象仍待接入。
