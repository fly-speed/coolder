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

### 编程运行的默认性能策略

Go 中间自动构建在已有构建脚本之外，为暂存 Go 写入所在的每个包执行 `go.packages.<包路径哈希>.build`（`go build -o .libai-build/go-packages/<哈希>.artifact ./包路径`），覆盖尚未被入口引用的新增/修改包及其依赖；仅编译，不执行测试，不改变脚本和完整验证的运行入口。使用单包与独立输出文件，避免 Go 多包目录输出模式跳过库包。所有输出限制在私有草稿目录。根模块之外的嵌套独立模块仍需单独配置验证。

内部原生多调用批次按实际子工具判断外部执行风险。仅含读取及私有文件提案的失败批次可正常记录返回并由模型继续修复，不能将其统称为外部结果未知；解析失败、未知子工具、验证或浏览器操作仍保留核对要求。不根据模型声称的 `rolled_back/side_effect_free` 放行，混合批次不因草稿回滚而自动重放命令。失败与取消收尾会更新已有增量成果的文案、任务证据及摘要，保留文件内容、审查决定和恢复记录。

前端 `thinking_adaptive=true` 优先于手动 `thinking_enabled` 和指定推理档位。支持思考开关的提供商使用内部 `thinking_mode=adaptive` 保存运行与恢复状态，在每轮构造请求时解析为实际值：普通编程轮次关闭思考；大型任务、验证失败或必需修复读取阶段启用思考，支持档位时使用 `high`，验证通过后普通轮次恢复关闭。`adaptive` 不直接发给提供商。输出耗尽等恢复关闭策略仍优先。未开启自适应时保留手动选择；不支持关闭思考的模型仍遵循其能力限制。自适应复选框默认关闭，开启后暂时禁用手动复选框，关闭后恢复手动选择。

启动预读包含根目录 `.gitignore`，并复用已扫描的 `src`/`tests` 直接子项清单，放入 `bootstrap_directories`。每个目录最多保留 64 项、约 3 KiB 的条目内容，不递归扫描；`complete=false` 明确表示有省略。提供商禁止发送文件内容时，不发送这些扩展预读数据。模型应先复用预读；仍有必要缺页或目录信息时先调用读取工具，再集中设计与提交实现和测试，不在一次长设计后只返回基础读取。后续必须读取时，用简短可见说明记录已确定的设计决策以便压缩后复用，说明中不包含源码。

此策略用于 libai 的 coding tool loop，不改变通用 `providers.complete` 或宿主自己实现的 Agent 循环。提供商支持推理档位、调用方未明确设置档位且未关闭思考时，普通轮次使用 `low`，单轮输出上限取原配置与 65,536 的较小值；大型执行模式或当前验证失败的修复轮次使用 `high` 并保留原输出上限。明确选择的 `low/high/max`、模型能力上限和关闭思考设置优先；输出耗尽及 DeepSeek 无推理历史恢复仍采用原有恢复策略。

`model_request_started` 新增 `reasoning_policy`，可与 `reasoning_effort`、`max_output_tokens` 和最终用量对照，区分 `adaptive_ordinary`、`adaptive_repair_or_large`、`configured` 与 `recovery_without_reasoning`。实际速度与功能质量仍需真实模型任务验证，不能从本地回归推断固定节省比例。

允许发送项目内容时，启动上下文有界预读根目录构建/运行脚本及 `src`、`tests` 顶层的入口和测试，不递归扫描仓库；`bootstrap_source` 是正式项目基线，含文件版本和续读元数据。验证能力的 `command_network` 提供每个固定命令的网络限制；禁止监听的单元测试应使用进程内 handler，真实服务就绪和浏览器交互保留独立验收。

压缩上下文的暂存源码预算为 64 KiB（包含 JSON 转义与元数据）。完整快照能够装入时保留全文；超过预算时优先保留声明与小文件，再分配大文件前缀；已缓存的同版本起始源码页不再重复附带。所有缺页仍明确标注 `eof/next_query`。

普通开发任务仅写入可识别的测试源码时，省去该次修改后的中间自动构建，记录 `automatic_build_deferred`，由完整 `workspace.validate` 统一编译和执行固定测试。构建脚本、配置、实现、测试夹具或删除操作的变化，以及专门的编译修复任务，保持自动构建。延后构建不产生通过证据，修改后仍须完成与当前版本匹配的验证。

自动构建的 `test_status=not-run` 表示本次仅构建；完整验证独立执行固定测试，模型不应为取得测试证据将测试重复添加到新生成的构建脚本，用户原有脚本行为则保留。验收收尾默认复用已有源码与验证证据返回逐项状态，只针对具体缺陷或已明确的未实现需求追加修改；没有单独直接测试的行为应标记未验证，不因此启动常规覆盖扩充。通过编译和测试仍不等于完成业务验收。

### 临时 HTTP 503 的有限退避重试

提供商明确返回 HTTP 503 且尚未收到正文、推理或工具调用时，在同一模型轮次内最多自动重试三次（含首次请求最多四次）。默认退避为 1、2、4 秒，优先采用错误诊断中保留的数字 `Retry-After`，限制为 1–60 秒；与已有临时 429 共用重试额度。等待期间可取消，不改变请求内容、推理设置，不重放已执行的工具。持续繁忙仍返回原始错误并走原有断点保存流程；其它 5xx 不因此扩大重试范围。

### 关闭推理时的修复收敛

