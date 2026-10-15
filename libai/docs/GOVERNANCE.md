# 通用 Agent 工具治理

通用接口用于 portable Agent SDK；C++ 编程 runtime 也在执行工具之前校验当前请求目录的 Schema。
原生调用整批检查，旧 JSON 文本协议检查原始参数；不先把数字等标量转换成字符串。
拒绝产生修正提示与检查点，不扣执行额度；连续无工具进展仍受已有停止规则限制。
显式final协议中的文件提案也先校验原始JSON与固定Schema，再进入原有领域验证。
这不是Provider原生结构化输出。非空最终提案已接入下述持久意图，自动验证进程和
其它编程修改路径仍需继续统一。

## 编程 final 协议的前置校验

原C++编程协议对显式type=final对象检查固定受限Draft4 Schema，并检查原始JSON
重复键、UTF-8、资源限制及所有字符串的NUL。不把数字等标量转换为代码内容。
最多20份changes，超量整批拒绝而非取前20份；任一Schema错误都在合并草稿前
拒绝整个响应，并反馈模型修正，不执行工具或消耗实际工具额度。

text必需且非空；completion_summary/memory_summary/session_title为可选字符串。
changes为可选数组，每项只接受operation/path/target_path/content/reason。默认write
要求显式字符串content（允许空字符串），delete/mkdir/replace_empty_file_with_directory
要求显式operation，move还要求非空target_path。字段按UTF-8字节限制：path/target_path
2048、content256KiB、reason1000；后续路径授权、来源和提案验证仍然执行。
模型不能提供generation、review_status或accepted_by等宿主审核字段。

