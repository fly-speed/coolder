# MCP 工具客户端与 Agent 接入

本阶段提供可插拔传输的 MCP 2025-11-25 tools 客户端，入口 `<libai/mcp.h>`。
通用 SDK 与独立 C++ facade 已提供拥有连接生命周期的接口；C ABI 也已提供连接句柄；Python已提供拥有连接生命周期的接口；Rust也提供安全拥有接口；Java也提供独立接口；Objective-C也已提供Foundation拥有接口。

本文各语言章节保留分阶段实施和测试记录，早期“其它语言待实现”描述当时状态。
当前六语言入口均已存在；内置Streamable HTTP、完整Schema方言、高级协议功能及
设备端运行验收仍未完成，不能将接口齐备当作完整MCP兼容性证明。

## 传输与协议边界

宿主实现 `mcp_transport_t::request / notify / close`，每个客户端独占一条逻辑连接。
request 返回当前请求的 JSON-RPC 响应；notify 不等待响应；close 必须可靠、幂等、
不抛异常。宿主负责 stdio/HTTP 消息边界、响应路由、认证、会话 ID、服务器 ping /
日志等消息处理和连接清理。HTTP 请求须按 context.protocol_version 发送协商后的
MCP-Protocol-Version。库提供下述 POSIX stdio 适配器；Streamable HTTP 适配器仍待实现。

客户端先发送 initialize，核对协议、tools 能力与 serverInfo，随后发送 initialized。
不支持其它版本时失败关闭，不自动重连或重试。请求使用匹配的整数 ID，拒绝批量
响应、重复键、错误 ID、同时 result/error 和损坏结果。JSON 使用现有2MiB/深度64 /
65,536节点限制。客户端与传输回调不可并行或重入访问；取消句柄可由其它线程触发。

发现最多16页/256工具，拒绝重复名称与循环游标。名称限定协议推荐的ASCII字符。
失败刷新禁用旧目录的调用；绑定后的定义被固定，在同一客户端锁内校验当前定义。
要求异步 taskSupport=required 的工具不能调用；没有实现 tasks、sampling、elicitation、
roots、resources/prompts 操作或服务器通知订阅，不向服务器宣称这些能力。

期限取单次 timeout 与宿主总 deadline 的较早值，分页共享同一总期限。默认30秒，
可配置至24小时；传输必须配合协作取消与期限，库不能强制中断任意阻塞回调。
超时/取消导致传输抛异常时，对仍被认为在途的非 initialize 请求发送尽力取消通知。
initialize 不被取消，迟到响应被拒绝；取消不证明外部效果未发生。

## 工具绑定

```cpp
#include <libai/mcp.h>
// transport 由宿主实现；model 与 Agent 使用正常 SDK 接口。
auto client = std::make_shared<libai::mcp_client_t>(transport);
client->initialize("my-app", "1.0");
auto catalog = client->list_tools();
libai::mcp_binding_t policy;
policy.parameters_schema = R"({"type":"object","required":["query"],"properties":{"query":{"type":"string"}},"additionalProperties":false})";
policy.execution_revision = "trusted-endpoint-and-tool-v1";
libai::add_mcp_tool(agent, client, "search", "mcp_search", policy);
```

MCP 未指定 dialect 时默认2020-12；现有 SDK 校验器是受限Draft-04。客户端保留远程
原始 Schema，检查基础容器，不宣称完整2020-12验证。绑定要求宿主提供审查过的
本地 parameters_schema，不能静默把远程 Schema 当作 Draft-04。远程声明 outputSchema
时也要求本地 output_schema；成功结果中的 structuredContent 必须通过它，否则
返回工具错误。完整 dialect 适配仍待实现。

绑定默认 requires_approval=true、side_effect_free=false，不把服务器 annotations
作为授权、无副作用或幂等性证据。宿主可在审查后改变本地策略。模型先经过现有
整批 Schema 和授权，再调用 MCP。远程内容是数据，服务器 instructions 不自动
加入系统指令。媒体内容保留 JSON，不自动下载 URI、解码或创建文件；宿主负责
多模态映射。模型结果仅保留 content/structuredContent/isError，删除协议位置的
_meta；structuredContent 中的业务字段不被改写。