实现任务按原始需求补充有界的行为测试设计提示：多人同会话初始状态与首步存活、连接数不改变共享时钟、多房间隔离、相同模拟时间内速度档位的实际移动差异、暂停恢复不重置进度等。只选择适用场景，随首个完整实现提交原生测试；不将提示作为新增用户需求或可信验收证据。纯分析、验证及编译修复任务不扩充这些场景。分批实现应选可独立编译的最小闭环；符号导出或重命名必须同步调用方或保留兼容别名。

能力上下文明确给出 `browser_acceptance_policy_enabled`，浏览器验收关闭时不再提示一定执行浏览器。原生行为测试、HTTP 就绪、模拟 DOM 和真实浏览器证据分别报告。编译失败后连续两轮读取/搜索/列目录且没有必需修复缺页时，暂时收起广泛 list/search，保留精确 read/read_batch 及续页；新修订或显式验证重新开放，正常功能开发、纯分析及显式浏览器调试不受此限制。

首次完整验证通过后，收尾请求仅在草稿和正式源码指纹仍匹配时收起重复 workspace.validate，附带真实测试/浏览器/功能验收状态，要求返回逐项最终报告。修改或外部源码变化立即恢复验证；定向测试与 build-only 不能触发此收尾策略。兼容模型仍能返回历史工具调用，运行时沿用原有验证复用逻辑，不因省轮次忽略实际失败或将未测功能标记为完成。

手动关闭推理且未启用自适应时，修复仍保持关闭推理。循环独立保留尚未解决的测试失败，后续仅构建通过不能覆盖这些失败。首次验证失败即提供相关的当前草稿源码；同一失败再次出现时要求检查实际输入、状态变化、返回值约定及测试预期，再做有证据的修复。Go 测试签名忽略耗时，避免同一失败因耗时变化而被当作新问题。读取不存在的草稿路径时附带有界的实际路径清单，减少猜测文件名。

存在可可靠定位包路径的 Go 测试失败时，后续修改自动运行相关包的固定测试命令，报告 `validation_scope=targeted-tests`、`targeted_packages` 和 `final_validation_required=true`。包测试通过只表示局部修复通过，必须再次完整验证才可清除未解决失败并形成最终验收证据。诊断路径不明确或不安全时退回原验证流程。这些机制减少失焦和无效重复，不保证任意模型一次生成正确项目；完整编译、测试和业务验收仍分别保留。

### 已完成模型响应的非 JSON 批次兼容

coding tool loop 收到明确 `response_status=completed` 的单个原生 `workspace.propose_batch` 调用时，如果仅因内层 `content` 不能解析成文件数组而未执行批次，会将精确原始参数和解码后的 `content` 保存到项目私有 `.libai_agent/responses/response-<run>-<sequence>.json`，并记录 `completed_batch_content_recorded` 事件。本轮处理结束，不自动重新生成整批源码；此前暂存的修改继续保留。此处的完成只表示模型轮次结束，最终回复明确说明该批次未生成文件、不代表项目实现或验收完成。

不完整响应、权限或路径拒绝、容量超限、无效 `saved_batch` 引用和其它执行失败保持原规则；独立工具接口仍严格返回解析错误。未解析内容不当作文件写入，归档失败也不能静默忽略。响应归档独立于实际发往模型的请求归档，操作日志只包含引用和元数据，不复制整个源码批次。


### 生成效率：失败检查点、读取与请求上下文

对已定位到源码行的编译失败（以及 Go 的 `no Go files in` 目标错误），自动构建会保存失败检查点。正式源码基线、完整暂存修改、私有草稿和执行配置版本均未变化时，下一请求暂时收起 `workspace.validate`，要求先修复已给出的诊断。过期的原生工具调用仍按本轮公布的 schema 拒绝，不能绕过权限或参数检查。直接工具路径复用验证报告前还会检查原有命令回执；复用保持原来的失败状态和 `validation_scope`，返回 `validation_reused=true`、`executed_this_call=false`、`commands_executed_this_call=0`，不作为新的测试证据。源码、草稿或配置版本变化使该失败检查点失效；运行中配置变化仍遵循原有执行配置核对和恢复机制。超时、取消、沙箱错误和测试断言失败不用于这一编译失败收敛策略。

Go 构建或构建脚本失败后，在存在已配置 Go 安装的同目录 `gofmt` 时，以 `-e -l` 补充解析最多 64 个本轮暂存的 Go 源文件；不改写源码、不启动后续测试或服务。该步骤只报告 `syntax-only`，不能证明类型检查、完整编译或项目测试通过。命令报告包含工作目录、固定参数和耗时。修复上下文优先附带诊断附近最多 6 KiB 的源码窗口、相关包/接口文件，以及目标错误涉及的构建脚本和入口配置；窗口保留文件哈希与字节偏移，缺失调用方仍需定向查询。

`workspace.read` 误读目录时给出 `path_state=directory`、`workspace.list` 提示及最多 8 个、约 1 KiB 的目录条目，并标明是否完整。`read_batch` 中普通文件读取失败逐项返回 `errors`，成功页面继续返回和缓存，续读保留 `next_content`。错误反馈继续进入下一轮模型输入，不因成功页面已经缓存而丢弃。越界路径、权限和参数 schema 仍执行整批预检查，不以“部分成功”绕过。

请求构造阶段按项目路径、完整文件版本和字节范围去除重复源码。只有相同字节仍在本次实际发送的请求中时，才用 `content_omitted` 与 `source_ref` 指向它；不同版本、缺失范围和有状态续接中未发送的本地历史不能充当引用。小于 256 字节的内容不替换，引用索引最多保留 2 MiB 源码。原始历史工具参数、审计记录及恢复检查点保持完整。完整项目测试通过后清除已解决的活动修复要求；局部测试和 build-only 不清除尚未解决的最终验收要求。

