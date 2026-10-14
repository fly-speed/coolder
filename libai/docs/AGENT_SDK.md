# 通用 C++ Agent SDK

## 快速开始

SDK 要求 C++11，入口 `<libai/agent.h>`，CMake 目标 `libai::agent`。
它使用标准库及内置的私有 Schema 验证器，无需额外安装 JSON 库，可运行在 CLI、桌面程序、服务或嵌入应用中。
完整库入口与原编程运行时保持兼容；链接 `libai::ai` 可使用原生 HTTP provider 和工作区能力。

```cpp
#include <libai/agent.h>

auto model = std::make_shared<libai::function_model_t>(
    [](const libai::model_request_t &request, const libai::stream_sink_t &sink) {
        // 在这里调用自己的模型服务，将响应映射为文本、工具调用和用量。
        // 工具调用必须含唯一 id、name 与完整 JSON object arguments。
        libai::model_response_t reply;
        reply.text = "Hello";
        return reply;
    });
libai::agent_t agent(model, "Help the user with their application.");
libai::tool_spec_t tool;
tool.name = "app.status";
tool.description = "Get the current application status";
agent.add_tool(tool, [](const libai::tool_call_t &, const libai::tool_context_t &) {
    return libai::tool_result_t::success("ready");
});
libai::agent_session_t session;
auto result = agent.run(session, "Check the application");
if (result.status == libai::run_status_t::completed) {
    // 显示 result.text；重复使用 session 保留多轮上下文。
}
```

可执行的离线工具循环见 `examples/agent/main.cpp`。安装 SDK 后：

```sh
cmake -S libai/examples/agent -B example-build -DCMAKE_PREFIX_PATH=/absolute/path/to/libai-sdk
cmake --build example-build
./example-build/libai_agent_example
```

## 直接使用已有模型协议

```cpp
#include <libai/sdk/provider_model.h>
#include <libai/sdk/workspace_tools.h>

libai::ai::provider_config_t config;
config.protocol = "openai_chat"; // 也支持已有 Responses、Anthropic、Gemini、Ollama 适配器
config.base_url = endpoint_from_application;
config.model = model_from_application;
auto model = std::make_shared<libai::provider_model_t>(config, key_from_secret_store);
libai::agent_t agent(model, "Inspect the project and explain your findings.");
libai::add_workspace_tools(agent, project_root); // read / list / search
libai::agent_session_t session;
auto result = agent.run(session, "Explain this project");
```

