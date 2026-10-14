# 跨语言编码接口

C++、C、Java、Python、Rust、Objective-C 使用同一个 libai coding service，功能由
`capabilities` 的 operations 返回值确定。C++ 的公开 `libai_cpp/coding.h` 提供轻量
CodingService 和 Provider 接口。coolder 的原有编码运行时、检查点、审查和沙箱流程
使用源码构建中的私有宿主兼容适配，不进入安装 SDK。JSON 服务不暴露 HTTP 身份认证或账号管理。

## 构建和消费

C++：`-DLIBAI_CPP_FULL=ON`；C/Java/Objective-C，以及 Python 所加载的 C 库：
`-DLIBAI_CODING_FULL=ON`。同时配置同架构 ACL 和 OpenSSL 的路径。
所有绑定的 build.sh/clean.sh 继续可用，Python build.sh 将额外 CMake 参数传给 C 库。
Rust 使用环境变量 `LIBAI_CODING_FULL=ON`、`ACL_PATH`、`OPENSSL_ROOT_DIR`，完整模式链接动态 C 库。
默认无完整组件时，available 为 false；调用报告明确的不可用错误。
Android/iOS 目前打包 portable Agent 和可用性查询，完整 desktop coding backend 不支持移动交叉编译。

C++ 安装后链接 `libai::cpp`；C 安装后链接 `libai::c`。完整包将后端对象嵌入绑定库，
无需原始 libai 头文件或 CMake 包。完整静态包的消费方配置 `ACL_PATH`/`OPENSSL_PATH`
以恢复链接依赖；完整动态包无需 ACL/OpenSSL 的开发头文件。Java 加载 ai_jni，Python 加载 ai_c。
Objective-C 的静态库内含后端对象，依赖 ACL/OpenSSL，CMake 目标会传递链接项；分发静态归档时也须
分发和链接这些依赖。移动静态包为独立 portable 库。

## 调用和边界

服务绑定宿主指定的已有 root 目录。输入是 JSON object，结果为 `{ "ok": true, ... }`
或 `{ "ok": false, "error": "..." }`。C 的 NULL、C++/Java/Python 的异常、Rust Err 和
Objective-C NSError 表示 ABI/组件可用性错误。C 返回字符串必须使用 libai_string_free 释放。
调用是同步的，放在工作线程。JSON 文本不允许 NUL，输入最大 8 MiB；原始模块还会进一步
限制文件、事务、提示词和记录大小。字符串字段必须是字符串，整数、布尔与数组也校验类型。

root 是受信任宿主配置；此 JSON 服务不继承 coolder 账号层的外部项目目录映射。
工作区文件操作拒绝目录穿越、越界符号链接和敏感元数据。完整服务拒绝包含外部项目映射
注册表的 scope，避免补丁或事务继承额外目录权限。嵌入者负责选择正确的用户 scope 和授权：
不要把全量 service.call 直接注册为无需授权的模型工具。
写入/删除/应用补丁/修改配置、会话和项目状态必须由宿主决定是否允许。
`patch.apply`、`changes.apply` 要求宿主先展示预览并确认 ID；模型提案不能自行确认。

provider 设置使用 root 下的专用 app 身份保存并加密密钥；返回值只含提示，不含明文或密文。
这与 coolder 自身按账号/安装范围的 provider store 配置独立。
`providers.complete` 使用宿主选择的 provider，返回原生模型结果；无需暴露密钥给语言模型回调。
此入口不流式传输，仅通过 timeout_ms 设置协作期限（1–300000，默认 30000）。
现有 Agent 的流式输出、取消、授权和事件回调保持可用。
`runs.audit_cancel` 仅修改审计状态，不会取消一个正在执行的 Agent；真正取消应调用 Agent.cancel。
`sandbox.preview` 只准备固定命令授权，JSON 服务不自动执行或消费它。
`sandbox.execute` 在桌面完整构建中执行已确认的固定命令计划，要求安装 helper 及可用的隔离 backend；
执行是同步的，由宿主在线程/任务中调度。宿主通过 `sandbox.cancel` 取消活动执行；不支持的系统明确失败。