生成提示已撤回固定 1～3 个阶段、核心层先行及界面后接入的安排，改为简短的实现契约：复用已有证据、一起提交实际相互依赖的修改及相关测试、维护入口与构建脚本的一致性。批次边界由实际依赖决定，不规定阶段数或层次顺序。保留验证、版本缓存、精确读取、去重和统计机制；这些控制流回归不等于真实模型的性能保证。

操作日志增加 `implementation_checkpoint_submitted`（本批改变文件、源码字节、测试文件数及有界路径清单）；`model_request_started` 增加 `context_bytes_before_source_dedup`、`context_bytes_after_source_dedup`、`source_bytes_elided` 和 `source_records_reused`。前后字节统计覆盖内部请求文本字段（后值包含引用说明），不是序列化 HTTP 字节数或精确 token 数；实际节省应结合提供商 usage 和真实任务耗时评估。验证报告的 `commands_executed_this_call`、`executed_this_call` 区分实际运行与历史复用，每个命令的 `elapsed_ms` 用于定位构建开销。

回归覆盖：`ai_coding_efficiency_test` 检查失败缓存失效、源码版本/范围、部分错误保留和诊断窗口；`coolder/tests/generation_efficiency_test.py <coolder binary>` 使用本地模型夹具，在关闭推理下执行部分读取、双诊断、外部基线变化、成批修复、最终原生测试与收尾流程。夹具用于验证控制流和真实性，不代表真实模型的生成质量或固定百分比性能收益。


### GLM 的推理选项

选择 GLM 模型（含网关的 `ZHIPU/GLM-...` 名称）时，界面默认勾选自适应推理，并将启用推理保持勾选、不可修改。自适应开启时忽略手动启用选项与手动强度；取消自适应后使用普通启用推理，保留显式强度，未指定时使用提供商默认。刷新模型列表保留同一模型的当前选择；切换回非 GLM 模型恢复原先的推理选项。后端对 GLM 同样强制启用推理，省略自适应字段时默认开启，显式 `false` 则采用普通启用模式。

GLM-5.3/Flash/FlashX 不支持关闭推理，支持 `low/high/max`；自适应轻量轮次使用 `low`，只有具备持续行为失败证据时临时使用 `high`；大型任务标记不会单独触发升级。输出预算恢复的关闭推理请求也映射到 `enabled + low`，不发送不支持的 `disabled/none`。GLM-5.2 的低强度映射为 `high`；旧版 GLM 保持 `thinking.type=enabled`，不发送其不支持的 `reasoning_effort`。请求日志同步反映映射后的实际参数。模型能力依据智谱官方文档 https://docs.bigmodel.cn/cn/guide/capabilities/thinking 。

验证：`node coolder/tests/glm_thinking_ui_test.js` 检查默认、锁定、反选自适应、模型切换及刷新；`coolder/tests/adaptive_thinking_test.py <coolder binary>` 经真实主机检查 GLM 与 DeepSeek 的实际出站参数；`ai_sdk_provider` 覆盖 GLM 参数序列化和恢复兼容。


### 重复路径批次的直接恢复

`workspace.propose_batch` 遇到同一规范化项目路径的多个记录时仍原子拒绝，不选择“最后一个覆盖前面”。反馈使用 `error_code=proposal_batch_duplicate_paths`，返回冲突路径、原数组的零基索引、操作、内容长度和 SHA-256。冲突元数据有 8 KiB 预算，`duplicate_path_count` 和 `duplicate_paths_complete` 标明是否完整；未通过路径授权的批次仍优先拒绝。

可保存的批次（最多 4 MiB）会原样保存在本次运行的 `saved_batch` 中，并尝试归档到项目私有 responses 目录。归档失败返回 `archive_error`，不能声称已持久保存；内存引用仍可用于当前运行。该引用沿用既有容量恢复语义，仅在当前运行有效，重启后不承诺可重放。返回 `response_archive` 时可审计原始内容，归档本身不修改正式源码。

模型可用原工具的 content JSON 检查一个记录：`{"saved_batch":"返回的哈希","inspect_index":2,"query":"0"}`。检查是只读的，不暂存记录；内容按管理员单页限制且最多 4 KiB 分页，使用 `next_query` 继续；记录索引、项目路径和查询类型均校验。读取原内容后，模型明确决定保留哪一条，再用 `{"saved_batch":"哈希","offset":0,"limit":4,"exclude_indices":[0]}` 重放原批次。排除索引必须是唯一、范围内的非负整数；offset/limit 始终针对原数组位置，不因排除而向后补位。切片容量、路径授权、内容限制、提案校验和原子提交保持原规则。

可恢复的重复路径错误存在期间，下一模型请求暂时收起宽泛 list/search，保留精确读取、代码引用及原批次检查，直接携带恢复信息。实际草稿修改后解除该限制。检查失败批次不会被当成验证通过或实际文件修改；混合批次原子回滚仍生效。

实际实现提交的反馈增加 `implementation_evidence`，明确本批源码、测试文件数与当前暂存测试文件数。没有独立测试文件修改时只提示利用相关已有或内联测试，或随实现提交必要行为测试，不把测试文件数量当作测试有效性证明，也不因缺少新增测试强制额外轮次。编译修复反馈保留各命令的结构化诊断，提示一起处理已知错误和相关定义、导入、调用方；不保证编译器已经揭示全部后续错误。

回归：`ai_coding_efficiency` 覆盖重复路径拒绝、原记录检查、索引重放、非法引用与越界路径；`coolder/tests/proposal_batch_recovery_test.py <coolder binary>` 覆盖关闭推理下失败批次直接恢复、分页、草稿保持、完整验证和直接收尾；原 generation_efficiency 回归继续覆盖保留的 P0/P1 机制，并检查固定阶段提示已撤回。