本例使用默认完整包并链接 `libai::ai`。宿主负责已有 ACL/fiber 初始化和线程生命周期；
`provider_model_t` 直接复用现有传输，不引入 vendor SDK。SDK 的 typed transcript 保留每一轮
user / assistant / tool 角色，传输按目标协议转换。工具 schema 支持完整 JSON object，
响应保留数值、布尔、数组及嵌套参数。模型推理文本单独保留以供需要 replay 的协议使用。
不同 provider 对 JSON Schema 关键字和推理字段的支持仍由其协议能力决定。
Gemini 自定义 schema 使用 `parametersJsonSchema`，对应其
[官方 FunctionDeclaration 接口](https://ai.google.dev/api/generate-content#FunctionDeclaration)。
SDK 不启用 provider background 或存储，也不使用 response ID 延续，完整会话由应用持有。

工具名称中的点会在 provider wire 上转换为下划线，响应按本次工具目录恢复原名称。
`app.search` 与 `app_search` 不可同时注册到同一 provider_model_t 请求；冲突会显式报错。
Gemini 等不提供调用 ID 的协议由适配器生成 ID。provider_model_t 保存凭证，不应写入日志或共享序列化状态。

## 自定义工具与编程接口

`tool_spec_t.parameters` 是 JSON Schema 字符串，默认接受无参数对象。例如：

```cpp
tool.parameters = R"({"type":"object","properties":{"limit":{"type":"integer"}},"required":["limit"]})";
tool.requires_approval = true;
```

工具处理器收到完整 `tool_call_t.arguments` JSON 字符串和 `tool_context_t`。
SDK 在注册时验证 Schema，执行前对整批参数做受限 Draft4 校验；任何调用不符合 Schema 时整批不执行，并返回错误供模型修复。工具仍负责业务权限、领域规则和执行时前置条件。详见 [工具治理](GOVERNANCE.md)。
可用它接入数据库、检索、业务接口、沙盒命令、其他 agent 或宿主插件。
同一模型轮的多个工具按顺序执行，避免隐式并发修改。
未注册工具、拒绝授权和工具异常转为 `tool_error` 结果返回模型。
模型重试只重试明确标记为 retryable 的模型失败，不重放已执行的工具。

`add_workspace_tools(agent, root, true)` 额外注册 `workspace.create`、`workspace.replace`。
写入工具默认要求 `run_options_t.authorize` 授权；未提供回调时拒绝执行。
创建不覆盖现有文件，替换要求 `expected_sha256` 与当前文件一致。
`workspace.read` 返回内容、截断标志和完整读取时的 SHA-256；截断读取不返回修改基线。
目录和文件路径相对于宿主指定根目录，使用现有路径隔离与敏感文件规则，关闭外部项目挂载。
此工具集提供单文件编辑；需要多文件事务、审核、构建修复和持久调度时使用原有
`coding.h`、`workspace_change_set_store_t` 及编程运行时。

## 授权、事件与取消

```cpp
libai::run_options_t options;
options.cancellation = std::make_shared<libai::cancellation_t>();
options.on_text = [](const std::string &delta) {
    // 追加到界面。返回 false 请求取消。
    return true;
};
options.authorize = [](const libai::tool_spec_t &spec, const libai::tool_call_t &call) {
    // 按宿主权限规则判断，可在 UI 中同步确认具体参数。
    return !spec.requires_approval;
};
options.on_event = [](const libai::agent_event_t &event) {
    // model_start/end、tool_start/end、finished。
};
auto result = agent.run(session, task, options);
// 另一个线程可以调用 options.cancellation->cancel()。
```

授权回调对所有工具调用生效。未提供回调时，仅自动允许无需审批的工具。
取消和期限属于协作式控制：自定义模型及长时间运行的工具需检查 cancellation_t / tool_context_t；
SDK 无法强制终止任意应用回调。provider_model_t 在传输观察点检查取消和期限，
无响应网络等待仍受已有 HTTP transport timeout 控制。
正在执行的工具可能已经产生效果，SDK 不回滚；未执行的同批调用会收到跳过结果，保持 transcript 完整。

事件和文本回调在运行线程执行。模型/文本回调异常返回 `callback_error`；
工具执行与授权异常返回工具错误。tool_start 事件异常阻止该工具执行并产生错误结果，
tool_end 事件异常被忽略以便完成整批调用。finished 事件异常会使结果成为 callback_error。
应用应让事件观察器保持轻量且不抛出异常。

## 预算、结果与状态所有权

默认上限为 16 次模型调用、64 次工具执行、32,768 个报告输出 token、每次 4,096 token、
1 MiB 请求上下文字节、64 KiB 单工具结果、5 分钟期限和每轮 1 次模型重试。
模型重试计入模型调用及用量预算；已有 provider 传输内部恢复仍按其原有策略执行。
token 预算依赖模型报告用量；缺少用量时仍受调用次数、字节和期限限制。
上下文字节是文本/schema 大小上限，并非精确 tokenizer 计数。超限返回 `budget_exceeded`，
SDK 不自动丢弃历史。工具结果超过字节上限会被截断并标为错误。

`run_result_t` 包含终态、文本、错误及调用/token 统计。终态包括 completed、cancelled、
deadline_exceeded、budget_exceeded、model_error、protocol_error、callback_error。
整批工具的次数与调用 ID 在执行前检查；预算不足或 ID 异常不会部分执行该批工具。

agent_session_t.messages 是应用拥有的内存会话，保留成功及部分执行的交换。可用自己的存储 codec
持久化角色、文本、推理、调用 ID、参数和工具错误；不要从中只保存显示文本。
失败后再次调用 run 会添加新用户轮次；SDK 不自动重新执行旧工具。
无需 HTTP 鉴权、用户存储目录或全局 agent registry 即可运行 SDK。
一个 agent_session_t、model_t 实例及其应用工具状态不能未经同步地同时用于多个运行。
不同 agent 可使用独立会话与模型实例由宿主调度。

## 验证

`AI_CODING_BUILD_TESTS=ON` 启用通用循环测试。默认完整构建还验证五种协议的 typed transcript、
JSON schema 与嵌套参数、SSE 参数解析、工作区授权、摘要冲突、目录越界和符号链接隔离，
以及已有公共组件和完整归档链接测试。`AI_CODING_SDK_ONLY=ON` 无需构建第三方依赖，
只安装 portable SDK；provider_model_t 与 workspace_tools 属于完整包。
离线 fixtures 不等同于使用各厂商密钥的线上兼容性测试。

## 源码风格与结构

沿用 `libai/.clang-format` 的 Linux kernel C++ 排版及现有下划线类型命名，使用 clang-format 18。
代码缩进不超过 4 级，每个函数不超过 200 行，每个文件不超过 1000 行。
SDK 类型、运行循环和公开 facade 分开存放；协议 transcript 转换独立为 provider 实现文件。
头文件类型、成员和方法保留英文注释，关键运行边界和恢复行为在实现中说明。
`coolder/tools/check_cpp_structure.py` 与 `check_header_comments.py` 提供自动检查。