非空最终提案作为内部coding.final_proposals批次计入一个累计工具额度。额度不足
时不合并；先保存running及原响应参数摘要，再执行原领域验证/合并并分配草稿
版本。完整草稿与上下文成功保存后才确认done，随后才进入自动构建/测试。
确认失败保留未知意图，正常恢复及同会话换计划均不能绕过；可信宿主可使用
[检查点确认接口](DURABLE_EXECUTION.md#原编程检查点宿主确认)。该内部名称不是
当前编程Agent公开的模型工具，不承诺自动验证进程也已取得同样的意图保护。

requirement_progress为最多12项的模型实施报告，每项仅允许requirement/status/
evidence/remaining。它继续被标记为未经独立验收，不能改变TaskContract的宿主或
人工证据。合法Markdown JSON围栏仍兼容；最终文本和已暂存成果不能代替构建测试。

## 参数验证与副作用边界

注册工具时编译 Schema；不支持或无效的 Schema 拒绝注册。
模型返回后先检查调用次数、字节预算和调用 ID，再校验整批工具参数。
任一参数失败时整批不进入授权或执行，返回对应工具错误供模型修复。
全部通过后按顺序执行，授权回调发生在 Schema 验证之后、工具执行之前。
这只防止无效参数产生副作用，不提供事务回滚或崩溃后 exactly-once 保证。

支持受限 JSON Schema Draft4：类型、对象属性、required、additionalProperties、数组、
数值/字符串约束、enum、组合、dependencies 和本地 JSON-pointer 引用。
不解析远程引用；format 作为注解，不作格式断言。较新草案的断言关键字拒绝接受，
避免静默忽略。扩展注解使用 x-*。Draft4 的 required 不能是空数组，未要求字段时省略它。
JSON 有 2 MiB、64 层、65,536 节点限额，拒绝重复键和无效 UTF-8。
引用验证有深度限额；无进展的引用环拒绝。失败诊断包含关键字和指针，不包含输入值。
这些限额不等于任意不可信 Schema 的硬 CPU 隔离；工具目录应由受信任宿主注册。

## 各语言入口

| 语言 | 接口 |
| --- | --- |
| C++ | libai::cpp::validate_json_schema(schema, instance)、governance_call(operation, arguments) |
| C | libai_governance_call；返回字符串用 libai_string_free 释放 |
| Java | Governance.call(operation, argumentsJson) |
| Python | governance_call(operation, argumentsDict) |
| Rust | libai::governance_call(operation, argumentsJson) |
| Objective-C | [LAICodingService governance:arguments:error:] |

capabilities 返回 SDK 的治理能力；schema.validate 接收 schema_json 和 instance_json 字符串。
返回 ok、valid、code、keyword、instance_pointer、schema_pointer。
验证不需要项目目录、模型凭证或网络，也不执行任何业务工具。

```json
{"schema_json":"{\"type\":\"integer\"}","instance_json":"42"}
```

## 执行控制

模型仅在显式 retryable 错误时按 max_model_retries 重试；不自动重试有副作用的工具。
重试计入调用预算。取消和期限为协作式，宿主长回调和传输必须主动观察。
工具结果超过 max_tool_result_bytes 时截断并标为错误。
流式可见文本受剩余上下文字节预算限制，超限片段不发送到显示回调。
流式限制不提供传输层内存隔离；宿主不能忽略停止请求后无限累积数据。
model_start/end、tool_start/end、finished 可用于追踪；不是完整分布式 trace 平台。

验证器来自固定版本 RapidJSON，使用私有命名空间和宏前缀避免与宿主 JSON 库冲突。
源码及许可证见 ../sdk/vendor/rapidjson/SOURCE.md 与 LICENSE.txt；二进制安装保留这些说明。

模型能力配置、持久执行日志、MCP、结构化最终输出、DAG工作流和可插拔检索已提供独立接口，
见下文及对应文档。仍需可组合 middleware、编程 runtime 的全部事件/策略统一，以及
各 APP 的完整接入；SDK 提供接口不等于所有宿主已采用这些执行规则。

## ModelProfile v1

Agent.configure_model 接收宿主声明的版本化 JSON；C 使用 libai_agent_configure_model，
Java 使用 configureModel，Objective-C 使用 configureModel:error:。Python 接收字典，
Rust/C++/C/Java 接收 JSON 文本。只能在空闲时修改；失败保留先前有效配置。
SDK 本身沿用宿主同步要求，不自行给 header-only Agent 增加锁。

```json
{"version":1,"id":"host-selected-model","tool_calling":true,"multiple_tool_calls":false,"max_output_tokens":8192,"max_context_bytes":524288}
```

version 必须为 1；id 为可选注解，不据此推断模型能力。两个 bool 默认 true，数值省略时
仍使用运行预算。显式数值必须为 1 到 1 GiB 的整数；未知字段、重复键、错误类型拒绝。
禁用工具时不向模型提供目录，模型仍返回工具则拒绝执行。禁止多调用时整批拒绝，
不会先执行第一项。multiple_tool_calls 指单轮调用数量，不表示工具会并发执行。
输出上限限制每次请求生成量，不替代整次运行的 token 预算；上下文上限取模型与运行
预算的较小值。用量依赖模型报告；上限并不是精确 tokenizer 或强制中断任意回调。
C/各语言 JSON 请求包含 model_profile；独立 C++ ModelRequest 提供 model_profile_json。
内置官方 OpenAI 协议发送对应 parallel_tool_calls；其他协议由适配器处理，执行边界
仍拒绝越过能力限制的调用。宿主声明必须来自可信配置，模型不能自行扩大它。

通用 SDK 与六语言封装支持该配置；共享 Provider 存储与三个 APP 的运行路径也已接入。
Provider 保存 API 的 model_profile_json 字段是 JSON 文本字符串，读取同名字段。
省略或空字符串更新保留已有能力配置；显式 {"version":1} 恢复默认。
旧的无此字段记录使用兼容默认，凭证仍按原来的加密方式保存。
保存前校验，存储中非法配置读取失败关闭。配置修改影响之后的请求；恢复时采用当前
Provider 配置。三个 APP 管理页面的“模型能力与限制”可配置工具调用、多调用和输出/请求字节上限。
上限留空恢复运行预算；编辑保存保留 profile 的宿主注解。授权 API 与库接口同样可用。

输出和工具能力在每次请求构建中应用，包括恢复/协议重试；传输前检查序列化 JSON
请求字节上限，包含协议包装与编码图片。SDK 的逻辑上下文字节检查仍独立生效。
ProviderClient 对违规原生工具返回作拒绝，APP 分派边界继续验证工具与领域参数。
不宣称已经支持视觉、Embedding、原生结构化输出等全部模型能力。


## 轻量任务契约评估

`tasks.evaluate`也可通过本页各语言的governance入口调用，不需要完整编程后端、
项目目录、ACL/OpenSSL或模型服务。SDK `<libai/task_contract.h>`提供
`parse_task_contract_json`（独立类型）及`evaluate_task_contract_json`（JSON结果）。
C++ facade、C ABI、Python、Rust、Java及Objective-C使用既有governance调用接口。

输入沿用TaskContract v1：loop_finished、loop_outcome、artifact_revision、requirements
及generation/build/tests/review证据。requirements默认四项均需要；明确的分析任务
可由可信宿主设为false。每项证据含state/revision/authority/reference。验收必须有
completed循环、当前成果版本的host生成/构建/测试证据，以及human审核证据。
recovery_required是合法但未成功的循环结果，不再返回非法状态错误。

结果为ok/version/accepted/checks/unmet。未提供字段采用保守默认，错误类型、重复键、
非法版本/状态/来源、NUL或资源超限拒绝。忽略输入中的accepted/checks等派生注解，
不能用模型自报结果替代证据。JSON仍有2MiB、64层、65,536节点限制。
完整后端tasks.evaluate及tasks.describe_run使用同一解码与接受规则。

评估器不访问reference指向的报告、不启动编译或测试、不修改审核状态，也不核实
宿主提供的证据真实性。它只能由受信任宿主结合真实执行记录调用，不应作为模型
可写验收证据的工具。独立C++、Java、Python、Rust和Objective-C已提供专用任务证据值类型
及宿主记录入口；C ABI提供共享JSON入口。详见各语言README，宿主仍须核实证据来源。

## 执行快照诊断

`executions.inspect` 通过已有六语言治理入口提供只读日志解析，capabilities 的
execution_inspection=true 和 operations 列表可用于能力探测。输入、恢复提示与
信任边界见 [持久执行诊断](DURABLE_EXECUTION.md#宿主只读恢复诊断)。该操作不
修改状态或调用业务工具，不属于无需鉴权的模型工具。

## 持久工具事件关联

原有 tool_start/tool_end 是调用与返回观察，tool_end 不代表结果已落盘。
启用执行存储后新增 tool_intent_committed（意图提交成功、尚未进入处理器）、
tool_result_committed（结果与对话提交成功）、tool_result_reused（恢复读取已保存
结果，包括完成缓存）。提交失败不会发出对应的已提交事件。事件回调不是外部
业务收据，不能据此自动确认未知效果或授予权限；真实日志和CAS仍是恢复依据。

SDK事件及C/Java/Python/Rust/Objective-C的JSON事件含 execution_scope、
idempotency_key、journal_revision、effect_state、attempts。无持久工具的事件
键为空、attempts=0；普通tool_start可能观察ready，tool_end仍可能观察running。
新提交/复用事件的字段对应日志中该次调用，不用当前cursor推断已结束调用的键。
error对应保存的工具结果标志，拒绝授权或输出截断的复用结果仍为error。
复用不触发处理器，也不消耗新的模型/工具调用；事件不是可靠投递队列。

新三类事件的观察异常不会撤销提交或授权执行。宿主主动设置取消令牌仍生效，
意图事件之后会重新检查取消/期限。语言绑定原有回调异常捕获/取消规则保持原样，
不能将“观察异常不回滚”理解成忽略宿主取消或吞掉所有语言异常。

独立C++ Event/RunOptions对象布局保持不变，Event::kind_t新增三种枚举。
在该Agent的on_event回调线程内调用 Agent::event_execution() 可取得独立的
EventExecution值快照；回调外或其它线程调用拒绝，复制的值可在回调后保存。
各语言观察者应允许新增事件种类，并可通过capabilities.events探测支持列表。
