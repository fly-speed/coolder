# 通用 DAG 工作流

`<libai/workflow.h>` 提供 `workflow_t`：明确JSON对象状态、节点、条件边、并行与合流。
SDK及独立C++、C、Python、Rust、Java、Objective-C均已提供运行接口。
它不依赖coolder项目/会话存储，不把旧agent_workflow_store当作通用图执行器。

## 执行规则

- 注册最多64个节点、256条边，运行前检查引用和环；不执行循环图。
- 节点读取该轮相同的只读state_json，返回patch_json（顶层对象）、route、status/error。
- writes显式声明可更新的顶层键；增量只替换整个键，不递归合并或删除键。
  `null`是正常JSON值。未声明的写入、重复键、非法UTF-8/JSON或超限增量拒绝。
- 空边标签为无条件边；有标签的源节点必须返回一个已声明route。
  所有匹配标签及无条件出边都激活。未选路径被标为skipped并向下传播。
- 合流等所有父节点终结后运行；至少一条入边激活才执行，等所有激活父节点完成，
  忽略已跳过父节点。多条激活入边不会重复运行合流节点。
- 默认max_parallel=1，在调用线程执行。显式大于1时，同一轮多个节点须声明
  parallel_safe=true，并在工作线程执行；宿主负责捕获状态和回调的线程安全。
  同轮writes冲突在任何回调之前拒绝；不采用“最后写入获胜”或隐藏的自动reducer。
- 所有该轮结果验证成功才提交合并状态。失败轮保留之前状态，成功回调标produced
  （未提交），失败回调标failed/recovery_required；已合并节点才标completed。
- 不自动重试、重连或重跑失败节点。不提供副作用回滚，外部效果需宿主核对。

节点状态pending/running/completed/skipped/produced/failed/stopped/recovery_required
与图运行status分开。子Agent的recovery_required优先传播，不被改成普通完成。
TaskContract只记录循环结束/结果，生成、构建、测试、审核默认unknown；完成图不
证明业务验收通过，节点JSON也不能修改原生验收契约。

状态/增量默认最大2MiB，可降低max_state_bytes；JSON继续受深度64、65,536节点限制。
max_parallel范围1..64，max_node_calls范围0..64，按被调度的尝试计数。
同一图不允许并行/重入run，也不能在运行期间注册/修改。期限0表示无期限，上限24小时。
取消/期限是协作式；等待已启动的全部回调后返回，不能强制中断任意宿主代码。

## 简例

```cpp
#include <libai/workflow.h>
libai::workflow_t graph;
libai::workflow_node_t choose;
choose.name="choose";
choose.execute=[](const std::string &,const libai::workflow_context_t &){
    libai::workflow_node_result_t result;result.route="yes";return result;
};
graph.add_node(choose);
libai::workflow_node_t answer;
answer.name="answer";answer.writes={"answer"};
answer.execute=[](const std::string &,const libai::workflow_context_t &){
    libai::workflow_node_result_t result;result.patch_json=R"({"answer":42})";return result;
};
graph.add_node(answer).add_edge("choose","answer","yes");
auto result=graph.run("{}");
// result.state_json包含answer；result.task_contract.accepted()仍为false。
```

安装后链接libai::agent；该目标已传递Threads依赖，消费方不需要手动追加私有线程配置。

## Agent组合与恢复边界

options.scope是宿主稳定任务ID（最多64个字母/数字/下划线/短横线），空值表示不提供
节点执行命名空间。context.execution_scope按长度前缀和节点名构造，最长102字符，
符合Agent执行存储scope限制。它是节点Agent的scope，不是该节点所有外部操作共用
的业务幂等键；context.control.idempotency_key保持空。Agent为每次工具操作另生成键。

节点构造/使用自己的Agent，传入state_json作为输入，设置execution_store和上述scope，
并转发control.cancellation及剩余期限。返回Agent状态及经过校验的最终JSON作为增量。
默认同步/多线程安全声明由宿主决定；不得跨线程共享非线程安全的语言对象或Agent。

## 图级持久执行

设置options.execution_store与非空稳定scope启用图日志。存储open/commit/close沿用
独占租约与持久CAS契约，图租约保持到整次run返回；应使用与节点Agent不同的存储
owner/租约实例，可以共享同一物理目录。内置文件owner一次只持有一个scope，不能
在其图租约期间再次用同一owner启动节点Agent。

图存储scope为wfg<scope长度>_<scope>，节点Agent仍使用wf<长度>_<scope>_<node>。
日志冻结初始JSON和图定义（节点名、排序writes、execution_revision、parallel_safe、
side_effect_free及排序边）。改变回调语义时宿主必须更新execution_revision，库无法
识别任意函数实现变化。修改图或初始状态返回recovery_required，不生成新任务绕过。