### 2026-10-15 全部 snake 项目回顾与可控性改进

审计范围为 `/Users/zsx/work/github/fly-speed/var/workspace` 下全部以 `snake` 开头的项目目录，共 21 个项目（`snake`、`snake2`～`snake21`），35 次运行；与共享 run registry 的 35 条相关记录逐一对应，未发现缺失日志或状态不一致。快照与每份日志 SHA-256、逐次指标、数据缺口保存在同目录 `generation-review-2026-10-15.json`。状态完成 18 次、失败 8 次、取消 9 次。这里的完成表示运行结束，不代表业务或浏览器验收完成。

各项目耗时包含初次生成、后续修复、取消和失败等不同任务，不能直接作 A/B 性能比较；表中为已结束运行的耗时范围。输入为已记录 tokens 下界，取完成响应累计与结束汇总中较大的值，避免早期结束记录清零丢失已发生用量。中断请求未返回的 usage 不估算。snake5/6 的刷新空白来自此前用户反馈，单独归因于会话恢复，不以生成日志证明已复现。

| 项目 | 运行数 | 完成/失败/取消 | 耗时范围（秒） | 已记录输入 tokens 合计 | 回顾的问题 |
|---|---:|---|---:|---:|---|
| snake | 1 | 1/0/0 | 564.1 | 350,156 | 模板 greeting 被替换后旧测试失配；沙箱网络限制。 |
| snake2 | 1 | 1/0/0 | 917.8 | 857,841 | 跨文件缺少 idx/core 定义；测试误用 API 与状态类型。 |
| snake3 | 3 | 2/1/0 | 5.7～2203.1 | 982,516 | GLM 长推理、404 配置错误；embed 导入、网络受限与碰撞测试失败。 |
| snake4 | 1 | 1/0/0 | 969.8 | 833,717 | 模板测试与方法返回值失配；移动行为失败。 |
| snake5 | 2 | 1/0/1 | 243.4～1203.8 | 719,704 | 长推理与取消；历史用户反馈涉及刷新后会话空白。 |
| snake6 | 1 | 0/0/1 | 495.1 | 17,193 | 长等待后取消；历史用户反馈涉及发送后刷新空白。 |
| snake7 | 1 | 1/0/0 | 974.0 | 522,232 | 长模型等待；模板 greeting 与页面测试失配。 |
| snake8 | 1 | 1/0/0 | 563.6 | 157,106 | 完成响应内批次 content 不能解析；完成状态与文件/验收完成需区分。 |
| snake9 | 1 | 1/0/0 | 368.1 | 358,143 | 测试预期接口返回值与实现不一致；补读与重复验证。 |
| snake10 | 1 | 0/1/0 | 828.4 | 386,933 | 测试接口失配；固定端口占用；执行结果未确认。 |
| snake11 | 1 | 1/0/0 | 496.7 | 238,560 | 空/旧模板测试、语法错误与房间退出行为。 |
| snake12 | 1 | 1/0/0 | 466.9 | 314,250 | 长模型响应；Join 参数与测试不一致。 |
| snake13 | 1 | 0/1/0 | 177.7 | 297,776 | 关闭推理后仍有缺少嵌入资源、类型/重复分支错误；编译未收敛。 |
| snake14 | 2 | 0/1/1 | 95.6～809.3 | 1,507,054 | 多轮依赖/测试接口失配、缺失资源与碰撞行为返工；另有取消运行。 |
| snake15 | 3 | 0/2/1 | 8.1～238.4 | 57,494 | 两次 503 失败，另一次 GLM 长推理后取消。 |
| snake16 | 2 | 0/1/1 | 675.9～900.2 | 0 | GLM 长推理且无完整响应；一次总超时、一次取消。 |
| snake17 | 1 | 1/0/0 | 249.4 | 472,835 | 多个 rand/Step/入口定义遗漏；房间列表测试失败。 |
| snake18 | 1 | 1/0/0 | 174.0 | 709,941 | 构建目标错误重复失败；语法、handler 与页面定义遗漏。 |
| snake19 | 2 | 0/0/2 | 55.8～556.3 | 0 | 两次 GLM 请求在完整响应前取消；零上报 usage 不代表零消耗。 |
| snake20 | 3 | 1/1/1 | 251.6～900.4 | 377,361 | 默认推理总超时；自适应后仍有编译/测试接口和业务断言返工。 |
| snake21 | 5 | 4/0/1 | 2.0～310.3 | 2,945,799 | 重复路径批次拒绝后补查；零散定义修复、晚补测试；多次真实不可玩反馈，含一次仅排查未记录新修改。 |

归纳与处理：