## 操作协议

字段缺省为空字符串或模块默认值；所有失败均通过 ok/error 报告。
list 的 limit 默认 20；模块自身再限制可接受范围（例如 sessions 不超过 50）。

| 操作 | 参数 | 结果或用途 |
| --- | --- | --- |
| capabilities / agents.list | {} | operations；agent/tool 元数据 |
| tasks.evaluate | TaskContract v1 对象 | accepted、checks、unmet；无副作用验收评估，不执行构建、不接受变更 |
| tasks.revision | changes（可选提案数组） | artifact_revision；受限项目清单合并待确认修改后的内容指纹 |
| tasks.describe_run | id、status、changes、validation、test_validation | 当前成果的 TaskContract；从真实命令退出状态和带身份的审核记录派生 |
| storage.migrate | {} | 升级本 scope 下的历史私有目录与元数据，返回 renamed_paths、migrated_files；格式转换保留 .legacy 备份，宿主必须先取得数据目录锁；冲突路径及活动执行锁会拒绝迁移 |
| storage.canonical_path | path | 将历史保留目录名称转换为当前名称，仅返回路径，不访问文件；用于宿主读取旧引用 |
| workspace.list / read / outline / sha256 | path | entries；content/truncated；items；sha256 |
| workspace.search | path, query | matches/truncated |
| workspace.create | path, content | 仅创建不存在的文件 |
| workspace.replace / delete | path, expected_sha256；replace 另带 content | 摘要前置条件写入/删除 |
| workspace.mkdir / rmdir | path | 创建；只删除空目录 |
| patch.preview / apply | path, content / id | id、diff、摘要；一次性应用结果 |
| changes.preview / apply / recover | changes 数组 / id / {} | 事务预览；一次性应用；中断恢复 |
| projects.list / get / remove | limit / id / id | projects；项目记录；移除元数据 |
| projects.create | title, project_path, language, platform | 项目记录 |
| projects.plan | id, expected_plan_version, goal, modules, tasks | 乐观并发更新计划 |
| projects.task_status | id, task_id, status, expected_plan_version | 更新任务状态 |
| projects.scaffold | project_path, language, platform | files；不运行包管理器 |
| projects.toolchain / diagnostics | project_path；diagnostics 另带 source | commands；diagnostics |
| sessions.list / get / remove | limit / id / id | sessions；会话含 messages；删除 |
| sessions.create | title, agent_id, provider_id, project_path | 会话记录 |
| sessions.update | id, title, summary, last_run_id, user_prompt, assistant_reply, assistant_state | 更新摘要和会话；还可带 reasoning/usage/duration 字段 |
| runs.list / get | limit / id | runs；元数据记录 |
| runs.create | id, agent_id, agent_version, provider_id, model, project_path, started_at | 创建运行审计 |
| runs.complete / fail / audit_cancel | id；complete 带 usage/count 字段；fail 带 error | 更新审计状态 |
| sandbox.history / preview | limit / project_path, command_id | runs；固定命令的授权预览 |
| providers.list / remove | {} / id | providers；删除配置 |
| providers.save | id（更新时）, name, protocol, base_url, model, api_key；可选 enabled, allow_file_content, is_default, clear_api_key | 无密钥的配置记录 |
| providers.test | id | 原生连接/能力测试；返回 http_status、latency_ms、endpoint 和能力验证状态，持久化测试健康记录；不使用普通文本生成的 16-token 预算 |
| providers.complete | id, instructions, messages, tools, max_output_tokens, timeout_ms | text, reasoning, calls, input_tokens, output_tokens, retryable |

changes 中每项为 `{operation,path,target_path,content,reason}`；operation 支持 write/delete/move/mkdir
及模块定义的空文件转目录操作。modules/tasks 的字段与 C++ 项目记录类型相同。
scaffold 的 platform 使用模块支持的平台名，如 linux/macos/windows/cross-platform，而非 native。
completion 的 messages 是 SDK 的 `{role,text,call_id,reasoning,tool_error,calls}`；tools 是
`{name,description,parameters}`，parameters 和 calls.arguments 是 JSON 字符串（与现有 Model 回调一致）。
completion 返回的 calls 可直接传给既有 Agent 的 ModelResponse；宿主应将 ok=false 的 error 转成
ModelResponse.error，保留 retryable。