每轮先保存ready批次与只读输入，回调前保存running意图/attempts。返回成功后逐项
保存produced增量及route，最后将全部已知结果与新状态、completed和分支一起提交。
原子合流前退出时恢复复用produced结果；剩余ready仍使用原始快照。部分并行成功
不会因另一项未知而丢失，也不会重跑已保存结果。启动失败也不能证明意图没执行。

默认side_effect_free=false，未确认running、抛异常、错误状态或非法结果均保留未知
意图，需要受信任宿主核对。side_effect_free=true明确声明没有外部业务副作用，允许
下次run安全重放未确认读取或重试错误，仍计入总attempts；不在同次run自动重试。
已完成整图复用终态，不启动任何节点。状态/预算失败可能缓存终态，但不回滚效果。

confirm_node(store, scope, initial, expected_revision, node, outcome, output, reference)
用于核对匹配的running意图。outcome=applied提供已确认完整节点patch/route，恢复
不再调用该节点；none确认没有效果，允许按原快照重新执行。必须匹配当前日志CAS
revision、图、初始输入及节点，且reference非空；输出仍检查声明与路由。不能把此
接口直接作为模型工具。仅一个文件存在不能证明整个节点已完成；若节点Agent自身
还有未知工具日志，宿主也必须核对该层收据，不能通过图层确认掩盖内部未知效果。

max_node_calls、max_state_bytes与非零timeout在恢复时只会收紧并持久保存；提高配置
不扩大原任务。预算累积attempts，校验总数等于各节点attempts之和。期限使用原始
Unix毫秒开始时间，包含进程停止时间；run内转换为单调期限，时钟倒退失败关闭。
它不是任意时钟篡改防护，也不能强制中断阻塞回调。完成缓存不再次消耗节点期限。

progress_available=false表示存储/日志失败或定义漂移后无法证明进度，此时state_json
为空、journal_revision为空，不把空状态/零计数伪装为真实进度。正常恢复可从结果的
journal_revision取得宿主确认所需CAS版本。Lease/CAS/fsync/codec真实性由存储负责。

图日志解析上限16MiB，数据状态/增量仍最多2MiB且有结构限制。具体适配器可有更低
容量（内置文件owner当前state上限2MiB）；快照包含初始、状态、冻结输入及未合并
结果，不能据此承诺所有2MiB输入均能存入该owner。存储超限失败关闭，不截断日志。
六语言工作流接口及[可插拔RAG](RAG.md)已提供；平台加密、更多存储故障边界与
生产后端适配仍需继续。以下测试与语言接入记录按实施顺序保留，早期“尚待实现”
描述当时状态，不代表当前接口缺失；各语言当前入口见后续对应章节。

测试覆盖真实线程屏障并行、分支跳过/合流一次、环/缺失引用/冲突前置拒绝、原子轮
提交、无效route/增量、预算、取消/期限、重入，以及真实SDK Agent日志组合。


本阶段主目录SDK13项回归通过；安装包独立消费项目仅链接libai::agent，使用导出的
Threads依赖，运行相同并行/分支/状态/Agent组合测试通过。合流后重新检查完整JSON
资源限额，避免两份合法增量合并超出节点数上限。Visual Studio工程XML登记校验
通过，未运行Windows或设备端测试；工作流六语言及图持久化尚未完成。


持久测试覆盖图/输入漂移、分支缓存、部分并行结果、未知效果与陈旧/非法收据、
只读重试与预算收紧、期限不重置、计数损坏、提交失败，以及真实POSIX子进程退出。
进程退出不是物理断电、网络文件系统或所有平台耐久性证明。

主目录SDK14项及仅使用安装包的独立消费测试通过，包含真实POSIX退出后不重写与
核对收据后完成；Windows/设备/网络存储/断电边界未验证，六语言图接口仍待补齐。


## 独立 C++ facade