1. **服务与推理等待**：GLM 默认长推理在 snake16/20 达到 900 秒总超时；503、404、取消和未知回执必须分别报告。保留既有 GLM 自适应适配、有限 503 退避和动态端口；不对未知外部执行自动重放，不把取消或未上报 usage 当作低成本成功。
2. **实现与测试的接口不一致**：9 次运行在测试命令中遇到编译失败，涉及 snake2/7/9/10/12/14/20/21。本次在混合实现/测试提案的自动 build-only 验证中增加 Go `test -c`，提前编译暂存测试包，最多选择 8 个包；不运行 init 或测试函数，scope 为 `test-compilation-only`，test_status 仍为 not-run。保留测试单独修改延后到完整验证的策略，以及最终全量测试。包清单是计划，`test_compile_plan_complete` 不代表执行或通过，必须看各命令回执；大型项目未选中的包仍由完整验证覆盖。
3. **协议边界缺少证据**：snake21 初次完整测试通过后仍出现加入房间、渲染和 WebSocket 分帧问题。追加通用 client/server 契约测试指导，要求覆盖实际选定的编码/帧/请求到处理器的链路，复用已有协议库；不规定必须使用 WebSocket，也不规定阶段数。不把 handler 单测、模拟 DOM 或 HTTP 就绪升级为真实浏览器验收。该指导是模型提示，不能证明每个未来项目都已覆盖；真实浏览器未执行时继续明确待验收。
4. **拒绝、补读与零散修复**：保留已实现的原批次索引检查/重放、缺页与目录反馈、版本去重、已知诊断集中反馈。固定 1～3 阶段提示已回退；本次不恢复机械拆分或无条件重复全量验证。
5. **交付可核验性**：snake21 的 cdf135b 排查运行有 13 次模型响应、无 staged_revision_persisted 记录，随后仍有不可玩反馈。增加 `mutation_batches_this_invocation`、保留提案数、与本次执行起点相比是否变化；本次执行未修改时在实现/修复最终说明中明确写出，历史待审查提案不算本轮新修复。恢复执行也按“本次执行”口径记录，不抹除历史工作。
6. **对照与归因**：历史日志没有完整实现版本和初始上下文指纹，不能据某一次耗时增减断言某项策略造成改善或退化。新请求记录 generation_implementation、execution_revision、task_sha256、initial_context_sha256、system_prompt_sha256、model_profile_sha256；执行绑定包含模型 profile 和 Responses 状态选项。固定同一任务、初始源码快照、模型、推理、网络/端口政策后多次对照，比较总耗时、未命中 tokens、实际命令时间、修复轮次和协议/业务验收；分别报告初次生成与后续修复，不混为同组。指纹用于排除明显不可比条件，不能消除模型随机性。 implementation 是手动维护的策略版本标记，并非完整构建源码哈希；initial_context 也不等于整个项目源码快照。修改生成或验证策略时应更新 coding_execution_implementation，严格对照还须固定完整输入源码。

可重复审计命令（只读项目，输出必须位于被审计项目之外）：

```sh
python3 coolder/tools/generation_audit.py --workspace /Users/zsx/work/github/fly-speed/var/workspace --prefix snake --output /tmp/snake-generation-review.json
```

审计区分实际 journal 问题与请求归档中保留的历史错误，归档重放不重复计数；缺少耗时/终止事件/版本记为 unknown 或数据缺口，未检查进程时不把日志尾部当作运行失败。命令回执去重避免把验收复用算作再次执行。记录 usage 冲突时保留较大的已记录值并标注不一致；不把它当作精确账单。报告不保存原始用户提示、源码、提供商密钥或错误正文。

新增验证：`coolder/tests/generation_audit_test.py` 覆盖未知数据、usage 清零、回执去重、局部验证不能冒充完整验收及审计只读性；`coolder/tests/test_compile_checkpoint_test.py <coolder binary>` 验证测试接口错误提前暴露、`-c` 不运行 panic init、测试单独修改仍延后、最终完整验证和新审计指纹/真实修改计数。`ai_coding_efficiency` 覆盖无修改交付说明与非游戏 client/server 测试指导。历史项目和运行中的服务不由本次审计修改或重启。

最终兼容回归还发现批次恢复重构丢失了 `proposal_batch_unparsed` 错误标记，使 completed 非 JSON 内容未触发既有兼容分支并再次请求模型。本次恢复解析专属标记，并用实际工具调用与本地主机端到端回归覆盖；无效 saved_batch、路径/权限和容量错误继续严格拒绝。真实 Go 夹具中两次测试编译共约 3.3 秒，仅一次全量测试；该数据只反映本地夹具成本，实际生成收益仍需同条件多次对照。


### snake22 修复反馈链调整（v18）

snake22 暴露了错误测试、强制补读扩大和 Go 失败停滞识别缺口。生成策略版本更新为 `libai-coding-execution-v18-repair-evidence`，推理启用/关闭选择保持原义。

- 修复提示先核验断言真假方向、测试前置条件、边界条件和时钟单位是否符合用户需求；有证据时允许纠正错误测试，并说明依据，继续覆盖原需求及有效异常路径。不得删除失败用例或放宽断言来冒充修复。模型仍须判断具体契约，运行时不自动改写测试预期。
- 必须核对的源码限制为实际诊断/失败测试文件，以及存在的同名 Go 实现文件（`value_test.go` 对应 `value.go`）；不再因共享目录或持续失败就强制重读全部修改。其他实现、依赖与调用方仍可按需精确读取。已有源码只按当前私有草稿内容哈希复用，版本改变仍须刷新。
- 当前请求中已有的完整源码不再占用额外供应的 32 KiB 预算；预算用于缺失源码，大文件仍走显式读取。校验、路径权限和最终完整测试要求保留。
- 三个不同草稿上的同一 Go 测试持续失败时，与 CTest 一样标记 `persistent_test_identities`，继续给出失败输入、上次修改及实际诊断，要求基于新证据调整修复方向；不能以等价改写或一个局部通过宣称完成。
- 实现/修复轮次要求简短说明后调用工具，不因上下文压缩重新输出会话回顾；工具交互中的说明及保留的工作说明限制到 1536 UTF-8 字节，源码参数与工具回执不裁剪，最终答复不受此限制。分析/验证任务保留原有说明范围。该机制限制重放开销，不能保证任意模型不输出长篇文本。