## 示例

```python
from libai import CodingService
service = CodingService("/absolute/app-workspace", library="/absolute/libai_c.dylib")
assert service.available
print(service.call("workspace.read", {"path": "src/main.cpp"}))
```

```java
CodingService service = new CodingService("/absolute/app-workspace");
String json = service.call("workspace.read", "{\"path\":\"src/main.cpp\"}");
```

```rust
let service = libai::CodingService::new("/absolute/app-workspace")?;
let json = service.call("workspace.read", r#"{"path":"src/main.cpp"}"#)?;
```

```objective-c
#import <LibAI/LAICodingService.h>
LAICodingService *service = [[LAICodingService alloc] initWithRoot:@"/absolute/app-workspace"];
NSDictionary *result = [service call:@"workspace.read" arguments:@{@"path":@"src/main.cpp"} error:&error];
```

验证包含 C++ 类型化链接、各语言 Unicode/错误路径，以及 Python 经 C ABI 对摘要并发校验、补丁重放、
事务、目录边界、项目和会话的实际调用。coolder 的 facade_dependency_test 防止 APP 再次直接包含/lib链接 libai。

C++ 绑定的公开类型独立于原始 libai，公开头文件不包含 libai 头文件。
`CodingService` 的跨语言 JSON 操作协议保持一致；Java、Python、Rust、Objective-C
及移动端现有绑定无需感知 C++ 类型转换或命名空间的变化。

C++ 公共 `coding.h` 只提供轻量入口；工作区、项目、会话等功能统一通过
`CodingService` 操作协议调用。私有值转换和宿主适配头文件位于 `libai-c++/src`，
不安装到 SDK，也不被公共头文件引用。NativeProviderModel 使用标准库组成的
独立 `ProviderConfig`，无需了解内部 Provider 存储记录。

## Agent 接口能力补齐

各语言现在均支持：复制完整会话历史、独立取消令牌（可在 run 前取消，状态不自动重置）、
以及内置 Provider Agent。Provider 的配置字段与 C++ `ProviderConfig` 一致，密钥单独传入、
只保存在模型实例内存中；内置 Provider 要求桌面完整构建，移动/轻量构建返回明确的不可用错误。
普通 Agent 的流式文本、工具、授权和事件钩子同样适用于内置 Provider。
会话只能在 Agent 空闲时读取；返回快照不影响原会话。取消令牌可跨线程使用。

| 语言 | 会话快照 | 取消令牌 | 内置 Provider |
|---|---|---|---|
| C++ | `Agent::messages()` | `Cancellation` / `RunOptions::cancellation` | `NativeProviderModel` |
| C | `libai_agent_messages()` | `libai_cancellation_*` / `libai_agent_run_with_cancellation()` | `libai_agent_create_provider()` |
| Python | `Agent.messages()` | `Cancellation` / `run(cancellation=token)` | `Agent.from_provider(config, api_key)` |
| Java | `Agent.messages()` | `Agent.Cancellation` / `run(text, options, hooks, token)` | `Agent.fromProvider(configJson, apiKey, instructions)` |
| Rust | `Agent::messages()` | `Cancellation` / `run_with_cancellation()` | `Agent::from_provider()` |
| Objective-C | `[agent messages:&error]` | `LAICancellation` / `LAIRunOptions.cancellation` | `initWithProvider:apiKey:instructions:error:` |

C 的会话结果是拥有所有权的 JSON 数组，使用 `libai_string_free()` 释放；取消令牌句柄需要销毁。
新增函数不改变原 `libai_options` / `libai_hooks` 布局或旧函数签名。
C 调用期间不能并发销毁同一句柄；`libai_cancellation_retain()` 可生成独立所有权的共享句柄。
Python/Java 提供 close/上下文管理，Rust/Objective-C 自动释放；运行会保留取消状态的有效生命周期。

