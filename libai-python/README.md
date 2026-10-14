# Python binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
先准备 Python 3.9+、pip、setuptools>=61 和 wheel（`python3 -m pip install -U setuptools wheel`）。
设置 `PYTHON` 选择解释器；脚本离线构建 C ABI 动态库、Python wheel 及可直接导入的包。
wheel 为纯 Python 包，安装后仍需设置 `LIBAI_LIBRARY` 指向 `build/native/install/lib/` 中的动态库。

Python 3.9+，使用标准库 ctypes，模型请求及运行结果为 Python 字典。
先构建上一级 `libai-c`；Python 包不会自动安装或下载原生库。

```sh
python3 -m pip install ./libai-python
export LIBAI_LIBRARY="$PWD/build/libai-c/libai_c.dylib" # Linux 改为 .so；Windows 设置 DLL 绝对路径
PYTHONPATH=libai-python python3 -m unittest discover -s libai-python/tests -v
```

```python
from libai import Agent, ModelResponse, ToolCall

def complete(request, invocation):
    # 此处调用应用自己的模型客户端，将模型输出转换成 ModelResponse。
    return ModelResponse(text="你好，" + request["messages"][-1]["text"])

with Agent(complete, instructions="你是编程助手") as agent:
    agent.add_tool("echo", lambda call, ctx: call.arguments["text"],
                   parameters={"type": "object", "properties": {"text": {"type": "string"}}})
    result = agent.run("介绍一下接口", max_model_calls=8)
    print(result["status"], result["text"])
```

也可向 `Agent(..., library="/absolute/path/to/library")` 指定共享库。
用上下文管理器或 `close()` 显式释放 native agent；reset 清空会话，cancel 可从另一线程调用。
模型/工具异常被转成原生错误；观察者异常取消运行并在 run 返回后抛出 RuntimeError。

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

`CodingService(root, library=...).available / .call(operation, arguments_dict)` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。


## MCP 工具接入

`McpClient` / `McpTransport` 使用同一C ABI协议实现，支持初始化、发现、直接工具调用、
Agent绑定以及POSIX stdio。Python仅负责类型/回调转换，不重新实现协议或恢复规则。

```python
import sys
from libai import Agent, McpClient

# my_model 与 approve_tool 由宿主实现。
with McpClient.stdio(sys.executable, ["/absolute/path/to/server.py"]) as client:
    client.initialize("my-app", "1.0")
    tools = client.list_tools()
    with Agent(my_model) as agent:
        agent.add_mcp_tool(client, "search", "mcp_search",
            parameters={"type": "object", "required": ["query"],
                        "properties": {"query": {"type": "string"}},
                        "additionalProperties": False},
            execution_revision="host-reviewed-search-v1")
        result = agent.run("搜索项目文档", authorize=approve_tool)
```

自定义传输实现request(message_dict, context_dict, invocation)、notify及close。
request返回完整JSON-RPC字典或原始JSON文本；context包含request_id/protocol_version，
invocation提供stopped()/idempotency_key()且仅在当前回调线程和回调期间有效。
错误转换为原生传输错误，不允许异常穿越ctypes。close必须可靠释放资源且不抛异常。

默认绑定requires_approval=True、side_effect_free=False；必填宿主审查过的parameters
及execution_revision，远程outputSchema存在时还需output_schema。本地规则使用受限
Draft-04，不静默转换远程现代Schema。直接call_tool(name, arguments)返回result对象与
error布尔值，属于宿主显式调用，不获得Agent的授权、预算或执行日志保证。

initialize/list_tools/call_tool接受timeout_ms与Cancellation；call_tool另可传幂等键。
同一连接不允许并行或重入；Agent配置必须空闲。绑定要求与Agent使用同一原生库。
客户端context manager退出或dispose()只释放外层句柄，已绑定Agent仍持有连接；
close()显式关闭所有共享绑定。最后一个连接/绑定释放才回收传输状态和回调所有权。
应显式关闭Agent；不要依赖解释器退出清理尚在使用的原生回调。