回归：`ai_coding_efficiency` 覆盖精确必读范围、已有证据不占新增预算和 Go 连续失败识别；`coolder/tests/test_contract_repair_test.py <coolder binary>` 使用本地模型夹具和真实 Go 测试，验证错误断言修正、不强制读取同包无关文件、关闭推理、完整验证以及长说明裁剪不影响源码参数。夹具通过不等于真实模型耗时或生成质量的保证；历史项目未被直接修改。


### 三种推理模式调度与共同验证修复（v19）

版本为 `libai-coding-execution-v19-mode-aware`。调度依据当前请求用途、最近实际验证及不同修订上的持续失败；不再因 large 标记、待读文件或任意一次失败直接升为高强度。

- 手动启用：保持启用，未指定强度时普通读取、明确编译诊断和一般实施使用 low。初次实现或行为契约修复保持所需输出空间；同一失败在不同草稿上持续出现时才临时 high，同一执行内的同一失败证据只升级一次。用户明确设置的强度不覆盖。
- 手动禁用：保持禁用。反馈继续提供当前源码、失败输入、实际诊断、上次修改和测试契约检查，允许有依据地纠正错误测试；不偷偷打开推理。
- 自适应：初次实现和实际业务测试失败用低强度推理，普通实施、已有证据补读、明确编译错误及环境/未知失败采用关闭或提供商支持的最低模式。持续行为失败临时 high；对同一失败证据的后续读取和实施不重复 high。新的验证通过后降级，即使仍等待最终全量确认。GLM 继续按已有能力规则映射，不发送不支持的关闭参数。

`model_request_started` 增加 `reasoning_trigger`：source_inspection、compiler_diagnostic、persistent_behavior_failure、behavior_contract、environment_or_unclassified_failure、initial_implementation、routine_execution；连同最终出站 thinking/effort 和版本记录用于对照。low 是提供商参数而非独立推理 token 硬上限。沿用已有常规首轮输出上限、显式强度和输出预算恢复，不引入未声明的提供商参数；不能保证任意模型的推理时长。

初始有界源码快照补充 README、模块/构建清单和 stdafx.h，仍只读根目录及原有的浅层目录、16 个文件/16 KiB 源码预算。模型优先复用这些脚手架与当前暂存接口、状态和协议约定，缺少页面才定向读取，不强制额外设计或固定阶段。

已知测试失败后的自动检查优先使用安全定位的 Go 包；如果诊断仅有裸文件名、范围不明确或不是可映射的 Go 测试，则运行完整验证并记录 `origin=automatic_repair_full_test`，不能降成仅编译。测试单独修改也遵守未解决失败的重测要求；定向通过仍须最终完整测试，完整测试通过可直接复用其证据收尾。

snake22 的最后一次成功检查点 transcript 已达 258406 字节，接近 256 KiB 限制，而工具副作用可以在轮末压缩前再次保存。公共保存入口现在对超限历史先做有界恢复上下文压缩，保留最近工具结果；原始需求、暂存源码、工具回执和待交付原生结果仍以独立字段原样保存，不放宽存储容量/路径/收据检查。`progress_context_compacted` 标明压缩前后字节。此修复覆盖历史超限路径，不代表所有检查点错误都属于同一原因。

回归：`ai_coding_efficiency` 覆盖模式切换、单次升级、显式强度、脚手架预读及超限保存/加载（含精确提案和原生结果）；`coolder/tests/reasoning_modes_repair_test.py <binary> <disabled|enabled|adaptive>` 用真实 Go 工具链和本地模型检查实际出站 effort、重名测试文件的完整验证回退、错误修复及直接收尾。既有 GLM 参数、关闭推理修复、编译检查点和批次恢复继续回归。夹具用于验证控制流；性能收益与业务可玩性需要固定任务、模型及配置的真实多次对照，不预设固定加速比例。

DeepSeek 无状态原生历史包含无推理调用时，重新启用推理不能直接重放缺少 reasoning_text 的旧调用。v19 使用当前有界源码、只读证据和完整任务契约重建请求链，记录 `reasoning_history_rebased`；不编造历史推理、不重放工具执行、不修改副作用回执。保留超预算恢复的既有保护，最终参数仍按提供商能力映射。

审计报告增加逐轮 `effective_thinking_counts`、`reasoning_effort_counts`、`reasoning_trigger_counts`，以及请求链重建、保存入口压缩次数；历史缺少字段记为 unknown，不能把用户选择自适应等同于每轮都开启推理。


### 验证范围与收尾一致性（v20）

策略版本 `libai-coding-execution-v20-validation-scope`，适用于所有推理模式：

- Go 聚焦测试仅在失败范围可完整映射时使用。超时、panic、fatal error、signal、诊断截断，以及输出中未被源码诊断覆盖的失败包，均回退完整验证。验证器显式记录 `diagnostic_truncated`，不能把缺失诊断当作没有其它失败。
- 局部验证保留上一次完整失败范围，不用局部结果覆盖它；局部新发现的未知范围会要求全量验证。只有完整验证通过才清理待修复失败；局部成功仍需一次完整验证。
- 自动修复触发的完整验证与显式验证共用成功状态、源码/基线/执行配置绑定缓存和一次验收总结。自动完整通过后不再要求模型重新验证同一版本；局部成功或 build-only 成功不能冒充完整通过。源码、基线或执行配置改变后仍需重验。
- 用简短通用 `behavior_contract` 替代关键词驱动的场景长清单：正常路径、首次与持续更新的有效状态、失败/恢复、相同时间下的行为差异、上下文隔离与真实组件边界。压缩重复执行指导，保留网络政策和真实浏览器验收边界。这些指导不是强制覆盖证明，不保证模型一次实现正确。

