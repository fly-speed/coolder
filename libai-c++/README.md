# libai-c++：第三方 C++ APP 接口

本目录与 `libai` 同级，提供可安装的 C++11 库和拥有独立会话的 `libai::cpp::Agent`。
应用只需包含 `<libai_cpp/agent.h>`，链接 `libai::cpp`，无需 ACL、OpenSSL 或第三方 JSON 库。
公开头文件只使用封装自身定义的类型和标准库，不包含或导出原始 `libai` 类型。
`Message`、`ToolCall`、`ModelRequest`、`ModelResponse`、`Model`、`RunOptions` 等均独立定义，
`Agent`、`Cancellation` 的底层状态通过 Pimpl 隐藏。实现层转换请求、回调和结果，复用 agent 循环。
模型网络客户端和具体业务工具由应用提供。

## 构建及清理

```sh
./build.sh
./build.sh /absolute/output/path
./build.sh /absolute/shared/output -DBUILD_SHARED_LIBS=ON
# 不指定输出目录也可直接传参数：
./build.sh -DBUILD_SHARED_LIBS=ON
./clean.sh
./clean.sh /absolute/output/path
```

可以从任意工作目录调用脚本。默认安装产物位于本目录 `build/install/`，
默认只生成静态库：macOS/Linux 为 `libai_cpp.a`，Windows 为 `ai_cpp.lib`。
显式传入 `-DBUILD_SHARED_LIBS=ON` 才生成动态库：macOS `libai_cpp.dylib`、
Linux `libai_cpp.so`、Windows `ai_cpp.dll`（以及导入库）。每次构建只选择一种库类型。
直接使用 CMake 时同样默认静态库；`cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON`
启用动态库。`build.sh` 每次默认指定 OFF，再由额外参数覆盖，避免沿用旧的动态库缓存。
支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量和额外 CMake 参数。
`clean.sh` 只删除对应 `cmake/`、`install/`，保留输出目录中的其他文件。
构建依赖 CMake 3.16+、C++11 编译器和线程运行库。可使用系统 CMake 命令在 Windows 构建。

## 第三方 APP 集成

在应用的 CMakeLists.txt 中：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_agent_app LANGUAGES CXX)
find_package(libai_cpp 1 CONFIG REQUIRED)
add_executable(my_agent_app main.cpp)
target_link_libraries(my_agent_app PRIVATE libai::cpp)
```

配置时传入 `-DCMAKE_PREFIX_PATH=/absolute/path/to/libai-c++/build/install`。
也可源码集成：`add_subdirectory(/path/to/coolder/libai-c++ libai-cpp-build)`，
然后链接相同目标。应用直接使用安装后的 include/lib/cmake 目录即可，不需要整个 coolder 仓库。
分发 C++ 二进制库时，应用和库需要使用兼容的编译器、C++ ABI、标准库及运行库设置。
动态库部署到应用的运行时搜索路径；Windows 同时分发 DLL，静态链接由 CMake 携带线程依赖。

```cpp
#include <libai_cpp/agent.h>
#include <iostream>

int main()
{
    using namespace libai::cpp;
    Agent app([](const ModelRequest &request, const StreamSink &sink) {
        ModelResponse response;
        // 在这里调用应用的模型客户端，转换回答及 ToolCall 到 response。
        response.text = "你好，" + request.messages.back().text;
        if (sink) sink(response.text);
        return response;
    }, "你是应用的 AI 助手");
    const auto result = app.run("介绍你的功能");
    if (result.status != RunStatus::completed) return 1;
    std::cout << result.text << '\n';
}
```

完整工具循环示例见 `examples/main.cpp`。安装后的 `share/libai_cpp/examples` 是独立消费项目：

```sh
cmake -S build/install/share/libai_cpp/examples -B build/app-example \
    -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/app-example