stdio_available()查询当前编译平台能力；iOS/Windows暂不可用，Android仅面向API28+
构建时包含stdio。stdio的环境默认继承；inherit_environment=False与environment字典
可显式替换整个环境。宿主实现的其它传输可用于不支持stdio的平台；内置HTTP待完成。
旧原生库仍能使用原有Agent接口；缺少新MCP符号时会明确要求重建libai-c。
详见 [MCP边界](../libai/docs/MCP.md)。


本阶段主目录21项回归通过。离线wheel包含mcp.py，解包后仅从wheel目录导入，
使用同次打包的轻量原生库再次运行21项回归通过。系统Python缺少打包依赖，
此次使用已安装的Python3.12/setuptools/wheel环境，没有联网安装或修改系统Python。
外层句柄直接调用期间dispose拒绝并行/重入；Agent直接持有底层连接，释放外层
句柄不关闭正在使用的绑定。仍须遵守Agent自己的运行/关闭同步要求。

## 工作流

`Workflow`、`WorkflowNodeResult`、`WorkflowContext`封装同一C ABI图执行器，支持JSON
状态、条件分支、显式并行、合流、持久恢复与宿主收据。需配套包含工作流入口的
新原生库；旧原生库的既有Agent仍可使用，构造Workflow会明确报告缺少ABI。

```python
from libai import Workflow, WorkflowNodeResult

with Workflow() as graph:
    graph.add_node("choose", lambda state, ctx: WorkflowNodeResult(route="yes"))
    graph.add_node("answer", lambda state, ctx: WorkflowNodeResult(patch={"answer": 42}),
                   writes=["answer"])
    graph.add_edge("choose", "answer", "yes")
    result = graph.run({})
    assert result["status"] == "completed"
    # result["state_json"] is JSON text; task_contract["accepted"] remains False.
```

节点回调接收独立解析的dict状态与借用context，必须同步返回WorkflowNodeResult。
patch可为dict或原始JSON对象文本，route/status/error为字符串；写入需在writes声明。
返回None、错误类型、无效JSON/UTF-8、重复键或超限输出失败，不当作成功。修改输入
dict不会更新图，只有返回的patch参加原子合并；未知副作用默认停止等待宿主确认。

run选项scope、execution_store、cancellation、max_parallel、max_node_calls、
max_state_bytes、timeout_ms与SDK含义一致，数值选项拒绝bool与非整数。
结果为dict，包含status/state_json/error/nodes/node_calls/progress_available/
journal_revision/task_contract。限制与恢复规则见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

默认在调用线程执行；max_parallel>1时同轮节点须声明parallel_safe=True且writes不冲突，
在原生工作线程回调Python。线程会获取GIL，并发适合等待I/O，不承诺CPU密集Python
加速。宿主负责所有捕获状态的线程安全；同图重入、并发运行/修改/关闭立即拒绝。

context.stopped()/remaining_ms()/retain_cancellation()只在原回调线程和回调内有效。
remaining_ms为-1无限、0到期；复制的取消令牌独立拥有，需close/context manager释放。
context.execution_scope是子Agent日志范围，可复制字符串，不是节点统一业务幂等键。
子Agent可通过下列模式共享停止状态和剩余期限；使用同一原生库与独立存储owner：

```python
def agent_node(state, context):
    with Agent(model) as agent:  # model/tool/store are application-owned adapters.
        agent.add_tool("lookup", lookup, side_effect_free=True)
        agent.set_execution_store(context.execution_scope, child_store)
        with context.agent_options() as options:
            result = agent.run(json.dumps(state), **options)
        if result["status"] != "completed":
            return WorkflowNodeResult(status=result["status"], error=result["error"])
        return WorkflowNodeResult(patch=result["text"])
```

该片段另需导入Agent与json；模型最终text应为符合节点writes的JSON对象，或由宿主
映射为patch。agent_options退出后释放复制令牌，不能在退出后继续使用options；到期
时拒绝启动，不把0变成无限期。取消仍是协作式，无法强制停止任意阻塞Python代码。

持久化示例：`graph.run({}, scope="task1", execution_store=store)`；相同任务恢复时沿用
相同scope、初始状态及图定义/执行版本。`graph.confirm_node(store, scope, initial,
revision, node, "applied", output=WorkflowNodeResult(patch=...), reference="receipt")`
仅由验证完整节点效果的宿主调用，CAS/写入/路由仍由原生引擎校验。none确认无效果
后允许原输入重试，applied确认完整结果后不重跑。不要把确认接口暴露为模型工具。