C/C++ 的完整库共用 libai 后端，不再维护改名的源码/内部头文件副本；各语言的
CodingService 操作使用同一实现。审计记录与沙箱预览不会执行任务；
模型/工具循环使用 Agent，工具授权和沙箱执行策略由宿主提供。coolder 的内部宿主兼容接口
不安装、不作为稳定第三方 API，后续通用能力应先在 libai 实现，再通过公开接口绑定。

## 原生语言 APP 与通用能力补齐

coolder 的 server/action 源文件保留在 C++ APP 中。coolder-java 和 coolder-rust 各自实现
完整 APP 业务，libai 不提供整套 APP 的 Application/HTTP dispatch 转发接口。

`providers.complete` 额外返回 `cached_input_tokens`、`cache_usage_available`、`latency_ms`；
可带 `images: [{name, mime_type, base64}]`，最多 8 个、每个解码后不超过 8 MiB。
这会转换为 OpenAI Chat/Responses、Anthropic、Gemini 的图片内容块；原有 typed transcript 保留。
仅此操作的 JSON 请求上限提高至 32 MiB，其余仍为 8 MiB。Provider credential 不返回给 APP。

| 操作 | 参数 | 结果 |
| --- | --- | --- |
| sandbox.capabilities | {} | available、reason；不可用时禁止执行 |
| sandbox.execute | id、confirm: true、helper（可选，宿主受信配置）、idempotency_key、parent_idempotency_key（均可选） | 一次性消费 preview ID；idempotency_key、started、exit_code、stdout、stderr、elapsed_ms 等 |
| browser.receipt.inspect | idempotency_key | found、revision、receipt_json、recovery_policy；POSIX只读浏览器动作诊断，与模型browser.inspect工具不同 |
| browser.receipt.confirm | idempotency_key、revision、outcome、reference、result | 受信宿主核对running记录；none/applied确认；不派发、不开放旧键重放、不确认父任务 |
| batches.inspect | idempotency_key | found、revision、receipt_json、recovery_policy；POSIX只读批次子调用诊断，不授权重放 |
| batches.confirm | idempotency_key、revision、child_index、outcome、reference、result | 受信宿主确认running/returned子调用；none/applied；不执行、不重放、不确认父操作 |
| sandbox.inspect | idempotency_key | found、revision、receipt_json、recovery_policy；POSIX只读命令收据诊断 |
| sandbox.parent.inspect | idempotency_key、project_path | found、revision、children；C++验证器已关联的逐命令键，供宿主分别核对 |
| sandbox.confirm | idempotency_key、revision、outcome、reference、result | POSIX受信宿主确认未知结果，返回新revision/state，不执行命令 |
| sandbox.cancel | id | 对同 root 下活动执行发出取消；不可取消其他 scope |

`projects.toolchain` 的 commands 增加 fixed_arguments、dynamic_arguments、requires_confirmation。
`sandbox.preview` 增加 executable 和 fixed_arguments，供宿主完整展示确认内容。

完整桌面服务的sandbox.execute接收最多4096字节的严格JSON对象，只允许上述五个
字段；重复键、额外字段、错误类型及非法幂等键在消费计划前拒绝。键最多256字节，
仅允许ASCII字母、数字、下划线、连字符、点和冒号；缺省使用`sandbox.<计划ID>`。
宿主可传入Agent工具回调的idempotency_key，将逻辑执行关联到命令。POSIX broker
通过固定环境变量`LIBAI_IDEMPOTENCY_KEY`传给进程，不追加命令参数、不继承服务
环境。sandbox.history保存并返回同一键；旧审计记录保持空键，不补造历史证据。
Java/Python/Rust/Objective-C与C/C++共用此JSON入口，无需新增外部库类型。
该字段不授权执行，不对不同计划自动去重；业务命令须自己落实幂等键与结果收据。
Windows沙箱当前仍失败关闭，尚未验证设备端命令执行。