```

## 接口及运行语义

- `Agent(ModelFunction, instructions)` 直接接收应用回调，也可传 `std::shared_ptr<Model>` 子类。
- `add_tool(ToolSpec, ToolHandler)` 注册唯一能力。工具的 arguments/schema 为 JSON 文本，SDK 在执行前校验整批参数；应用负责业务规则。独立 `<libai_cpp/governance.h>` 提供 Schema 验证及治理能力查询。
- `run(input, RunOptions)` 返回类型化状态、回答、错误和调用/token 统计；自动保留多轮会话。
- `RunOptions` 设置调用、token、上下文、工具结果预算、重试、截止时间、授权、流式输出及事件。
- `messages()` 复制空闲会话，`reset()` 清空会话，`cancel()` 请求当前运行协作停止。

需要审批的工具在没有 authorize 时默认拒绝。工具错误会作为消息交给模型，模型可以继续处理。
模型、工具及观察者异常由 SDK 转成相应运行状态；无效构造/配置或并行状态访问抛标准异常。
同步 run 应放在应用工作线程；回调也在该线程执行。同一实例不允许并行或回调重入
run/add_tool/reset/messages，跨线程 cancel 可用。共享同一个 Model 的多个 Agent 需要模型自行保证线程安全。
模型和长工具检查 cancellation/deadline，自行设置网络超时；取消不会强制终止应用回调。
若需要在 run 开始前也能可靠请求取消，在 RunOptions 中传入应用持有的 `shared_ptr<Cancellation>`。
Agent 不可复制/移动；RAII 析构前必须等待正在执行的 run/cancel 及相关应用线程结束。

## 验证

```sh
./build.sh build/test -DLIBAI_CPP_BUILD_TESTS=ON
ctest --test-dir build/test/cmake --output-on-failure
```

测试覆盖工具循环、Unicode、流式输出、会话/重置、默认权限拒绝、预算、异常、
并行访问、回调重入和跨线程取消。默认构建封装 portable SDK。启用完整组件后还包含编码基础设施。

## 完整编码组件

```sh
./build.sh build/full -DLIBAI_CPP_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl
```

`<libai_cpp/coding.h>` 是轻量入口，只引入 `CodingService` 和完整构建中的
`NativeProviderModel`。Provider 使用独立的 `ProviderConfig` 端点配置，模型继承
公开的 `Model` 接口；原生请求、存储记录、工作区、沙箱、运行时和 ACL 类型均不公开。
公共头文件只依赖标准库，不需要 ACL/OpenSSL 或原始 libai 的头文件。

```cpp
#include <libai_cpp/coding.h>
libai::cpp::ProviderConfig config;
config.protocol = "openai_chat";
config.base_url = "https://example.com/v1";
config.model = "your-model";
auto model = std::make_shared<libai::cpp::NativeProviderModel>(config, api_key);
libai::cpp::Agent agent(model);
```

所有构建均提供 `<libai_cpp/coding_service.h>` 的 `CodingService(root)`；
`available()` 检测完整组件，`call(operation, arguments_json)` 返回 UTF-8 JSON。
完整编码组件要求桌面 ACL/OpenSSL；默认轻量构建无这些依赖。Windows 完整类型化封装
使用 `-DBUILD_SHARED_LIBS=OFF`，VS 工程同样生成静态库。

```cpp
#include <libai_cpp/coding_service.h>
libai::cpp::CodingService service("/absolute/application-workspace");
if (service.available()) {
    auto result = service.call("workspace.read", R"({"path":"src/main.cpp"})");
}
```

安装后继续使用 `find_package(libai_cpp CONFIG REQUIRED)` 和 `libai::cpp`。
安装包只需要本绑定的头文件、库和 CMake 元数据，不要求原始 libai 头文件或其 CMake 包。
完整静态库的独立消费者设置 `ACL_PATH` / `OPENSSL_PATH` 或 `OPENSSL_ROOT_DIR`，
以定位对应架构的链接依赖；公共头文件无需这些库的头文件。
统一操作协议、写入授权与各语言示例见 [CODING_SERVICE.md](../libai/docs/CODING_SERVICE.md)。

## 公开接口与内部实现

`include/libai_cpp/` 根目录保留独立公共接口，包括 Agent、执行存储、治理、MCP、
Provider 与 CodingService；内部转换和后端类型放在 `src/`。
第三方编码能力通过 `CodingService::call()` 使用，操作协议包含工作区、项目、会话、
沙箱、Provider 和运行记录，无需包含内部头文件。

`src/` 仅保存封装实现、私有值转换及宿主兼容适配，不保存 libai 模块的头文件副本。
完整构建通过 `libai/cmake/CodingBackend.cmake` 直接编译 libai 的原始源码；
同一 CMake 构建中的 C/C++/JNI/Objective-C 绑定共用 `libai_coding_objects`。
库中嵌入这些对象，安装包不要求消费者安装原始 libai 头文件或 CMake 包。

coolder 是同仓库的服务端宿主，显式链接只在源码构建中提供的 `libai::cpp_host`，
使用 `src/coding_host.h` 与 `src/platform.h` 接入内部工作线程及 HTTP 状态展示。
这个目标不导出或安装；第三方 APP 只链接公开的 `libai::cpp`。
宿主兼容层仍用于 coolder 的内部工作线程与状态展示，不视为稳定的第三方 SDK。
ACL/OpenSSL 的编译定义和头文件路径只给实现与内部宿主使用；静态库的链接依赖
由 CMake 携带，动态库安装包不要求消费者配置 ACL/OpenSSL 的开发包。

`libai_cpp_internal_headers` 检查无后端副本及公开头文件隔离，`libai_cpp_types` 检查类型独立性，
`tests/consumer` 只用安装后的公共头文件验证 C++11 第三方集成。
VS 工程直接编译同一份 libai 原始源码，不生成改名的实现副本。


`Agent::configure_output(schema, repairs)` 可要求最终回答满足 JSON Schema，并在限制内
提供修正反馈。修正阶段不允许工具调用；结果通过后才发布完整 JSON。详见
[结构化输出边界](../libai/docs/STRUCTURED_OUTPUT.md)。


## MCP 工具接入

`<libai_cpp/mcp.h>` 定义独立的 `McpClient`、`McpTransport`、`McpTool`、
`McpOptions` 和 `McpBinding`，不引用 libai 头文件。轻量静态/动态库都包含此能力。

```cpp
#include <libai_cpp/agent.h>
#include <libai_cpp/mcp.h>
using namespace libai::cpp;
McpStdioOptions options;
options.executable = "/absolute/path/to/python3";
options.arguments = {"/absolute/path/to/mcp_server.py"};
auto client = McpClient::stdio(options);
client->initialize("my-app", "1.0");
auto tools = client->list_tools();
McpBinding policy;
policy.parameters_schema = R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})";
policy.execution_revision = "host-reviewed-search-v1";
// agent 是绑定了应用模型的 Agent，run 时仍须提供授权回调。
agent.add_mcp_tool(client, "search", "mcp_search", policy);
```

也可实现 `McpTransport` 注入自定义传输；回调使用独立 `McpRequestContext`。
传输必须观察 control 的取消/期限；HTTP 协议版本、认证、会话及 framing 由宿主负责。
stdio 工厂目前支持 macOS/Linux 等桌面 POSIX，iOS/Windows 返回不可用；以
`stdio_available()` 查询。iOS 可通过宿主提供的其它传输使用 MCP 客户端。

Agent 绑定默认需要授权，不信任服务端 readOnlyHint，必须提供本地受限Draft-04
Schema及宿主执行版本。远程现代Schema不被静默转换。直接 `call_tool` 是宿主显式
调用，不经过Agent授权/日志规则；需要这些规则时使用 `Agent::add_mcp_tool`。

客户端操作不允许并行/重入，绑定配置要求Agent空闲。Agent持有绑定的底层连接；
释放外层client不会使绑定悬空，最后一个绑定/客户端释放后关闭连接。显式close会
使所有共享绑定停止可用。销毁/close不得与请求并行，取消可从其它线程触发。
失败连接不自动恢复；宿主可建立新连接，但效果未知的持久写操作仍需核对收据，
不会因为重连而重跑。更多协议、存储和平台边界见 [MCP.md](../libai/docs/MCP.md)。


## 独立工作流接口

`<libai_cpp/workflow.h>` 定义Workflow、WorkflowNode、WorkflowContext、WorkflowOptions、
WorkflowNodeResult与WorkflowResult。公共类型不包含libai头文件，运行时、存储与取消
转换放在src/workflow.cpp；轻量静态/动态包都可使用，无需完整编程后端。

```cpp
#include <libai_cpp/workflow.h>
using namespace libai::cpp;
Workflow graph;
WorkflowNode step;
step.name="answer";step.writes={"answer"};
step.execute=[](const std::string &,const WorkflowContext &){
    WorkflowNodeResult result;result.patch_json=R"({"answer":42})";return result;
};
graph.add_node(step);
auto result=graph.run("{}");
// result.status是图循环结果，不代表构建/测试/人工验收。
```

add_edge(from,to,label)支持条件分支，空label表示无条件。显式max_parallel>1且节点
parallel_safe=true才在工作线程并行；同轮写入冲突在回调前拒绝，合流只提交完整有效
增量。context.execution_scope可用于节点Agent的执行日志，不是整节点所有业务操作
共用的幂等键；context.control提供取消/期限，用户需转发给自己的Agent/传输。

设置options.execution_store及稳定scope启用图级日志。与子Agent使用不同存储owner，
可共享物理目录；定义/输入漂移、未知效果、损坏或提交失败不会自动重跑。
confirm_node提供受信任宿主none/applied收据确认，匹配日志revision、定义及节点；
模型不能直接调用它。副作用声明默认false，已知节点/终态复用，预算只收紧。
结果progress_available=false表示进度无法证明，此时不得把空状态和零计数当作事实。

同一图的配置/运行/确认不允许并行或重入，销毁必须等调用结束。取消句柄可从其它
线程触发；绑定控制对象共享原生取消状态，不依赖包装对象的指针身份。
完整执行/存储/容量边界见 [WORKFLOW.md](../libai/docs/WORKFLOW.md)。

## 独立 RAG 接口

`<libai_cpp/rag.h>`提供Document、DocumentChunk、RetrievalContext/Request/Hit/Result、
DocumentLoader、TextSplitter、EmbeddingModel、VectorStore、Retriever及Rag静态操作。
所有公共类型独立声明，转换与SDK依赖仅在src；无数据库或模型厂商依赖。

```cpp
using namespace libai::cpp;
RetrievalContext context;
context.access_scope="tenant-a";
auto documents=Rag::load_documents(loader,"manual-source",context);
Utf8TextSplitter splitter;
auto prepared=Rag::prepare_index(documents,splitter,*embedding,context);
Rag::commit_index(*store,prepared,context);
VectorRetriever retriever(embedding,store);
RetrievalRequest query;query.query="How to restart?";
auto result=Rag::retrieve(retriever,query,context);
```

loader/embedding/store由宿主实现或提供；可选MemoryVectorStore为有界临时cosine索引。
FunctionRetriever可以直接适配SQL、全文或业务检索。prepare_index只准备批次，commit_index
是显式外部副作用，适配器负责持久原子性/去重/收据；失败不证明没有写入。

`Agent::add_retrieval_tool(shared_ptr<Retriever>, name, RagBinding)`注册受治理检索工具。
绑定保留Retriever与其捕获资源；默认需要审批且不声明纯读取。RagBinding的scope、
filter、execution_revision由宿主配置，模型只有query/top_k参数。版本必须覆盖语料、
权限、模型、后端和过滤变化，不能以同版本绕过未知效果确认。结果保留引用/来源/
版本，按整条命中截断，沿用Agent预算、授权、事件与持久日志。

公共Context复用独立ToolContext/Cancellation，转换保留相同原子取消状态、期限与
幂等键，不通过复制成新令牌丢失父停止状态。宿主捕获资源须满足自己的并发规则。
适配器输入/输出转换前有硬大小检查，实际校验继续采用RagLimits。同步静态操作
只借用loader/splitter/model/store至返回；VectorRetriever拥有EmbeddingModel/VectorStore。
不提供SDK类型别名、对象布局假设或把实现头文件安装到公开目录。

详细来源、UTF-8/span、scope/filter、维数、容量、索引替换及停止边界见
[RAG.md](../libai/docs/RAG.md)。结构和引用正确不证明业务事实或任务验收。

本阶段主目录静态/动态库各11项回归通过，含公开头文件隔离。两个仅用安装包
find_package(libai_cpp)的独立消费项目均通过索引/检索/Agent/治理/缓存/取消测试。
coolder重建与iOS arm64模拟器交叉编译成功，未验证设备或Windows运行。