图保留handler至close；失败注册释放本次引用。使用context manager或显式close，在
Python退出前关闭所有图。若handler捕获graph形成引用环，不能依赖垃圾回收关闭。
FFI回调/释放trampoline由模块持有，context离开回调即失效。共享存储异常转换有界，
异常自身__str__出错也向原生层报告失败，避免把失败open当作空快照继续执行。

主目录完整原生库28项回归、仅从离线wheel加载封装并使用配套轻量原生库的28项
回归均通过。涵盖实际并行/分支/合流、停止与借用过期、持久收据/提交失败、子Agent
缓存及真实进程退出不重写。旧原生库Agent兼容与工作流缺少ABI诊断通过；未验证
本阶段Windows或移动设备运行。

## 可插拔 RAG

公开 `Document/DocumentChunk/RetrievalRequest/RetrievalHit/RetrievalResult`、
`VectorRecord/VectorQuery/PreparedIndex`、`RagLimits/RagOptions/RagBinding` 独立Python
类型，宿主接口为 `DocumentLoader/TextSplitter/EmbeddingModel/VectorStore/Retriever`。
`Rag.loader/splitter/embedding/vector_store/retriever` 创建拥有原生组件的适配器；
也支持 `Rag.utf8_splitter/memory_vector_store/vector_retriever` 内置组合。
没有默认Embedding厂商或数据库依赖，可接全文、SQL、向量或混合检索后端。

```python
from libai import (Rag, RagOptions, Document, EmbeddingModel, RetrievalRequest)

class ExampleEmbedding(EmbeddingModel):
    def space_id(self):
        return "example-constant-vector-v1"
    def embed(self, texts, context):
        # 仅演示离线接口；实际使用替换为自己的Embedding服务。
        # 网络请求须配合context.stopped()/remaining_ms()，不是强行中断。
        return [[1.0, 0.0] for _ in texts]

options = RagOptions("tenant_a", timeout_ms=300000, idempotency_key="host-index-1")
with Rag.embedding(ExampleEmbedding()) as embedding, \
     Rag.utf8_splitter(bytes=1024, overlap=128) as splitter, \
     Rag.memory_vector_store() as store:
    prepared = splitter.prepare_index(embedding,
        [Document("manual", "manual.txt", "r1", "Installation instructions", {"kind": "manual"})],
        options)
    # prepare不写入store；可先保存prepared.to_json()或审核，再显式提交。
    store.commit_index(prepared, options)
    with Rag.vector_retriever(embedding, store) as retriever:
        result = retriever.retrieve(RetrievalRequest("installation", {"kind": "manual"}, 3), options)
        for hit in result.hits:
            print(hit.chunk.source, hit.chunk.revision, hit.chunk.text)
```

自定义宿主接口返回上述Python类型：loader.load返回Document数组，splitter.split返回
Chunk数组，embedding.embed返回向量数组，store.search接收VectorQuery并返回
RetrievalResult，retriever.search接收RetrievalRequest并返回RetrievalResult。
store.replace_documents接收文档ID数组、VectorRecord数组、embedding_space及context，
正常返回None；异常/取消不能证明未写入，不自动重试。space_id须为纯、快速、稳定的
字符串配置读取，不进行隐含网络操作；模型/归一化/维数语义改变须更新它。
回调必须同步，async函数或返回awaitable被拒绝。

metadata/filter为JSON对象，也接受原始JSON对象文本，由共享SDK检查。Chunk.span是
原文UTF-8字节位置元组；prepare验证来源、版本、metadata和声明跨度，commit再次
验证批次。PreparedIndex.from_json/to_json可保存准备结果；原始JSON也可提交，重复键、
未知字段、越界、无效向量或错误来源范围均失败关闭。RagLimits只能收紧SDK硬上限。
内存库是临时精确cosine小索引，不是持久数据库；真实权限和外部存储原子性由宿主负责。