POSIX执行器现在对每个键持有进程间租约，先持久保存running元数据意图再派发，
返回后持久保存finished元数据。记录位于root/.libai_agent/sandbox_commands，
使用SDK私有文件存储的版本检查、原子替换及文件/目录fsync；不保存stdout/stderr。
已存在running或finished记录的键均拒绝执行，且在消费新计划之前拒绝，不能借
新计划ID绕过同一执行键。sandbox.inspect只返回进程结果事实、命令/输出摘要等
元数据；running要求host_reconciliation，finished要求do_not_replay。finished不
证明外部业务成功、源码覆盖或任务验收通过。诊断需要已有安全目录，可能创建锁。
命令级宿主确认使用sandbox.confirm，不应手工删除或修改记录解除未知状态。
macOS helper既有规则拒绝沙箱读取/写入私有元数据；Linux helper在私有挂载命名
空间对工作区中的.libai_agent设置只读空遮蔽，并拒绝挂载管理系统调用，遮蔽
失败则停止派发。本阶段未运行Linux环境测试，不能据此声称其运行验收完成。

sandbox.confirm只用于已鉴权、已核实效果的宿主，不得注册为模型工具。参数严格
为五个字段，最多16KiB；revision须匹配inspect返回版本，记录必须为running。
outcome=none要求result为空对象，保存confirmed_none；outcome=applied要求result
为与inspect相同的九字段结果元数据对象，保存confirmed_applied。reference为
非空宿主审计引用（最多2048字节）；库不核实引用的签名或真实性。确认取得同一
进程间租约并按版本提交，活动/陈旧/重复确认拒绝。确认不改命令身份或摘要，不
写stdout/stderr，不执行或自动重试命令，confirmed_none同样不能重放旧键。
它不确认外层Agent工具日志、不更新任务验收；宿主仍须分别核对相关父执行。
这些是通用操作，现有 C/C++/Java/Rust/Python/Objective-C CodingService 都可直接调用；
模型工具必须使用宿主明确授权的白名单，不能暴露全量 CodingService。

`runtime.policy.get`、`runtime.policy.validate`、`runtime.policy.configure` 提供进程级通用运行参数（工具链、提供商超时、工具预算和沙箱资源限制）。嵌入方负责授权和持久化；账户角色及 APP 路由仍由 APP 自己实现。

持久化数据使用 JSON，旧格式启动时迁移并保留 `.legacy` 备份。存储结构、迁移规则与跨 APP 使用范围见 [JSON_STORAGE.md](../../coolder/docs/JSON_STORAGE.md)。

`providers.complete` 返回 `effective_max_output_tokens` 和 `reasoning_tokens`。响应因输出预算耗尽失败时，错误包含实际生效的单次输出上限；Agent 的总输出预算与 `max_tokens_per_call` 必须分别配置。

OpenAI Chat/Responses 兼容协议的 `base_url` 可配置完整 API 前缀，例如
`https://open.bigmodel.cn/api/paas/v4/`、`https://example.com/gateway/custom/`。
请求路由直接追加到该前缀；只配置域名（无路径）时默认追加 `/v1`。
无需将 `/chat/completions` 或 `/responses` 写入 `base_url`。

任务状态、证据可信来源及实施范围见 [通用 Agent 平台实施记录](AGENT_PLATFORM_PROGRESS.md)。

tasks.describe_run 的验证报告必须包含与当前成果匹配的 artifact_revision、commands；命令含 command_id、started、ok、exit_code、cancelled、timed_out。单独的 passed 状态或模型文字不会产生验证证据。审核提案须含 accepted_by 与 review_reference。指纹扫描排除私有/构建目录，有目录深度、条目及字节预算；变化和扫描失败应使验收失效。

可选parent_idempotency_key使用相同键格式；缺省或空值保留无父关联行为。
非空时，在保存running命令意图后、派发前提交父关联，可用sandbox.parent.inspect查询。
关联提交失败不派发，但计划已消费且running意图保留，需宿主核对，不能直接重试。
该字段不自动关联任意回调；父子记录必须使用同一持久存储root与项目。