宿主必须在 endpoint、工具实现语义变化时更新 execution_revision，不能用服务器
自报身份证明可恢复。此版本将协议版本纳入 SDK 执行版本并固定发现的 Schema；
无法自行识别远程服务代码或传输目标改变。

## 持久执行

SDK 工具预算、输出截断、授权、事件、取消与执行日志继续适用。绑定把 SDK
幂等键通过自定义 `_meta["io.coolder.libai/idempotency_key"]` 传递；服务器是否实现
幂等行为由宿主确认，此字段不是 MCP 的 exactly-once 保证。

成功结果提交后恢复复用；写入丢失响应、服务器 isError 或输出契约失败时，默认
保留未知意图等待宿主收据确认，不自动重试业务工具。只读重放须由宿主显式声明。
测试验证真实 Agent 回路、默认拒绝、无效参数无 RPC、副作用只执行一次、完成缓存、
丢失响应不重跑、取消通知、分页/版本/ID异常与私有元数据过滤；使用脚本传输，
没有把它当成实际外部 MCP 服务或 HTTP/stdio 完整兼容性证明。

协议依据：[Lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle)、
[Tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)、
[Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)、
[Cancellation](https://modelcontextprotocol.io/specification/2025-11-25/basic/utilities/cancellation)。


## POSIX stdio 传输

`<libai/mcp_stdio.h>` 提供 `mcp_stdio_transport_t`（macOS/Linux 等桌面 POSIX；iOS/Windows 尚无此适配器）：

```cpp
#include <libai/mcp_stdio.h>
libai::mcp_stdio_options_t options;
options.executable = "/absolute/path/to/python3";
options.arguments = {"/absolute/path/to/mcp_server.py"};
auto transport = std::make_shared<libai::mcp_stdio_transport_t>(options);
auto client = std::make_shared<libai::mcp_client_t>(transport);
client->initialize("my-app", "1.0");
auto tools = client->list_tools();
// 随后按上述宿主 Schema/授权策略绑定工具。
```

使用 posix_spawn 和明确 argv，不经过 shell、不搜索 PATH、不改变工作目录。
默认继承宿主环境；如需最小环境，设置 inherit_environment=false，并用 environment
提供完整 KEY=value 列表。配置必须来自受信任宿主；启动服务拥有当前进程的权限，
此适配器不是沙盒。宿主应将其它私有描述符设置 close-on-exec，不应由其它代码
回收本适配器拥有的子进程，也不应全局忽略 SIGCHLD。

stdin/stdout 按 UTF-8 JSON 单行加换行分帧。stderr 重定向到 /dev/null，避免日志占满
管道造成阻塞；尚未提供日志订阅。读写使用非阻塞描述符和20毫秒轮询检查取消/期限，
每条消息最多2MiB，每次响应最多256条消息、4MiB累计流量。服务器 ping 返回空结果，
其它服务器请求返回 -32601；通知在下一次等待响应时被消费，不提供后台订阅或自动
刷新目录。请求完成后，由协议客户端核对响应 ID。错误不自动重连或重发工具调用。
写入仅在线程范围屏蔽本次 SIGPIPE，不改变应用进程的信号处理器。

close 可重复调用：关闭管道，对仍拥有的进程组发送 TERM，最多等待100毫秒后 KILL，
回收直接子进程。close/析构仍遵循客户端的非并行生命周期约定；取消通过独立取消
句柄发出。进程组外的派生进程不受管理；权限隔离应使用宿主沙盒。

测试启动真实本地服务子进程，验证初始化/initialized、发现、Agent工具循环、ping、
不支持请求、参数原样传递及替换环境；同时覆盖stderr大日志、损坏/超大/过多消息、
错误ID、服务退出、阻塞写、读取超时、取消、SIGPIPE及子进程回收。该fixture是协议
兼容测试，不等于已验证所有第三方 MCP 服务。HTTP、其它语言及设备端验收仍待完成。

消费方 CMake 链接 libai::agent 与 Threads::Threads（find_package(Threads REQUIRED)），
不需要编译 libai 完整运行时。主目录SDK12项测试及只使用安装目录头文件的独立消费
项目均通过；消费项目重复运行真实stdio fixture，无源码目录 include 路径。


## 独立 C++ facade

`<libai_cpp/mcp.h>` 提供 McpClient / McpTransport / McpOptions / McpTool / McpBinding
及 McpStdioOptions；公开类型独立，转换和 SDK 依赖只存在于 libai-c++/src。
McpClient::stdio 提供本地进程，Agent::add_mcp_tool 将已发现工具接入既有SDK执行器。
静态/动态库同样提供这些接口，安装后的消费方仅链接 libai::cpp，不需要安装 libai。

绑定持有底层客户端直到Agent释放，外层客户端对象可以提前释放；显式close影响
所有共享绑定。配置时Agent必须空闲，客户端请求和close不得并行或回调重入。
自定义传输收到的取消对象共享原生取消状态，不依赖公共包装对象的指针身份。
直接call_tool是宿主操作；它不获得Agent的Schema、授权、预算或副作用日志保证。
详细示例见 [C++ README](../../libai-c++/README.md#mcp-工具接入)。

绑定回归覆盖默认拒绝、无效参数无RPC、真实持久键、完成结果缓存、连接失败后新
连接不重发未知写入、取消通知、固定定义变更拒绝及绑定生命周期。真实Python服务
用于stdio调用与Agent循环；Java/Rust/Python/Objective-C的语言级MCP对象仍待补齐。

本阶段主目录验证：C++静态/动态各9项、原生SDK12项测试通过；安装包静态/动态
独立消费项目分别运行MCP/stdio/恢复fixture通过，头文件目录只有libai_cpp，无原始
libai头文件。coolder APP重建通过，iOS arm64模拟器交叉编译通过且无posix_spawn
引用；没有将交叉编译视为设备运行验证，未执行Windows/Linux设备测试。


## C ABI 接入

libai.h提供libai_mcp句柄、回调传输/延迟release、stdio工厂、JSON操作分派，以及
libai_agent_add_mcp_tool。Agent保留底层连接，外层句柄释放不导致回调user悬空；
最后一个owner释放才调用release。显式close关闭所有共享绑定，不自动重连或重发。
参数、结果的字段定义及生命周期见 [C README](../../libai-c/README.md#mcp-c-abi)。

纯C测试覆盖真实Agent治理与持久恢复：默认授权拒绝、无效参数无RPC、幂等键与
结果缓存、外层句柄先销毁、一次close/release、丢失响应重连仍保持未知状态、
取消、回调重入拒绝、缺少/重复/超限响应。stdio回归运行真实Python服务。
JNI/Objective-C构建已登记同一C实现，但这不代表其语言级MCP接口已经完成。


Android stdio按编译目标限制为API28+，API21保留可插拔传输客户端且不引用spawn。
此依据本机NDK spawn.h的__INTRODUCED_IN(28)；低版本不会因新传输符号而无法链接。
主目录C静态/动态各4项、安装包独立C消费测试通过；完整JNI x86_64与Rust arm64
原生库运行同一组MCP回归通过。Objective-C现有6项回归、C++现有9项回归通过。
iOS模拟器、Android API21/28 C库交叉编译通过；Android API21 C++静态库与独立
消费者链接通过，均未执行设备测试。旧NDK21归档工具在当前macOS触发宿主libc++
初始化错误；本次临时构建让归档工具使用NDK自带lib64，并未修改项目或全局工具链。


## Python MCP owning 接口

McpClient / McpTransport 与 Agent.add_mcp_tool 复用C ABI。模块级固定ctypes trampoline
配合按连接登记的状态表，延迟原生release回调移除状态；避免外层Python对象释放后
Agent回调悬空，也避免在release返回途中释放其CFUNCTYPE执行代码。
回调Invocation自动过期并检查线程；外层句柄直接调用期间拒绝并行/重入销毁；Agent绑定独立持有底层连接。取消令牌保留共享
原生状态直到调用结束；Schema/授权/日志恢复规则与SDK一致。stdio调用真实子进程。
细节与示例见 [Python README](../../libai-python/README.md#mcp-工具接入)。

工作区21项Python回归通过，包括5项MCP场景；弱引用/GC核对绑定持有及最终释放，
真实文件日志核对丢失响应后新Agent/新连接不重发，旧原生ABI仍可运行Agent而明确
拒绝缺少的MCP能力。HTTP与其它语言仍待实现。


Python阶段主目录21项测试通过；离线生成wheel后，核对包含mcp.py，并从解包目录
加载模块，使用同次生成的轻量C库重复21项测试通过。仅Python源码/测试/文档变化，
未重启三个APP或修改其运行数据；Java/Rust/Objective-C的专用MCP接口仍待完成。


## Rust MCP owning 接口

McpClient/McpTransport与Agent.add_mcp_tool复用C ABI。创建成功才把稳定Box holder交给
原生release，失败在Rust回收；Agent保留原生连接，外层client Drop不提前释放回调。
可变借用与非Send/Sync接口约束客户端；共享传输借用冲突失败关闭，Invocation不能
安全脱离回调。宿主需保证close可靠且不被外部RefCell借用阻止。
普通unwind panic及析构panic在Rust边界捕获，异常payload的二次析构panic不进入FFI。
panic=abort/双重unwind仍会终止进程；不是隔离任意宿主代码的安全沙盒。
工作区全部Rust测试通过（20项，其中5项MCP），包括真实stdio、授权/Schema、幂等键、
缓存、未知效果新连接不重发、取消/借用/panic和最后owner析构。示例见
[Rust README](../../libai-rust/README.md#mcp-工具接入)。


Rust阶段逐文件同步主目录后，轻量静态及完整动态原生后端各20项测试通过；独立
Cargo消费项目通过真实stdio/Agent绑定/外层owner先释放场景，coolder-rust重建通过。
没有新增JSON/网络crate依赖，没有设备端运行验收，也未据此完成全部MCP或平台目标。


## Java / JNI MCP owning 接口

McpClient/Transport与Agent.addMcpTool复用C ABI；JNI持有transport与回调class全局引用，
缓存方法ID，在当前线程取得JNIEnv，最后native release删除引用。Invocation在Java
finally过期，异常转换为最多4KiB的传输诊断。创建失败回收本次引用，不接管user。
close释放外层句柄，closeConnection关闭共享连接；请求期间并行/重入操作拒绝，
Agent配置必须空闲。详见 [Java README](../../libai-java/README.md#mcp-工具接入)。

工作区-Xcheck:jni测试通过：真实stdio、UTF-8补充字符、后台线程/并发、过期回调、
弱引用/GC核对最后owner释放、Schema/授权计数、缓存、取消和未知写操作新连接不重发。
原有Agent/Governance/Durable/File/StructuredOutput回归通过。授权负例使用实际计数，
不依靠可能被框架转换的回调异常作为失败信号；Rust对应负例也已加强。


Java阶段主目录轻量/完整JNI与独立安装JAR消费项目均通过-Xcheck:jni MCP测试；
既有五组Java回归、Rust五项MCP授权负例加强回归通过。Android API21 JNI交叉编译
通过，coolder-java重建完成，无设备运行验收。完整库保留既有SIGPIPE处理器提示，
退出码0，未将其归因于新stdio线程信号屏蔽，也未宣称已修复全局信号行为。


## Objective-C / ARC MCP owning 接口

LAIMcpClient/LAIMcpTransport与Agent绑定复用C ABI；桥接retain的传输owner由原生release
回收，外层对象销毁不使Agent绑定悬空。回调提供Foundation context及仅当前线程/
回调有效的Invocation；Objective-C与C++异常不会穿越C ABI。Schema字典排序编码，
原始字符串保留exact文本；不转换远程现代方言。详情见
[Objective-C README](../../libai-objc/README.md#mcp-工具接入)。
工作区8项回归通过，含真实stdio、ARC最后owner释放、授权/参数计数、UTF-8、缓存、
取消、异常后Invocation过期及文件日志未知效果新连接不重发。HTTP、完整Schema方言、
服务器通知等仍待补齐，六语言拥有接口不等于全部MCP功能完成。


Objective-C阶段逐文件同步后，主目录8项测试、iOS arm64模拟器交叉编译与仅使用
安装包的LibAI模块消费回归通过。消费者启用Objective-C++模块选项，安装头文件
只有Foundation绑定自身类型，无原始libai头文件。六语言已有MCP工具拥有接口，
HTTP/完整方言/高级协议/设备验收及其它总体目标仍需继续，不标记平台完成。