```python
from libai import RagBinding
agent.add_retrieval_tool(retriever, "knowledge_search",
    RagBinding("corpus-v1-acl-v1-embedding-v1", "tenant_a", {"kind": "manual"}))
# agent.run(..., authorize=宿主审批函数)
```

Agent绑定冻结scope/filter/revision。模型只有query/top_k参数，不能选择租户/过滤；
默认requires_approval=True、side_effect_free=False，沿用工具Schema、授权、预算、
事件、取消、日志和宿主收据规则。结果含citation/source/revision，按完整命中截断；
检索完成不改变构建、测试或人工验收证据。execution_revision必须覆盖语料、ACL、
后端、Embedding及过滤语义，不由模型或远端声明替代宿主审核。

回调 `RetrievalContext` 是借用对象，只能在当前回调线程及返回前使用。支持stopped、
remaining_ms、idempotency_key和retain_cancellation；access_scope可复制保存。
`with context.operation_options() as options` 可供不同组件的嵌套调用，保留同一停止
令牌并在每次调用前重新读取父期限；过期不能转为不限时。协作期限无法强行中断任意
宿主回调，子调用仍须配合停止；令牌在上下文退出时关闭，不能把options留到之后使用。

组合检索器与Agent保留底层组件/宿主；关闭外层输入句柄后已绑定的Agent仍能使用。
组件用上下文管理器或显式close，失败创建不转移宿主；回调重入/活跃关闭被拒绝，
调用方取消令牌在运行中关闭不会释放已保留的停止状态。Agent释放原生回调与宿主对象
时不持有Python门面锁，宿主析构重入不会因此死锁。库路径须一致，旧原生库仍可用
既有Agent接口；创建RAG组件时会明确提示缺少RAG ABI。协议及边界见
[RAG.md](../libai/docs/RAG.md)和[C ABI说明](../libai-c/README.md#可插拔-rag-c-abi)。

本阶段主目录完整/轻量原生库各37项回归通过（含9项RAG测试）；离线wheel安装到
独立目录后，配套轻量库运行全部37项通过。覆盖全部宿主组件、来源与访问校验、
最大维度、GC/析构重入、取消/期限、JSON保存及持久恢复。旧原生库既有Agent接口
兼容、缺少RAG ABI的明确诊断通过；尚未验证设备或Windows运行。


## 独立任务契约与证据类型

`TaskContract`、`TaskEvidence`、`TaskRequirements`、`TaskChecks`和`TaskEvaluation`提供
Python值类型。`evaluate()`始终调用轻量核心的tasks.evaluate；不复制接受规则，不需要
完整编程后端、项目目录或模型服务。可用`library=`指定匹配的原生库路径。

```python
from libai import TaskContract

contract = TaskContract(loop_finished=True, loop_outcome="completed", artifact_revision="revision-1")
# Only record these after the host has verified actual reports and the human decision.
contract.record_generation("revision-1", "draft-index")
contract.record_build("revision-1", "compiler-report")
contract.record_tests("revision-1", "test-report")
contract.record_review("revision-1", "human-review")
result = contract.evaluate()
assert result.accepted
assert result.checks.build and result.checks.tests
assert not result.unmet
```

四项要求默认均为true，证据默认unknown。`record_build/tests(passed=False)`记录失败；
`record_review(accepted=False)`记录拒绝，开关只接受真正bool。方法不执行编译/测试，
不打开引用报告或核实身份，不能作为模型可写证据工具。成果版本变化后，旧证据保留
但不再有效；generation/build/tests仅接受host来源，review仅接受human来源。

`TaskContract.from_run_result(agent.run(...))`读取原生运行结果，不从模型文本推断证据。
`from_json/from_dict`校验后建立独立对象，忽略accepted/checks等派生注解；raw JSON先
交给核心校验再解码，重复键、非法类型/版本/NUL不会被转换抹掉。`to_dict/to_json`只
序列化事实，评估需显式调用evaluate。没有任务契约的旧结果不会自动升级为可信状态。

证据、checks和评估结果不可变；契约与requirements由宿主维护。不同实例不共享默认
证据或要求。旧原生库既有Agent仍可用，调用本接口会明确提示缺少契约评估支持，
不会回退到另一套Python接受逻辑。其它语言专用证据类型仍在补齐中。