回归：`ai_coding_efficiency` 覆盖混合超时/断言、未映射包、截断和局部失败范围保留；`coolder/tests/mixed_failure_scope_test.py <binary>` 通过本地模型夹具和真实 Go 工具链产生两个包的断言失败/panic，验证修复期间不漏包、自动完整通过后只总结一次且不重复验证。既有三种推理模式、行为失败和编译检查点回归继续适用。夹具验证控制流，不代表真实模型质量。没有修改历史生成项目，也不把后端测试或 HTTP 就绪当作浏览器可用性。

### 源码接口复核与中间构建（v21）

策略版本 `libai-coding-execution-v21-contract-review`，不改变用户选择的推理模式、网络政策或浏览器验收开关。

- 固定验证器对同一份物化草稿增加 `source_contract_review`。以有界词法扫描发现 Go 无 JSON 标签的导出字段与 JavaScript 小写属性读取、手写 WebSocket 握手/帧读取、测试内重复丢弃局部测量函数结果，给出源码位置、相关消费端位置和需要执行的检查。扫描跳过注释与非代码字符串，最多 128 文件、1 MiB、12 条线索；单文件上限 256 KiB。不能完整读取时记录 `scan_complete=false`。当前只支持 Go/JavaScript/HTML 线索，不是跨语言类型分析、确定缺陷或覆盖证明；自定义序列化与动态前端等情况仍需真实测试。
- 线索随原有自动编译/完整验证反馈，并纳入现有最终复核及任务验收记录。不会增加强制设计或审查轮次，不因静态线索反复重跑测试，也不将它们标成测试失败或通过。模型应对实际适用路径增加可执行断言：生产端实际序列化结果和消费字段、实际输入协议、共享状态、测量结果比较。已有 HTTP/内部类型测试不能替代另一条 WebSocket/浏览器路径。真实浏览器缺失仍明确保留未验证状态。
- 对仅含安全目录创建及 `go build -o <relative-output> <configured-target>` 的简单 build.sh，中间编译使用已配置的 Go 工具链；仍编译其它变更包及测试二进制。最终完整验证照常执行 build.sh、固定构建、测试和已配置验收。包含代码生成、环境变量、额外参数、不同目标或其它命令的脚本保持原行为；聚焦编译修复不跳过脚本。不使用未经验证的构建缓存、不删掉最终交付检查。

回归：`ai_source_contract_review` 覆盖源码线索、注释/标签/自定义序列化反例及严格脚本识别。`coolder/tests/contract_boundary_generation_test.py <binary>` 使用本地模型夹具和真实 Go 工具链，确认初始代码通过已有检查后，真实 JSON 消费字段和掩码帧测试失败、修复后通过、交付脚本仍执行且没有新增收尾轮次。静态复核和夹具不保证任意真实模型一次生成正确，也不建立全部需求覆盖。

### v22：失败状态、托管验收和构建证据复用

- 仅当定向 Go 测试覆盖全部已知失败包且通过时，清除旧失败报告、修复契约和补读要求，标记 `focused_tests_passed / requires_full_validation`。局部成功不能成为最终验收；范围不完整或有 panic/超时仍需全量验证。全量 Go 测试通过后，后续 HTTP/功能失败不会复活旧 Go 错误。
- POSIX 宿主对已配置 HTTP 服务的 `functional.acceptance` 管理启动、就绪、Node 测试和进程清理。`tests/acceptance.test.cjs` 使用 `LIBAI_HTTP_BASE`、`LIBAI_HTTP_PORT`、`LIBAI_ACCEPTANCE_SERVICE_MANAGED=1`，不自行启动/终止服务。动态端口沿用 `.libai-http-port-env` 契约；旧固定端口项目保留原行为。脚本仍在沙箱内运行，失败保持失败，不因就绪成功而吞掉断言错误。Windows 暂无该托管入口，提示使用进程内测试并明确未验证部分；Linux 实现沿用 namespace/Landlock/seccomp，本轮运行验证平台为 macOS。
- 工具格式拒绝前不执行批次，原始正文和原始参数保存在私有 responses 归档。可恢复的 `workspace.propose_batch` 提供内容散列引用、解析错误字节位置、`inspect_raw` 分页和显式 `raw_edits`。每次替换必须唯一匹配；解包只允许原 `workspace.propose_batch`，仍完整执行严格 JSON、工具 schema 和原子文件批次校验。不会自动猜测修复、丢弃重复键或执行其他工具；原读文件证据保留。暂存引用限当前运行。
- POSIX 工具 HOME、Go 编译缓存和命令临时目录固定到 `.libai-build`，避免系统默认缓存目录污染源码指纹。构建复用限同一次运行的普通 Go 构建和输出到 build/ 下的可识别简单 Go 构建脚本。比较当前输入树（仅排除独立 `tests/acceptance.test.cjs`）、正式源码基线、执行版本、限制、工具及参数、产物散列和宿主持久回执。验证后仅在非产物文件指纹不变时记录新产物快照，避免下一轮重新物化把有效产物丢弃；保持不变的既有 mkdir 提案支持后续文件增量修改，新增/删除/不可信目录仍完整重建；测试若改写源码则仍重建草稿。源码/依赖/配置/工具/产物变化即失效；cgo、嵌入资源、复杂脚本或无法完整扫描时保守重建。Go 测试、HTTP 就绪和功能断言仍真实执行。报告区分 `commands_reused`、`commands_executed_this_call`，复用记录保留原回执，不虚增耗时。
- 通用行为指导要求在适用时通过真实创建入口检查至少两个参与者的初始资源和持续进展，以及启动实际调度器后修改配置的效果。源代码提示可跨文件识别手写 WebSocket 握手/帧读取，并提示可能未更新的运行时定时器；这些均为有界提示，不是类型分析、执行证据或新的强制评审轮次。最终只对已执行断言作验证声明。