`<libai_cpp/workflow.h>`提供独立Workflow/Node/Context/Options/Result类型。转换和SDK
依赖仅位于libai-c++/src，静态/动态轻量包都包含图执行、持久恢复及宿主确认能力。
复用已有ExecutionStore、Cancellation、RunStatus和TaskContract，不修改旧Agent结果ABI。
示例见 [C++ README](../../libai-c++/README.md#独立工作流接口)。
工作区静态10项回归通过，包含实际并发/分支、写冲突拒绝、取消/重入、日志收据、
独立Agent组合、完成缓存及POSIX真实退出后的不重写和确认后完成。

独立C++封装已同步主目录，静态和动态构建各10项回归通过。两个独立消费项目
仅通过find_package(libai_cpp)与安装头文件编译并运行相同测试，覆盖真实并行、
分支/合流、Agent组合、进程退出与收据恢复。主目录coolder重建通过。
iOS arm64模拟器交叉编译通过；未执行设备运行或Windows编译。其它语言图接口
及RAG仍待实现，本阶段测试不构成五项目标全部完成的证明。

## C ABI

`libai-c/include/libai.h`提供拥有libai_workflow句柄、节点回调、结果builder、严格JSON
配置与whole-node收据确认。回调与存储复用SDK执行规则，公开头文件无C++类型。
返回共享TaskContract，不将图完成升级成构建/测试/审核通过。回调所有权仅成功注册
转移，重入/修改/活跃销毁拒绝；默认调用线程，显式并行需要宿主保证FFI线程安全。
子Agent可复制父取消令牌和读取剩余期限，context.execution_scope提供日志范围。
接口、JSON字段与生命周期见 [C README](../../libai-c/README.md#通用工作流-c-abi)。
Java/Python/Rust/Objective-C的语言级对象仍待实现，可插拔RAG继续待实现。

C ABI阶段主目录静态/动态各5项测试通过；两个仅使用安装包的独立C消费项目通过
同一组真实并行、分支、取消、收据、Agent组合和进程退出恢复测试。实际完整JNI
(x86_64)与Rust原生库(arm64)运行同一C工作流测试通过，Java/Rust APP重建成功。
Objective-C既有8项与Python既有21项回归通过。iOS arm64模拟器及Android arm64
API21 JNI交叉编译成功；未验证设备运行、Windows或物理断电。

## Python

`libai.Workflow`提供拥有图对象、同步节点回调、独立解析的dict状态及WorkflowNodeResult，
复用C ABI的分支/并行/合流、图日志和宿主收据规则。WorkflowContext提供原线程/回调
有效的停止与剩余期限，agent_options复制共享取消令牌并自动释放，用于子Agent组合。
模块级FFI trampoline持有节点注册状态至图关闭，失败注册不留下引用；需显式关闭。
用法及GIL/生命周期边界见 [Python README](../../libai-python/README.md#工作流)。
Java/Rust/Objective-C图对象及通用RAG仍待补齐。

Python阶段主目录完整原生库与离线wheel/配套轻量原生库各28项回归通过，包含真实
并发、分支/合流、停止/借用过期、持久收据、存储失败及进程退出后不重写。旧原生
库的Agent兼容与工作流缺少ABI诊断通过；未宣称设备或Windows运行已验证。

## Rust

Workflow拥有共享C ABI图，普通节点使用非Send闭包，add_parallel_node要求Send并以
Mutex保护可变回调。parallel_safe由注册方法派生；普通节点线程检查在读取本地闭包
之前完成，图自身非Send/Sync。存储回调保留调用线程与既有Rc/RefCell owner契约。
Context提供子Agentscope、RAII复制取消与剩余期限；失败注册回收本次Box，close/Drop
释放成功注册的闭包。完整JSON字段、持久收据、借用及panic边界见
[Rust README](../../libai-rust/README.md#通用工作流)。Java/Objective-C图对象及通用RAG继续待实现。

Rust阶段主目录轻量/完整库各28项回归与非Send回调编译拒绝用例通过，独立Cargo
消费项目8项工作流测试通过，coolder-rust重建成功。覆盖新进程退出后不重复效果、
收据恢复、真实并行/分支/合流与Rust回调panic载荷析构边界；未验证设备或Windows。

## Java

`io.coolder.libai.Workflow`拥有C ABI图，提供NodeSpec/NodeResult/Context/Options与
confirmNode宿主收据接口。缓存类/方法全局引用，并行工作线程使用自己的JNIEnv，
仅主动附加线程在返回时分离；注册失败释放Node引用，成功保留至close。Context
在回调返回时过期，子Agent使用ChildOptions继承父取消/剩余期限。Store继续使用
调用线程及既有Execution.Store契约。用法见 [Java README](../../libai-java/README.md#通用工作流)。
Objective-C图接口、通用RAG及其它未完成能力继续保留。

Java阶段主目录APP重建成功，仅使用安装JAR的独立消费测试在轻量/完整JNI上各8组
通过（含工作流实际并行、子Agent与新JVM退出后恢复）。Android arm64 API21 JNI
交叉编译通过；未验证设备或Windows。完整JNI既有SIGPIPE处理器提示仍存在，测试
退出码为0，保留日志；未把该提示隐藏或宣称已修复。

## Objective-C

LAIWorkflow提供Foundation图对象、NodeSpec/NodeResult/Context/Options与收据确认，
公开头文件无原生C/C++类型。图持有复制block至释放，失败注册释放本次owner；
原生工作线程使用autoreleasepool，NSException与C++异常保留失败语义，上下文finally
过期。Context支持ARC复制取消与子Agent剩余期限。字典输入排序编码，原始JSON
入口保留跨语言冻结契约；用法见 [Objective-C README](../../libai-objc/README.md#通用工作流)。
六语言工作流入口已接入共享执行器，可插拔RAG及其它未完成项继续进行。

Objective-C阶段主目录轻量/完整库各9项回归通过；独立消费项目仅使用安装后的
LibAI模块、Foundation头文件和静态库，运行同一组并行、Agent、异常、取消及真实
进程退出/收据恢复测试通过。iOS arm64模拟器交叉编译通过，未验证设备或Windows。