回归入口：`managed_acceptance_test.py`（并发端口、服务生命周期、沙箱）、`managed_validation_reuse_test.py`（真实 Go/Node 与缓存失效）、`rejected_response_recovery_test.py`（原文局部恢复）、`behavior_generation_test.py`（真实初始资源冲突/运行中变速失败→修复）、`ai_coding_efficiency`、`ai_source_contract_review`、`ai_proposal_response_recovery`。集成模型使用确定性本地响应，编译、测试、HTTP 服务与沙箱均实际执行；这些回归不能替代真实模型生成质量和端到端耗时测量。

### v23：减少补读交互并预留最终验证

策略版本 `libai-coding-execution-v23-bounded-investigation`，适用于启用、禁用和自适应推理；不改变用户的推理选择。

- 上下文压缩、兼容协议重建和断点恢复时，按实际保留的文件版本和字节区间重建已读覆盖。修复控制器主动提供的完整源码同样计入已读证据。保留页原先的 `low_value_read` 标记不会导致进展监督器丢失其证据；换偏移重复读取已覆盖内容不能重置无进展计数。版本改变、真实未读区间和被淘汰的源码仍允许正常读取。
- 实现任务连续调查达到 12 次工具调用后进入收敛阶段；已有编译或测试失败的修复阶段为 6 次。保留最多 4 次额外读取/搜索，用于缺失证据；明确的待补读修复证据和浏览器取证继续遵循原有规则。源码发生有效修改后恢复正常工具选择。分析、验证任务不受实现收敛策略约束；不能用这个机制把未经验证的修改宣布成功。
- 编程运行从既有总额度中预留 1 次固定验证，模型批次不能消耗该额度。到达模型工具额度边界后，宿主直接验证当前草稿或复用匹配回执并交付真实结果，不另发一轮模型请求。总额度不增加，失败仍报告失败，无验证能力仍明确披露；不会超额度补跑。分析、验证任务、普通聊天和文本预览保持原有额度行为。
- POSIX 的固定 `go.test` 允许现有沙箱策略下的回环网络，支持 `httptest` 与系统分配的临时端口，禁止外网；构建命令的网络策略不改变。定向测试继承同一命令策略。Windows 不启用此能力，能力快照仍反映其实际限制。测试启动与清理由测试自身负责，宿主继续约束进程树和执行时限。该机制不等于浏览器验收。
- 修复指导先核对失败用例的真实入口、初始化及 worker/event-loop 生命周期，再修改实现，避免未启动工作循环却不断重写计时逻辑。保留原有断言强度要求。

回归：`ai_coding_efficiency` 检查覆盖保留、重叠区间、版本改变、源码淘汰和有界补读；`bounded_interactions_test.py <binary> reads <disabled|enabled|adaptive>` 用本地模型夹具故意重复读取，跨压缩后收敛至修改、完整测试和一次总结；`budget` / `budget-failure` 检查 8 次总额度内 7 次模型工具加最终验证，分别保留通过/失败结论。夹具实际执行 Go HTTP 回环测试，模型响应采用固定数据，不能据此承诺真实模型的固定轮数或耗时。运行验证平台为 macOS；Linux 沿用现有 namespace/Landlock/seccomp 实现。

### v24：接口复用、局部修复与连续验证

策略版本 `libai-coding-execution-v24-coherent-repair`。三种推理模式共用这些机制，用户的推理选择保持不变。

- 从当前未拒绝草稿提取最多 4 KiB 的声明导航，包含文件指纹、行号和源码片段，排除测试及二进制文件；在下一次请求中直接提供，不增加模型调查轮次。复用 `workspace.outline` 的词法提取，覆盖其支持的语言；索引明确标为不完整，不得将缺失条目判为不存在，也不是编译器或需求契约。引导模型沿用已有接口与初始状态/动作/结果约定，确需变更时同批协调调用方及各层测试。分析、验证、聊天及已通过全量验证后的总结阶段不追加索引。
- 精确补丁匹配失败时，使用失败 old_text 中唯一精确行定位修改前的真实源码窗口，提供 SHA256、字节偏移和从 1 开始的失败替换序号。找不到唯一锚点时返回前缀。定位只提供证据，不进行模糊写入；此前已匹配的替换仍整体回滚。引导修正片段后重试相关补丁，避免整文件重新生成。
- 已知失败 Go 包全部定向通过后，先提交原修改的执行记录，再由宿主执行一次 `coding.repair_full_validation`，无需模型专门请求验证。该验证独立消耗一次既有工具额度，沿用取消、固定验证器、指纹、执行回执和恢复机制；其他包失败继续交给模型修复，完整通过才进入最终证据总结。不能把部分包通过或没有验证能力当成完成。编译专项、分析及验证任务不扩大检查范围。
- 行为测试指导增加初始状态及生命周期一致性，要求断言实际下游状态；调用计数或配置回显不能替代运行效果证据。仍不将词法规则当成自动验收，不因缺少某种测试而无限扩充测试。

回归：`coherent_repair_test.py <binary> <success|regression|budget|budget-regression> <disabled|enabled|adaptive>` 使用模拟模型、真实 Go 编译与测试，覆盖局部通过后跨包回归、最终验证复用和预算边界。`ai_coding_efficiency` 覆盖多语言声明、版本替换、上下文上限及失败补丁回滚；`ai_coding_zero_recovery` 验证新回执类型的存储/恢复及未知类型拒绝。真实模型的质量与节省幅度仍需新项目验证。
