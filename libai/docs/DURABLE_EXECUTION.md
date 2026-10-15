# 通用 SDK 持久化执行

此接口已接入 `<libai/agent.h>` 的真实运行循环。通用 SDK、独立 C++ facade、C ABI、Python、Rust、Java 和 Objective-C 已提供存储接入；
Rust/Java APP 已在支持 POSIX 文件适配器的平台接入新日志；C++ APP 保留原编程检查点，
已增加 POSIX 会话租约、工具意图/完成记录及下述宿主确认接口，但尚未统一到此 SDK 日志，
也尚未接入 APP 的收据核对管理界面。
APP 接入范围与剩余边界见 [平台实施记录](AGENT_PLATFORM_PROGRESS.md#rust-app-接入-sdk-执行日志)。
通用SDK六语言持久执行及C++兼容检查点的恢复规则已通过当前源码运行验收，
见[五项目标最终验收](GOAL_ACCEPTANCE_AUDIT.md)。这不宣称磁盘格式统一、
管理UI完成或所有任意外部效果自动确认。

## 原 C++ 编程检查点诊断

### C++验证命令的父关联

可信验证器在保存逐命令running意图后、实际进程派发前，把命令键追加到父工具
的command_parent关联。父关联以用户存储root、项目和父键绑定，最多64个唯一
命令键；CTest inventory独立关联。关联失败不派发进程，保留命令意图供核对。
无父键的旧调用不补造关联；该记录不保存原始命令输出。

sandbox.parent.inspect接收idempotency_key与project_path，返回found、revision、
children，便于宿主逐项查询sandbox.inspect/confirm。progress.confirm核对已关联
命令，持有父关联及全部命令租约直到父检查点提交。缺失、损坏、项目不匹配或
running阻止父完成；父none要求全部关联命令已confirmed_none。批次子确认及父
确认也核对各自关联命令，不能用batches.confirm绕过未知编译/测试进程。

C++直接验证器自动关联；通用sandbox.execute可显式传入parent_idempotency_key，
使用同一root与项目在派发前建立关联。其它宿主回调仍未自动建立父关联。
旧缺失关联保持兼容，不宣称已核对旧调用的全部命令。元数据不同于业务效果签名，
宿主确认仍须核实外部结果；原始记录不能清除或换键来绕过未知效果。

主目录十二个任务文件同步核对、三个APP构建、24项libai测试和实际C ABI/Python、
JNI父命令确认测试通过。工作区回调在派发入口读取已提交父关联；真实APP六组
恢复场景逐项核对报告命令键属于父关联。未知/缺失命令、损坏关联、父关联或子
命令租约争用及错误none均拒绝；已核对none和已返回命令有正常完成路径。
合成确认记录不是真实外部效果证明；真实进程验证由APP编译测试覆盖。

### 批次父子确认

batches.confirm接收且只接收idempotency_key、revision、child_index、outcome、
reference、result。受信宿主核对running或returned子调用，在批次租约与当前CAS
版本下保存confirmed_none/applied和非空审计引用。none的result须为空；applied须
含output_sha256及callback_ok。确认不执行子调用、不重放旧键、不确认外部命令，
也不自动确认父批次。pending子调用未派发，不允许伪造成已执行的确认。

progress.confirm核对workspace.execute_batch父意图时，锁住对应批次日志直到父
检查点提交完成，验证项目、子调用数和全部子记录。running阻止父确认。父none
只允许未派发子调用、已核对none的写操作及已返回的只读观察；不能把returned
的写操作直接当作没有外部效果。回调返回与宿主核对仍是不同事实。
已确认的部分批次可由宿主用错误结果结束父操作；不自动拼接局部结果或执行
pending子调用。旧缺失日志保持兼容，不补造子证据。外部命令收据仍须独立核对。

主目录八个任务文件同步核对、三个APP重建、24项libai测试、实际C ABI/Python、
JNI父子确认及六组APP恢复回归通过。覆盖子确认CAS、越界/布尔序号、非法结果、
租约与重复确认；父running、损坏、项目/数量不匹配及错误none保持检查点不变；
none与已核对部分批次完成均有正常路径。用例模拟丢失响应，不声明真实外部命令
已核对，也不是物理断电证明。

### 浏览器父子确认检查

progress.confirm确认browser.*父意图时，检查主键及固定辅助后缀undo-previous、
baseline/applied/restored的inspect/snapshot。已有记录须通过严格格式验证，项目、
运行身份与动作类型须匹配；running必须先通过browser.receipt.confirm核对。父none不能
覆盖returned或confirmed_applied的写入动作；观察型记录可作为已返回的读取事实，
不代表业务成功。子确认并不自动确认父意图。

检查持有每个子键的租约直到父检查点CAS提交完成，防止检查后子状态被改动。
记录缺失保留旧无子日志调用兼容，不补造证据；已存在的目录也锁住尚无记录的
固定键，防止提交期间生成它们。命令与批次的父子核对仍须单独实现。

主目录六个任务文件同步核对、三个APP重建、24项libai测试及实际C ABI/Python、
JNI父子确认测试通过。未确认、损坏、项目/运行/动作错配、子租约争用和错误none
均保持父检查点字节不变；核对子动作后applied成功，子写入确认none后父none成功。
用例为合成丢失响应记录，不是实际浏览器业务效果或物理断电验证。

### 工作区撤销日志的进程间租约

POSIX下workspace_change_set_store的create/apply/create_and_apply/recover均在现有
进程内锁之后取得用户根目录change_sets的workspace_transaction租约。预览创建
和恢复也可能修改撤销日志，因此与实际应用使用同一把锁。租约跨越原始快照
检查、一次性预览消费、撤销日志、文件变更、回滚和日志清理；争用时失败关闭，
不能恢复另一进程仍在执行的事务。原有日志格式、预览ID和内容比较规则不变。

审核重试仍先核对正式文件是否已匹配修订，避免已应用文件再次写入。该租约与
结果审核租约分别保护源码事务和结果元数据，没有把二者变成跨文件原子提交；
应用完成但元数据保存失败时，宿主必须重读并核对，不能假定源码未变。
Windows与未使用新租约的旧程序不具有本阶段的跨进程保证。

主目录六个任务文件同步核对、三个APP构建、24项libai测试、实际审核恢复及
六组进度恢复回归通过。真实子进程竞争同一预览仅一次应用成功；活跃租约阻止
预览、应用和恢复，原文件未修改且原预览未消费。实际APP测试恢复pending审核
快照以模拟源码提交后的确认丢失，重试仅补齐审核身份，文件inode/修改时间/内容
不变。这是模拟故障与真实进程测试，不是物理断电或跨文件原子提交证明。

### 结果与审核的进程间事务

POSIX下同一项目的结果更新共享results目录中的result_updates租约。worker的save
在租约内读取并合并同一不可变generation/hash的已审核决定，再安装文件。
set_decision/set_change_reviews在同一租约及进程内互斥锁内完成当前记录读取、
审核条件校验、修改和保存，不再先load再独立save。不同进程的决定不能用旧快照
覆盖；冲突决定和租约争用失败关闭，由宿主重读/重试。已有结果不可读或损坏时
save拒绝覆盖。只读查询仍可读取已提交的原子文件，不代表取得写入授权。

此锁保护结果元数据，不涵盖正式源码应用事务、用户鉴权或业务幂等性。客户端
仍须提供路径、generation和draft_hash；未知旧程序或外部直接写文件不受保护。
Windows继续使用原进程内控制，本阶段不声称其跨进程事务保证。

主目录五个任务文件同步核对、三个APP构建、24项libai测试、实际审核/版本/
重启及六组恢复回归通过。两个真实进程并发审核不同文件，争用时仅重试读取与
审核事务，最终保留双方决定；旧worker快照不会恢复pending。还覆盖活动租约
无写入、相反决定拒绝及损坏结果不被覆盖。本阶段未验证物理断电或正式源码
应用事务与结果元数据之间的跨文件原子性。

### 后台worker持有租约直到收尾结束

非预览、非assistant_chat的C++编程循环在启动前取得会话级coding_loop租约。
后台worker通过作用域守卫接收该租约，保持到最终结果保存、审查快照、会话更新、
进度清理和加密启动检查点清理结束。正常返回、失败、取消及异常都会释放；
重复worker不改变当前任务或删除其检查点。直接调用循环的宿主仍只在循环期间
持有租约，需要自行保护其额外收尾操作。

这修正了循环返回后提前释放租约、worker随后仍删除进度的窗口。同一会话的
另一个编程进程或宿主确认不能在该收尾窗口取得同一租约。它不替代人工审核
记录自己的并发控制，也不声称保护未使用此租约的旧程序或其它存储写入。

主目录六个任务文件同步核对、三个APP构建、24项libai测试、实际C++取消/失败/
恢复/批次/缓存六场景及正常结果/审核/版本/重启回归通过。作用域测试在真实循环
返回后使用独立子进程确认锁仍被占用，守卫退出后重新取得锁，并拒绝重复worker。
未证明其它写入路径的跨进程事务或物理断电行为；人工审核并发仍需独立核对。

### 浏览器逐动作持久记录

所有实际派发且带宿主执行键的browser_debug_tool调用，包括辅助观察、撤销和终态清理，
在受信用户根目录私有browser_commands文件存储取得键租约。命令对桥接客户端
可见之前持久提交running；接受响应后提交returned，保存命令名、项目、运行与
非秘密连接证据身份、参数/输出/截图摘要及callback_ok。不保存配对令牌、原始
参数、响应或截图像素。callback_ok只是客户端返回声明，不代表业务或任务验收。

browser.receipt.inspect可通过各语言CodingService读取严格校验的元数据与CAS
版本；活动租约、损坏记录及非法身份拒绝读取，running要求宿主核对。returned
仍须核对父意图，不自动确认外部效果。宿主确认接口见下文；没有自动重放接口。
同一活跃连接的结果缓存仅在持久记录摘要匹配时复用，不生成新截图；重新配对
或进程重启后，任何已有记录都阻止同键再次派发。结果提交失败撤销连接并将
结果视为未知。无键旧宿主调用保留原行为，不声称持久保证。文件后端限POSIX，
C++语义v12，旧冻结定义不静默迁移。动作权限及当前连接仍由宿主与原桥接验证。
状态查询不派发客户端命令，不创建动作记录。

主目录验证：三个APP构建、24项libai测试、C++恢复回归及Python/C ABI、JNI
入口检查通过。真实桥接代码覆盖派发前running、返回后摘要、活动租约拒绝、
缓存与持久摘要不一致、损坏记录、断线、重新配对和全新exec进程旧键拒绝。
测试没有打开真实浏览器页面，未验证实际点击/截图效果或物理断电；不将客户端
fixture返回声明当作已完成业务操作。

### 浏览器动作宿主确认

browser.receipt.confirm接收且只接收idempotency_key、revision、outcome、reference、
result。宿主须先核对实际动作及外部证据；库只记录宿主声明，不验证页面业务效果。
接口取得执行时同一键租约，要求版本仍匹配且状态为running。none要求result为空
对象，保存confirmed_none；applied要求result恰好包含output_sha256、image_sha256、
callback_ok，保存confirmed_applied。输出摘要为64位小写十六进制，截图载荷摘要
允许为空，callback_ok必须为布尔。reference为非空、无NUL且最多2048字节的审计引用。

确认保留项目、运行、命令、连接证据身份及请求摘要。活动执行、损坏记录、陈旧
版本、重复确认、非法结果及额外字段均拒绝。确认不派发动作、不保存原始结果、
不恢复历史截图、不开放旧键重放，也不更新父工具或任务验收。none确认后若需
执行新动作，必须由宿主重新授权并使用新键；不能以新键绕过尚未核对的旧效果。
各语言经CodingService调用；不能将此管理接口注册为模型工具。后端限POSIX。

主目录验证：三个APP构建、24项libai测试和实际C ABI/Python、JNI确认测试通过。
两种确认结果保留身份；陈旧/重复确认、非法输入和活动租约均拒绝；none与applied
确认后全新exec进程仍不派发旧键。绑定测试使用合成的丢失响应记录，C++测试
调用真实桥接代码；没有验证真实页面业务效果或物理断电。

### C++批次逐子调用记录

workspace.execute_batch有宿主执行键时，在私有tool_batches文件存储中取得批次
独占租约，先冻结顺序、项目及参数摘要，再为每个子调用保存pending/running/returned。
子键按父键与原始序号派生；并行只读操作的日志提交串行执行，回调仍可并发。
returned仅说明回调返回，callback_ok和输出摘要描述该次返回，不等于外部效果、
候选草稿发布或任务验收已确认。异常或结果提交失败可能保留running；父意图仍
由原检查点独立处理。记录不保存原始参数或工具输出。

batches.inspect通过六语言已有CodingService读取该批次的完整元数据与CAS版本。
执行中的租约、损坏数据、非法字段或身份不匹配拒绝读取；running提示宿主核对。
其它状态也要求检查父意图，不能仅凭returned自动确认父批次或外部命令。
任何已有批次记录均阻止同键再次派发，包括已returned的子调用；此阶段没有
自动拼接局部结果、重放批次或清除记录；子调用确认API见上文。外部命令继续使用
独立命令收据，私有候选回滚不能消除已发生的外部效果。无键旧调用保持兼容，
不声称持久保证。当前文件后端限于POSIX。语义v11，旧冻结定义不静默迁移。

主目录验证：24项libai测试、三个APP构建、真实C++混合批次失败及重启回归
通过；并行读取、同键拒绝、租约争用、损坏/未确认记录和根项目路径均覆盖。
C ABI/Python与JNI实际调用新的诊断入口，缺失记录无状态目录创建，非法参数拒绝。
测试不等于设备端、Windows或物理断电验证。

### 验证报告复用时核对命令记录

C++自动验证和显式workspace.validate的报告缓存，在复用前逐项读取独立命令记录。
记录须已finished或经受信宿主confirmed_applied，且项目、命令身份、退出码、
取消/超时及启动事实与报告匹配；CTest清单记录也须已确认。缺失、损坏、
执行中或元数据不一致时停止，不能发送新模型请求或重新执行命令来掩盖未知效果。
真实APP恢复测试覆盖执行中、缺失、损坏和退出码不一致，均保留原检查点且
不发送模型请求；恢复原始私有记录后缓存复用成功。
无执行键的旧宿主报告保留兼容，不补造持久证据。此检查不代表外部业务验收，
也不代替父工具和子命令分别核对。语义版本v10，旧冻结定义不静默迁移。

### C++验证器独立命令记录

编程验证器的直接program_sandbox调用已接入同一私有命令记录执行器；命令与
CTest清单查询在派发前取得独立键租约并持久提交running，返回后提交finished
元数据。记录位于受信用户存储root，不能放入仅用于验证的随机草稿身份中。
可信内部调用不消费模型可提交的sandbox.preview授权，也不暴露任意命令接口。

同键记录、租约争用或提交失败阻止派发；回调异常保留未知状态。验证器将此类
问题作为需要核对的工具失败，不能封装成正常编译报告让外层意图确认完成。
命令stdout/stderr只参与当前报告，不存入命令收据；inspect/confirm入口沿用上述
协议，父工具意图与子命令确认仍须分别核对。无键的旧宿主验证调用保持原行为，
不声称已具有独立命令记录。C++语义版本v9，旧冻结定义不静默迁移。

### 逐命令宿主确认

完整服务提供sandbox.confirm，接收且只接收idempotency_key、revision、outcome、
reference、result。受信宿主先读取sandbox.inspect并核实命令效果；版本必须仍
匹配且状态为running。none保存confirmed_none且result只能为{}；applied保存
confirmed_applied及已核实的九字段结果元数据。非空reference是宿主审计引用，
不作为库已验证的外部签名。命令/计划身份及原始摘要保持不变。

确认持有与实际执行相同的进程间租约并执行版本提交；活动执行、陈旧/重复确认、
非法格式或损坏记录均拒绝。它不派发命令、不保存原始输出、不使旧键可以重放，
也不自动完成外层工具收据或任务验收。各语言通过既有CodingService入口调用；
不得把这个管理操作注册为模型工具。该接口不迁移旧历史为已确认事实。

### 完整沙箱服务的逐命令元数据

sandbox.execute在POSIX下为每个执行键取得SDK文件租约，消费计划后先提交running
记录再进入审计/进程派发，进程返回后提交finished结果元数据。版本检查与原子
替换/fsync由既有私有文件存储执行。任何已有记录都阻止同键再次执行，拒绝发生
在消费新计划之前；这不意味着库自动把一个新计划认作旧执行的合法重试。

记录只保存键、计划/命令身份与摘要、进程启动/退出/取消/超时等事实和输出摘要；
stdout/stderr仍仅用于当前调用，不写入该收据。sandbox.inspect通过已有各语言
CodingService入口只读诊断，running要求宿主核对，finished不能重放，也不能
推出业务效果或任务验收通过。旧执行没有命令收据时不补造历史证明。
记录损坏、租约争用或提交失败关闭；未知状态的宿主确认见上述接口。
C++语义版本v8，Java/Rust宿主工具5-command-receipts，旧冻结定义不静默迁移。
C++编程验证的直接program_sandbox调用接入范围见上述v9说明。
当工作区等于存储root时，私有记录仍须对命令隐藏。macOS已有目录读写拒绝规则；
Linux helper增加私有命名空间的只读空遮蔽及现代挂载管理系统调用拒绝，失败关闭。
当前测试环境为macOS，未验证Linux命名空间/Landlock运行行为。

### 浏览器动作执行键

C++模型浏览器工具、自动实验撤销/观察/截图和终态清理传入独立宿主键；桥接命令
JSON新增idempotency_key。辅助步骤使用父意图键的固定后缀，不从模型参数读取。
旧客户端可忽略键；新客户端可在结果消息中回显，回显错配拒绝。结果JSON严格
检查重复键/UTF-8/资源限制，保留browser_execution_reused为宿主元数据。

同一活跃连接内最多256个键，同键参数变化拒绝；未确认键不再次派发。确认结果
复用返回原键和browser_execution_reused=true，不派发新命令。JSON结果缓存最多
512KiB，超限仍保留已确认标记，结果不可用时要求宿主检查，不能借缓存限额重放。
缓存不保存截图像素；重放JSON不代表新截图或当前页面状态，需新键进行新观察。
断线/超时继续按未知结果处理，不自动重试。

上述结果缓存仅在进程和当前连接内有效；逐动作持久元数据见本文v12说明，
仍不替代客户端幂等存储或业务收据。
新配对不自动证明旧动作未产生效果；运行时未知意图仍要求宿主核对。语义版本v7，
旧冻结摘要不静默迁移。子动作父工具确认仍独立；逐动作持久记录及宿主确认API已接入。

### 显式验证和批次验证键

C++执行器将模型工具参数与宿主意图键分开传递，模型不能填写或覆盖此键。
workspace.validate传入已保存的意图键；批次按父意图键与原始序号派生batch键，
不使用随机私有草稿ID作为执行身份。并行读取及顺序执行的子调用均保留固定序号。
验证进程和报告沿用上述命令键规则。语义版本v6，旧冻结摘要不静默迁移。

缓存命中不执行命令，也不把原键改成当前调用键；tool_execution_key和命令键
保留原执行所有者，reused_for_tool_execution_key仅说明本次复用目标，并明确
validation_reused=true、executed_this_call=false。不可用报告复用也注明未执行。
批次级收据仍不等于独立的子调用/逐命令收据；未知批次仍须宿主核对。

### 自动验证命令键

C++独立自动验证与工具内修复验证显式传入已保存的工具意图键；命令键由工具键、
源码/草稿指纹、固定命令定义和顺序派生，传入LIBAI_IDEMPOTENCY_KEY。报告记录
tool_execution_key和每个命令的idempotency_key；CTest清单查询使用独立inventory
后缀，不与测试执行共用键。相同收据复用保留原键，不新增命令执行。
Java/Rust编译门槛由SDK调用键、成果版本及固定命令定义派生键，经sandbox.execute
传递并检查返回键；命令结果随完整报告保存。代码或调用变化会产生不同键。
C++语义版本v5、Java/Rust宿主工具版本4-validation-keys；旧冻结定义不静默迁移。

键是关联标识，不是独立的逐命令收据或外部业务去重保证。C++模型直接请求的
workspace.validate及批次传递见上述v6接入；浏览器键见上述v7接入。
历史空键不补造执行身份。旧公开验证调用可省略键，报告空键表示未绑定工具意图。

### 完整服务沙箱幂等键

CodingService的sandbox.execute可接收宿主工具上下文的idempotency_key；非法JSON、
字段、类型或键在消费计划前拒绝。缺省键绑定一次性计划ID。POSIX进程只通过
固定LIBAI_IDEMPOTENCY_KEY环境项接收它，审计开始/结束与历史保存同一键；旧
11/12字段记录不补造键。计划消费仍阻止同一计划再次执行，键本身不授权或自动
对不同计划去重，外部业务必须落实持久结果和幂等语义。参数及使用示例见
[CODING_SERVICE.md](CODING_SERVICE.md)。编程验证与浏览器键已分别接入；
不将键传递视为所有命令都已具有独立收据，客户端幂等存储仍由宿主提供。

### 终态浏览器临时样式清理

运行结束时的自动undo-all使用browser.patch_style工具意图，执行前写入检查点，
收到ok确认后保存完成收据和结果上下文；内部调用不伪造Provider调用ID。
失败、异常或确认保存失败均保留未知效果，不自动再调用撤销；已有未知工具也会
阻止终态清理。意图保存失败或工具预算耗尽时不调用浏览器，提示宿主清理。
清理计入工具预算与执行计数，清理未确认时运行不能被标记为成功完成。
原始运行错误保留并追加清理诊断。执行语义标志升级v4，旧冻结摘要不静默迁移。

这一收据确认的是浏览器桥接响应，不证明正式源码已修复，也不证明浏览器样式
视觉验收通过。进程在清理意图保存前退出、桥接连接更换、逐动作幂等键转发与
浏览器端效果核对仍需宿主配合；不承诺跨浏览器进程的恰好一次清理。

### 并发审核与失败工具重试

用户在工具执行期间接受草稿时，运行时仍同步审核状态，但只允许已审查的本地
提案工具重试一次：propose、propose_batch、patch_set、replace、propose_delete、
propose_move、propose_mkdir。浏览器操作、workspace.validate、execute_batch及未知
扩展工具不能因审核变化自动再次执行；模型参数不能声明自身具有安全重放权限。
被阻止的审核重试记录tool_review_retry_blocked事件，保留原始工具失败结果。

失败的workspace.execute_batch保留running意图，要求宿主核对后才可恢复。
批次rolled_back仅表示私有草稿回退，不能证明此前的编译、测试或外部操作没有效果。
当前采用保守的批次级确认，包括失败的纯读取批次；尚未提供逐子调用收据。
成功批次继续正常确认。此规则不承诺外部业务恰好执行一次，也没有自动替宿主核实
命令效果。此规则在v3引入；当前语义标志已升级v4，旧冻结摘要不静默迁移。

完整编程后端的 `CodingService.call("progress.inspect", arguments)` 接收且只接收
`project_path`、`session_id` 两个字符串，例如：

```json
{"project_path":"snake","session_id":"宿主保存的会话ID"}
```

此操作可通过独立C++、C ABI及各语言已有CodingService入口调用，不属于轻量SDK
`executions.inspect`。宿主必须先鉴权，并选择该用户的可信root；库检查会话属于
所选project，取得与实际编程循环相同的POSIX会话租约，再读取检查点。活动任务、
损坏记录、身份不符或不安全存储均失败关闭。Windows暂不提供此诊断。

返回version=1、has_checkpoint、checkpoint_revision、completed_tool_calls和effects。
checkpoint_revision是同一次受限读取、完整校验后的原始文件字节SHA256；任何字节
变化（含空白）都会改变它。缺少检查点时为空，读取失败不返回版本；它不是副作用
收据或续跑权限；宿主确认必须再通过下述版本检查。每条effect包含
key/name/state、参数与输出摘要、reference及计数边界；不返回提示词、正文、原始
参数或输出。新记录另外在工具执行前冻结tool_call_count、native_call_ids和适用的
response_id；诊断只返回tool_call_count/native_call_count/has_response_continuation，
不公开原始Provider ID。原生调用ID必须完整、唯一且数量匹配；缺少ID时不补造。
原8字段意图保持可读，但没有这组关联信息；不得据此宣称能恢复原生调用结果。
这些字段本身仍不证明操作已执行或产生效果。
无检查点时recovery_policy=new_task，有未确认running时为
host_reconciliation，其余为resume_checkpoint。最后一种只是意图状态诊断，
不能证明Provider、任务配置或所有外部状态都满足续跑条件；正常恢复仍须完整校验。

查询不执行模型、工具、收据确认或检查点保存；租约可能创建保留锁文件。需要已有
安全的项目状态目录，不能用此接口建立任意目录。不得通过删除日志、修改state
或模型自报完成绕过未知效果，应由可信宿主核实后使用下述确认入口。
SDK日志与原编程检查点使用不同格式，不能交叉调用确认接口。

POSIX检查点读取固定安全目录描述符，以O_NOFOLLOW打开记录，并检查当前用户
拥有的0700目录、0600普通文件及单一硬链接。非普通文件以非阻塞方式打开后拒绝，
避免FIFO挂起恢复；仅ENOENT当作缺失，权限/读取错误失败关闭。Windows仍沿用
原文件读取路径，未因此具备相同保证。保存继续使用原子替换和文件/目录fsync。

POSIX存储层新增save_if_revision：要求现有检查点的原始字节摘要匹配后才保存，
成功返回新摘要，冲突/失败清空返回版本。普通save、此条件保存及remove共享
coding_progress_<session>记录租约，避免其它进程的普通写入绕过版本检查；没有
会话时使用single。租约覆盖检查与原子保存，争用立即拒绝，进程退出由内核释放。
读取可观察之前已提交的快照；宿主仍须持有coding_loop_<session>会话租约以
排除运行中的模型/工具。两个租约职责不同，不能只取得记录租约便确认活动任务。

save_if_revision是内部后端写入原语，不验证外部收据，不更新验收事实，也不是
公开收据确认入口。外部直接改文件的程序不遵守此租约，宿主必须管理
可信目录和写入者。Windows不提供条件保存。删除只增加租约保护，未新增删除
目录fsync保证；不能把删除检查点当作解除未知效果的核对操作。

### 原编程检查点宿主确认

完整后端提供 `CodingService.call("progress.confirm", arguments)`，复用各语言已有
JSON调用接口。它必须由已鉴权、已核实业务效果的宿主调用，不能注册为模型工具。
参数严格为以下8个字段；JSON重复键、错误类型和额外字段拒绝：

```json
{
  "project_path":"snake",
  "session_id":"宿主保存的会话ID",
  "checkpoint_revision":"progress.inspect返回的原始字节摘要",
  "key":"待核对的工具键",
  "outcome":"none",
  "reference":"宿主核实的外部收据或审计记录引用",
  "output_json":"",
  "staged_changes":[]
}
```

none确认未产生效果，禁止提交结果或草稿；保存明确的未执行结果并保留这次尝试
计数。模型之后可以提出新尝试，仍受权限和累计工具预算限制，并非自动重跑旧操作。
applied确认效果与结果已核实，output_json必须为受限JSON对象文本（最多256KiB），
staged_changes必须提供核实后的完整草稿集合，包括原有提案；没有提案时为[]。
收据引用非空且最多2048字节，库不验证它指向的业务记录、签名或真实性。

每份草稿必须提供原检查点格式的13个字段：operation/path/target_path/content/reason、
creates_file/creates_directory、original_content/original_content_available、generation、
base_hash/draft_hash/review_status。generation须为正整数且小于2^63-1，避免有符号
持久格式溢出。路径必须属于选定项目，草稿摘要及可用原文摘要
必须匹配。不得遗漏旧提案、改写同一generation的内容、降低版本或改写旧来源基线；
新增/更新的提案必须pending，不能通过此入口伪造人工接受。确认只保存草稿，不将
它们写入正式源文件，也不设置构建、测试或人工验收证据。

操作先取得coding_loop会话租约，匹配会话/项目、检查点版本与末尾running工具键，
再通过记录租约内save_if_revision提交。活动运行、陈旧或重复确认、损坏记录、缺少
调用计数的历史意图均拒绝。根据宿主结论保存confirmed_none/confirmed_applied、
收据引用、结果摘要和上下文，计数不回退。成功返回新checkpoint_revision和状态；
不会调用模型或工具，也不会自动启动续跑。

已冻结的Responses ID存在时，按原native_call_ids重建待发送结果。多调用成功结果
须采用原批次格式：results数组长度匹配，每项result是JSON对象文本；失败结果
可以按原批次错误格式应答每个调用。缺少关联时只保存文本上下文，不补造Provider ID。
结果保存成功后正常恢复可以继续，不重新调用已核实的这次工具。它不能阻止模型
今后提出语义相似的新调用；外部业务仍需落实幂等键。Windows及APP管理界面未接入。

最终回答的非空文件提案也使用这组记录，内部名称coding.final_proposals，每批
计入一个累计工具调用。意图先于草稿合并，确认包含完整草稿及上下文并先于自动
验证。它没有原生Provider调用ID；宿主确认时不得补造ID。只有总结的空changes
不新增意图，重复生成相同草稿也不能自动当成外部幂等性证明。正式文件仍须
经过独立用户接受流程；自动验证和其它副作用路径的日志保护尚未全部接通。

### 取消与保留

原C++编程任务的普通取消及迟到取消现在保留私有检查点、工具收据和工作副本。
取消表示停止执行，不证明已经开始的操作没有效果；取消不能清除running意图。
记录损坏或读取失败也不清理，保留证据供宿主核对。文本预览仍使用原只读清理
流程。保留的私有数据属于运行数据，不是build.sh临时编译目录。

状态API在取消后的诊断可用时提供recovery_note；checkpoint_read_unavailable表示
取消线程未能读取记录，不能从它推断文件不存在。被kill的fiber可能不能继续read，
此时recovery_available保持保守，不宣称已验证可恢复。正常线程中的progress.inspect
与实际启动仍检查完整记录；未知效果需要宿主核对。取消不会自动启动续跑，原
启动检查点清理仍关闭自动重启；用户可明确请求检查点恢复。暂未提供运行数据
的收据核对后清理管理界面，不建议删除文件来绕过未确认效果。

### 最终回答触发的自动验证

原C++最终回答触发的新验证批次使用内部coding.final_validation意图。执行前冻结
项目、草稿指纹和沙盒资源限制并保存running；完整验证报告返回且检查点保存成功
后才确认done，再将报告作为修复反馈或验收证据处理。编译/测试失败的完整报告
也是已知结果，不能将它等同于未确认效果；执行取消、环境准备错误或保存失败
则保留running，正常恢复不能自动绕过。宿主确认入口继续适用。

每次新批次消耗一个累计工具额度；额度不足不启动命令，返回未验证状态，不产生
构建或测试通过证据。同次调用已有匹配报告的缓存路径不新增执行。恢复预验证、
预算边界交付及超额调用交付也已共用相同意图/预算/取消/确认路径，使用
coding.recovery_validation、coding.budget_validation及coding.admission_validation。
独立调用失败会传递到整个运行，不继续请求模型。启动保存使用更新后的累计计数，
首轮图片仍在首个模型请求发送，不被恢复预验证的额度消耗跳过。

已知检查点恢复在缺少可复用收据或成果变化时启动新的预验证批次；完整报告复用
见下节。未知批次在启动加载时拒绝，不自动重跑。工具处理期间的修复验证仍是
已有外层工具意图的一部分，没有独立子记录；不能把独立入口统一当作所有内部
命令及副作用已统一。它保护整个验证批次，不声称每条
内部命令已有独立业务幂等键或外部数据库事务。

### 原编程检查点执行配置摘要

新检查点execution_revision保存工具目录/Schema/授权标志、编码的实现版本、运行
策略及探测到的工具链路径、Provider协议/URL/模型/内容权限、运行预算与推理配置、
沙盒资源限制对应的SHA256。保存摘要，不将这些配置的原始值追加到检查点。
progress.inspect返回execution_revision及execution_frozen，便于宿主判断记录状态。

恢复有摘要的检查点时，配置不符会在恢复工作副本/验证/模型调用前拒绝，保留原
检查点。每个新模型轮次、工具意图和最终回答也重新核对运行配置，避免运行中
更新策略后继续使用不同规则。调用者需明确核对原配置或建立独立新任务，不能
修改摘要来绕过。宿主收据确认保持原摘要，不授予更换执行配置的权限。

摘要不识别任意工具函数或相同路径二进制的内部变化，也不是远端服务代码签名。
编码实现标志和Agent定义版本必须随语义更新；运行中政策更新与已开始命令之间
尚未提供完整原子配置事务。旧记录缺少摘要时仍走兼容路径，不补造历史配置事实；
不能据此宣称旧记录已冻结。此机制本身不提供总时限迁移。

### 跨重启验证收据复用

新检查点validation_receipt仅保留一份最新完整独立验证报告（最多512KiB），包含
effect_key、execution_revision、source_sha256、proposal_sha256和report_json。
它与对应工具结果确认在同一次检查点保存中提交。读取检查收据所属工具为已确认
的独立验证、报告摘要与工具output_sha256一致，报告内源码/草稿摘要也必须一致；
running、缺失所有者、损坏或版本不符的缓存拒绝，不伪造空报告。

自动验证先核对执行配置，再重新计算正式项目树与草稿指纹；三者均匹配且收据
属于最新已确认验证时返回原始报告，发布automatic_validation_reused，不启动
命令、不新增工具尝试或重置额度。编译失败的完整报告也可复用，仍是失败证据。
源码/草稿变化则产生新验证批次。无法取得完整源码指纹时失败关闭，不能猜测
缓存有效。现有项目树指纹的路径排除与依赖规则继续适用，不声称外部依赖变化
或同路径工具二进制升级都会被检测。

宿主确认新格式独立验证的applied结果时，同步保存匹配草稿的完整报告收据；
没有冻结配置的旧记录仍可确认，但不补造其报告版本。检查点总体4MiB限制仍
适用，各上限不是同时容量承诺；超限保存保留未知效果，不能截断后确认。
实现标志已升级为libai-coding-execution-v2-validation-cache：带旧执行摘要的记录
不会静默迁移，新二进制恢复会拒绝不同语义版本；旧无摘要记录保持明确兼容。
尚未提供语义版本迁移/外部依赖指纹管理或总时限迁移入口。

## 存储契约

实现 `libai::execution_store_t`，设置 `run_options_t.execution_store` 和
`execution_scope`。scope 是宿主拥有的稳定任务 ID（1–128 个 ASCII 字母、数字、下划线或短横线）。
恢复同一任务必须沿用 scope，输入和系统指令必须匹配。

- open：取得该 scope 的独占执行租约，并读取 revision/state_json；新任务两者为空。
- commit：在同一租约下持久化比较并交换，校验 expected_revision；成功返回新的非空 revision。
- close：释放租约，不抛异常。open 失败时存储适配器负责释放已取得的资源。
- 适配器负责身份、隔离、加密、容量、原子性和落盘；rename 本身不代表持久化完成。
  不可对仍在运行的任务强行夺取租约。存储接口是受信任的宿主接口，不能注册为模型工具。

快照含对话、原始参数、执行意图和输出；可能含项目内容。宿主应按自己的数据保护规则保存。
SDK 不包含模型实例或凭证，不提供默认明文文件存储。POSIX 测试适配器验证进程租约、
原子替换、文件和目录 fsync；该测试实现不是生产存储包。

## 执行与恢复

模型调用前保存尝试计数；模型返回工具批次后，先保存完整批次及对话。
每个工具通过 Schema 和当前授权后，再提交 running 意图，之后才调用处理器。
`tool_context_t.idempotency_key` 是 scope 加日志序号，来自宿主与日志，不来自模型。
把它传给支持幂等请求的外部服务；不同工具调用有不同键，不自动推断业务意图是否相同。

成功工具结果与对话一起提交后标记 done。已确认结果在恢复中复用，不重跑处理器。
尚未开始的调用按保存的原始批次继续，不向模型重新索取这一批。
工具名称、参数 Schema 和 `tool_spec_t.execution_revision` 必须匹配当前注册。
工具实现语义改变时，宿主必须修改 execution_revision；库无法识别任意函数代码变化。

running 表示执行已获准但结果未确认：崩溃可能发生在实际操作之前或之后。
恢复默认返回 recovery_required，不重跑、不让模型重新规划其它副作用。
处理器返回失败或抛异常同样不能证明无副作用；日志保持 running 并停止。
成功处理器后的结果提交失败也保留不确定状态。此阶段没有自动重试副作用工具。

宿主核对外部收据后，可以调用 `confirm_execution_effect`，提供当前 revision、键、
确定的 none/applied 结论、工具结果和非空收据引用。接口取得独占租约、拒绝陈旧确认，
并把结果与执行位置一起提交。它不验证外部收据本身，不能由模型自称完成来调用。
再次运行会继续剩余批次；已完成的 scope 返回保存的回答，不再调用模型或工具。

计数与工具位置跨重启保留；当前工具预算仍适用。新日志保存原始开始时间及总时限，
恢复时不重新获得完整时限；规则见下节。取消仍为本次调用的协作式控制。
模型请求可能在崩溃后重发，付费请求并非 exactly-once。
JSON 快照受 2 MiB / 深度64 / 65,536 节点的验证限制；超限不会绕过存储确认执行工具。

## Agent 跨进程总时限

新执行日志的 `time_budget` 保存原始 Unix 毫秒开始时间、最后观察时间、总时限和来源。
有限时限包含进程停止时间；恢复只能收紧，不能延长或改为无限。收紧按原始开始
时间计算，不是从恢复时刻重新计时。`timeout=0` 一致表示无限；负值和超范围值拒绝。
单次调用同时使用单调期限，墙上时钟倒退返回 recovery_required；这不是防时钟篡改机制。

首次日志提交包含启动和存储打开消耗的时间。保存模型尝试/工具意图后再次检查期限，
存储延迟耗尽时限时不启动处理器；日志记录明确的未执行结果。已启动回调仍需协作
取消，库不能强制中断宿主代码。未确认副作用优先要求宿主核对，不因超时改报普通
结束，也不在核对前改写日志。已完成结果可在期限过后读取缓存，不发起新模型工作。

旧的未完成日志缺少可靠时间时，拒绝自动续期；旧的已完成日志仍可读取。SDK 的
`adopt_execution_deadline(store, scope, input, instructions, revision, remaining, reference)`
供可信宿主迁移：宿主独立核实剩余预算后，以当前租约/CAS版本和非空审计引用建立
新起点。它不会推断历史耗时、解除未知效果或验证引用真实性，不可暴露给模型。
SDK、独立 C++、C、Python、Rust、Java和Objective-C均提供迁移入口，见下表。
Java/Rust APP已接入下述可核实旧时限迁移；其它宿主或无法核实的日志仍需宿主
审计管理入口。不得手动删日志绕过检查。

### 各语言旧日志迁移入口

| 接口 | 剩余预算 |
| --- | --- |
| SDK `libai::adopt_execution_deadline` | `std::chrono::milliseconds` |
| 独立 C++ `libai::cpp::adopt_execution_deadline` | `std::chrono::milliseconds` |
| C `libai_execution_adopt_deadline` | `int64_t remaining_ms` |
| Python `adopt_execution_deadline` | `remaining_ms` 整数，拒绝 bool、负值和溢出 |
| Rust `adopt_execution_deadline(store, DeadlineAdoption)` | `remaining_ms: i64` |
| Java `Execution.adoptDeadline` | `remainingMillis: long` |
| Objective-C `LAIExecution adoptDeadlineForStore:…` | `remainingMilliseconds: int64_t` |

各入口共享scope/input/instructions身份检查、当前revision与reference。零是宿主明确
确认的无限预算，不能由模型或自动恢复选择。陈旧revision、已存在时间预算、完成
缓存、空reference或超范围期限均拒绝，不改写日志。成功仅保存新的时间来源和起点，
不会运行模型、核对副作用或消耗工具额度；原副作用记录和计数保留。

Python在旧原生库上保留既有Agent功能，调用迁移时明确报告缺少新ABI。Java新版
迁移方法同样需要新版JNI；其余既有方法保持兼容。ABI版本仍为1，新增符号不改变
既有类型布局。移动包支持轻量运行时，不因此提供桌面文件存储或APP迁移界面。

## 当前验证与剩余工作

测试覆盖写入失败、提交失败后的副作用不重跑、陈旧收据拒绝、剩余批次继续、预算降低、
完成结果复用和损坏状态拒绝。真实子进程在 fsync 外部效果后立即退出，父进程重新构造
Agent，确认处理器和模型均未自动重跑；核对收据后继续且效果文件只有一个标记。

各语言存储接口、显式安全重放和跨进程总时限已接入；MCP、结构化输出、工作流及RAG
也已提供独立接口。仍需生产存储适配、C++ APP完整收据核对与日志统一、更多故障注入
及平台运行验证。总体目标尚未完成。

下列各语言与适配器章节保留分阶段实施和测试记录，其中“此阶段”“尚待接入”等说明
描述当时状态；当前能力以本文开头、后续对应接口章节及[平台实施记录](AGENT_PLATFORM_PROGRESS.md)为准。


## C ABI 与 Python 接入

C 使用 `libai_agent_set_execution_store(agent, scope, callback, user)` 配置空闲 Agent。
回调接收 open/commit/close 操作，通过 `libai_execution_result_set` 复制返回值；
回调、user 及存储对象必须活到 Agent 不再使用它们为止。传入 NULL 回调关闭持久化。
工具回调内调用 `libai_invocation_idempotency_key`，需在回调返回前复制借用字符串。
`libai_execution_confirm` 是核对收据后的宿主接口，不能注册为模型工具。

Python 实现 `ExecutionStore.open/commit/close`，通过
`agent.set_execution_store(scope, store)` 接入，传入 None 关闭持久化。
open 返回 `(revision, state_json)`，commit 返回新 revision；状态是原始 JSON 文本。
Agent 保留存储回调及对象，工具通过 `invocation.idempotency_key()` 取得键。
Invocation 离开回调即失效；如需延后使用，应保存键字符串。
核对外部收据后调用 `confirm_execution_effect(store, scope, input, instructions,
revision, key, output, outcome, reference)`；outcome 为 none/applied。

open 抛异常时，存储实现必须自行清理本次部分获取的资源，不能释放其他运行的租约。
Python 只在 open 成功返回但返回值无法转换时释放本次租约。
close 必须可靠释放且不抛异常；原生 noexcept 关闭边界不能把关闭异常传回调用者。
配置不执行 open，实际运行或收据确认时才获取租约。非法配置保留原配置。

实际 Python/C ABI 测试覆盖结果提交失败、陈旧确认、冻结批次继续、完成结果复用、
借用回调失效及租约失败隔离。POSIX 子进程在 fsync 外部效果后退出，重新创建 Agent
停止等待确认；确认后恢复并复用结果，外部效果保持一份。测试文件存储不是生产适配器。


## 独立 C++ facade

`<libai_cpp/execution.h>` 定义独立 `ExecutionStore` / `ExecutionSnapshot`，不引用
SDK 头文件。实现宿主存储后，设置 `RunOptions.execution_store` 和 `execution_scope`。
处理器从 `ToolContext.idempotency_key` 复制效果键；`ToolSpec.execution_revision`
默认 1，工具实现语义变更时必须更新，以阻止旧批次使用新实现继续执行。

`libai::cpp::confirm_execution_effect` 接收独立 `ToolResult`，参数和信任边界与 SDK
相同。存储对象通过 shared_ptr 保持到 run/确认返回；私有适配器位于 src，公开类型
不依赖 SDK。预算、授权及存储失败仍按共享运行规则处理，既有 reset 只清理内存会话，
不会删除持久任务；新逻辑任务需要宿主选择新的 scope。

封装测试覆盖不确定效果停止、陈旧确认拒绝、剩余批次继续、工具版本变更拒绝、
恢复预算降低、完成缓存及租约失败隔离。跨进程实际崩溃另外由 SDK/Python POSIX
用例覆盖；尚未做 C++ facade 的 Windows/移动端运行验收。


## Rust 存储接口与工具版本

Rust 实现 `ExecutionStore`，使用 `Rc<RefCell<dyn ExecutionStore>>` 共享给 Agent 与宿主。
`agent.set_execution_store(scope, Some(store))` 保留稳定的回调存储地址；None 关闭持久化。
`Invocation.idempotency_key()` 在回调期间返回独立字符串，Rust 借用生命周期阻止
Invocation 脱离回调。Agent 与存储仍是本线程接口，不增加 Send/Sync 承诺。

确认外部效果后，构造 `ExecutionReceipt` 并调用 `confirm_execution_effect`。
存储借用冲突返回错误，panic 捕获后反馈给 SDK，不越过 FFI；open 失败时宿主清理
自己的部分获取资源，close 必须可靠且不 panic。测试覆盖这些边界及结果提交失败恢复。

C 新增 `libai_agent_add_tool_versioned`，旧注册入口继续使用版本 1。
Python `add_tool(..., execution_revision="1")` 与 Rust `add_tool_versioned` 可显式
声明工具版本。注册时拒绝空版本或超过128字节；恢复冻结批次时拒绝与注册版本不同
的工具。Java/Objective-C 目前仍使用默认版本，待接入新日志时补齐。


## Java / JNI 持久执行

实现 `Execution.Store`，open 返回 `Execution.Snapshot`，commit 返回新的 revision，
close 可靠释放租约且不抛异常。`Agent.setExecutionStore(scope, store)` 在空闲时配置，
传入 null 关闭持久化；Java Agent 持有 store，原生回调借用当前 JVM 调用线程。
需要显式 close Agent，以释放 JNI 对 Agent 的全局引用。

`Invocation.idempotencyKey()` 返回独立字符串；离开回调或跨线程访问 Invocation
被拒绝。`addTool(..., executionRevision, handler)` 声明工具实现版本；原重载仍默认1。
保留原 nativeAddTool JNI 入口，新增版本注册使用独立入口，避免改变旧入口参数布局。
核对外部收据后调用 `Execution.confirm`，接口仍校验任务、revision、key 和结论。

Java 存储异常通过 JNI 转换为日志失败，不越过语言边界；open 抛异常不调用 close，
存储实现必须自行清理自己的部分获取资源。open 成功但返回空/非法快照字段时释放
本次租约。持久数据文本须是合法 Unicode 且无 NUL。JNI LocalFrame 限制每次回调引用。
测试启用 -Xcheck:jni，覆盖提交失败、收据确认、版本不匹配、旧配置保留、租约隔离、
回调过期、非法快照及存储异常。尚未完成 Android 设备运行与三个 APP 的新日志接入。


## Objective-C 存储与确认

实现 `LAIExecutionStore`，通过 `setExecutionStore:scope:error:` 配置空闲 Agent，nil
关闭持久化；Agent 保留存储到替换或关闭。open 返回 `LAIExecutionSnapshot`，commit
返回新 revision，nil/NSError 表示失败；存储实现负责失败获取时的局部资源清理。
open 成功但快照文本非法时释放本次租约。close 必须可靠且不抛异常。

`LAIInvocation.idempotencyKey` 返回复制的 Foundation 字符串，Invocation 回调结束
或跨线程后访问会抛异常。`addTool` 的执行版本重载和 `LAIExecution.confirmStore`
复用共享日志规则；旧注册方法仍使用版本1。Objective-C 与 C++ 异常不穿越存储回调。

macOS Foundation 测试覆盖提交失败、确认与缓存重放、版本不匹配、租约隔离、过期
Invocation、非法快照清理与存储异常。尚未证明 iOS 真机运行、生产存储或 APP 接入。


## POSIX 文件适配器

`<libai/sdk/file_execution_store.h>` 提供 `file_execution_store_t(root, codec)`，
用于本地 POSIX 文件系统。宿主预先创建绝对路径私有目录（当前 uid、0700）；文件
要求当前 uid、0600、普通单链接文件。适配器固定目录 fd，使用 openat/O_NOFOLLOW、
非阻塞文件打开、flock 独占租约、版本 CAS、独占临时文件和原子 rename，并在文件
及目录 fsync 成功后返回。已有损坏文件、符号链接或不安全权限失败关闭。

每实例只持有一个 scope，可创建多个实例服务不同任务。close 不删除锁文件；宿主
不得替换/清理正在使用的锁文件，也不得在持有租约时 fork。存储目录及父目录由
受信任宿主管理；不抵御同 uid/管理员恶意改写、不保证网络文件系统语义。
fsync 依赖操作系统和文件系统保证，没有提供跨平台断电故障验证。

codec 必须显式提供 encode/decode（scope, revision, data）；加密适配必须认证 scope、
revision 与载荷并拒绝认证失败。`execution_file_codec_t::plaintext()` 是显式明文选项，
只适合宿主允许明文的场景，不能当作密钥保护。尚未提供内置认证加密和数据库适配。
解码状态上限2 MiB，编码载荷上限4 MiB。回调不可重入 close。

真实进程退出测试已使用此适配器验证不确定效果停止、收据确认、完成结果复用，
另覆盖文件 CAS、争用租约、FIFO、符号链接、目录权限、损坏版本和编码失败。
此阶段仅 SDK 提供文件适配；独立封装工厂、语言接口及 APP 接入仍待完成。


## 独立 C++ 文件存储工厂

公开 `ExecutionFileCodec` 与 `create_file_execution_store(root, codec)`，返回
`shared_ptr<ExecutionStore>`，可直接配置 `RunOptions.execution_store` 或用于收据确认。
公开头文件不引用 SDK，文件实现位于 `libai-c++/src/file_execution_store.cpp`。
codec 仍须显式选择，明文用 `ExecutionFileCodec::plaintext()`；宿主加密回调必须认证
scope/revision/载荷，不能把简单编码当作加密。

`file_execution_store_available()` 在 POSIX 构建返回 true，其他平台返回 false，
工厂抛明确不可用错误；这不等于 Windows 已有文件存储后端。私有目录、租约、落盘、
容量与宿主信任边界沿用 SDK 适配器要求。测试通过公开 API 验证真实进程退出后停止、
收据确认与结果复用，外部效果始终一份。C/Java/Python/Rust/Objective-C 文件工厂及
APP 接入仍待实现。


## C ABI / Python 文件存储

C 提供 `libai_file_execution_store_available` 与显式
`libai_file_execution_store_create_plaintext`，以及 open/commit/close/destroy。
open 返回 owned JSON `{revision,state_json}`，commit 返回 owned revision，均用
`libai_string_free` 释放。失败返回 NULL/-1，可读取 `libai_last_error`。
持有租约时 destroy 拒绝；错误 scope 的 close 不释放有效租约。
不得并发销毁正在调用的句柄；注册为回调存储后，宿主保持句柄到使用结束。

Python `FileExecutionStore(root, plaintext=True)` 实现既有 ExecutionStore，可直接
传给 Agent.set_execution_store 与 confirm_execution_effect；不再需要复制测试存储代码。
必须显式 plaintext=True，默认拒绝创建，不能将明文存储当作凭证保护。
close(scope) 释放租约，dispose() 释放空闲原生句柄，context manager 管理句柄；Agent
回调保留对象，但宿主不能在后续运行仍需存储时提前 dispose。

实际 C ABI/Python 测试覆盖句柄销毁拒绝、租约争用、错误 scope、CAS 和 Unicode，
并用内置文件适配器运行 fsync 后子进程退出、收据确认及结果复用。C/Java/Objective-C
构建登记共享实现，Java/Rust/Objective-C 的公开文件工厂、加密与 APP 接入尚待完成。


## 文件格式统一为 JSON

新提交的 `.state` 文件为 UTF-8 JSON v1，包含 version、scope、revision、encoding 与
payload。encoding=hex 将 codec 的任意二进制载荷安全装入 JSON；hex 不提供加密。
文件名保持不变，同一 scope 的锁和 CAS 不变。载荷上限4 MiB、解码状态上限2 MiB；
JSON 包装上限8 MiB +256字节。仅受信任存储解析使用此包装上限，模型参数和 Schema
解析仍保留2 MiB、深度64、节点65,536及重复键拒绝规则。

旧 revision 换行载荷格式仍可读取，读取不改写。下一次成功 commit 在同一租约下
原子写为 JSON；失败不删除旧日志，不新建任务或重放不确定效果。未知版本、重复键、
错误 scope/encoding、非法 hex 和损坏版本均失败关闭。
测试将真实崩溃后的 running 快照转换为旧格式，确认恢复停止且读取不改写；宿主
收据确认后迁移为 JSON，继续执行及结果复用均保持外部效果一份。二进制 codec 与
2 MiB 状态往返测试确认新包装不收窄现有载荷上限。


## Rust 内置文件存储

`FileExecutionStore::plaintext(root)` 是显式明文工厂，实现 ExecutionStore，无需 JSON
crate。用 `Rc<RefCell<FileExecutionStore>>` 共享给 Agent，或转换为 SharedExecutionStore。
根目录、JSON 包装、租约和落盘规则沿用 SDK 文件适配。`available()` 查询平台可用性。
`release(scope)` 报告关闭错误；trait close 不 panic，错误 scope 保留原租约。
最后一个 owner Drop 时释放自身租约和句柄；Agent 回调的 Rc 保持存储活到使用结束。
不增加 Send/Sync 承诺，也不提供加密保护。

C ABI 增加 open_values 的两路 owned string 输出，避免 Rust 为读取快照引入 JSON
依赖。失败输出均为空，成功调用者分别 libai_string_free；读取验证状态 JSON/UTF-8，
两路内存分配失败时释放本次租约。Rust 在复制后释放两路内存。

测试用新的独立进程执行工具，外部效果 fsync 后退出；重新构造 Agent 停止等待确认，
确认后继续并复用结果，效果文件始终一个字节。避免在多线程 Rust 测试框架内 fork。
另覆盖 owner Drop 解锁、CAS/错误 scope、Unicode 和 JSON 文件格式。
Java/Objective-C 文件工厂、加密适配及 APP 接入仍待完成。


## Java 内置文件存储

`FileExecutionStore.plaintext(root)` 实现 Execution.Store 与 AutoCloseable，明确选择
无加密的本地 JSON 存储。available 查询可用性，open/commit/close(scope) 复用既有
租约协议，close() 仅释放空闲原生句柄；有租约时拒绝销毁，错误 scope 不解锁。
Agent 保留 store，但使用者必须在所有 Agent 用户结束后显式 close 或使用 try-with-resources。

JNI 使用两路 C 快照输出并及时释放原生内存，LocalFrame 限制引用。成功取得租约后
若 JNI/Java 快照构造失败，释放本次租约；获取失败不解锁其他持有者。输入要求合法
Unicode 且无 NUL。仍无 JSON Java 库依赖，也未提供加密或自动 GC 关闭保证。

-Xcheck:jni 测试覆盖活跃租约关闭拒绝、CAS、错误 scope、Unicode 与 JSON 格式。
新 JVM 工具 fsync 外部效果后 halt，父 JVM 重建 Agent 等待收据确认，恢复与结果复用
保持效果一份。Objective-C 文件工厂、加密适配及 APP 的新日志接入尚待完成。


## Objective-C 内置文件存储

导入 `<LAIFileExecutionStore.h>`，使用 `initWithPlaintextDirectory:error:` 显式选择
无加密 JSON 存储。对象实现 LAIExecutionStore，可直接配置到 Agent 或用于收据确认。
available 查询平台支持，releaseScope:error: 报告关闭错误，错误 scope 不释放原租约。
close:error: 仅销毁空闲句柄；dealloc 在最后一个 owner 消失后释放自身租约。
Agent 强持有存储，宿主仍须管理所有使用者的生命周期，不能提前关闭后续要用的存储。

C 两路字符串由私有 RAII 释放，Foundation 快照转换失败时释放本次租约。公开头文件
不包含 C ABI 指针，模块图登记新头文件。macOS 测试使用 NSTask 启动新进程，在效果
fsync 后退出；恢复、确认和缓存结果保持效果一份，并覆盖 owner 释放、CAS、Unicode、
JSON 格式及错误 scope。所有语言文件入口已补齐，认证加密、更多平台运行验证和 APP
新日志接入仍待完成。


## 宿主声明无副作用工具

SDK tool_spec_t 与独立 C++ ToolSpec 新增 side_effect_free，默认 false。此声明只由
受信任宿主注册，模型参数或工具名称不能覆盖它。无副作用指处理器不产生外部业务
副作用，不能把发送消息、写入、执行用户代码等能力仅凭名称标为安全。

声明为 true 的工具错误可作为已确认错误返回模型；结果提交失败或进程退出留下的
running 调用可按冻结参数、同一幂等键重试。每次处理器尝试都计入持久预算，授权与
Schema 仍重新检查；版本、Schema 或无副作用声明变更时拒绝恢复。默认写入工具
running 仍停止等待收据确认，不自动重试。旧日志缺少声明时按 false 处理。

每个效果记录 attempts，恢复核对累加值与全局 tool_calls，防止重启重置预算。
C 新增 add_tool_governed；Python add_tool(side_effect_free=True) 严格要求 bool；
Rust add_tool_governed 提供类型化布尔，原注册方法保持 false。Java/Objective-C 新声明
尚待接入，已有存储接口继续默认安全关闭。

此阶段测试覆盖只读错误反馈、结果提交失败后重试、原键保持、预算降低、声明变化
及模型伪造参数不能改变策略。尚未接入 Rust APP：其模型回调中编译验证和内存草稿
必须先迁移到可记录的宿主步骤，并冻结恢复所需的任务上下文，才能安全启用新日志。


## Java / Objective-C 安全重试声明

Java 新 addTool 重载接收 executionRevision 与 sideEffectFree 布尔；Objective-C 新
selector 接收 executionRevision:sideEffectFree:。原注册方法均默认 false，保留 JNI
原始与版本化入口的参数布局。工具名称、模型参数或响应不能设置宿主声明。

两种封装的测试覆盖已声明读取的错误反馈、结果提交失败后按原键重试、声明变化
拒绝恢复、降低预算阻止重试、尝试总数保持及完成缓存。共享日志校验尝试计数与
已执行结果的对应关系。所有语言声明入口已补齐；需要宿主确保声明符合真实行为。

此补齐不代表 APP 已启用持久执行。Rust 的编译验证需从模型回调迁移为宿主步骤，
草稿与恢复上下文需持久化；三个 APP 新日志接入、认证加密及其它平台运行仍待完成。


## Java/Rust APP 的可核实旧时限恢复

仅在旧SDK日志缺少time_budget且未终结时，APP在受控续跑流程尝试迁移。它查找
当前账户下id等于execution_scope的唯一原始运行记录，不使用续跑记录的新开始
时间，不接受请求参数提供的时限。原始durable标记、冻结上下文version=1、scope、
input、instructions及项目路径须匹配；时间/策略必须是确切整数且不溢出。

v1预算按原始冻结策略的provider_request_timeout_seconds乘(tool_calls+1)计算，
与原APP公式一致。started_at为秒级，按该秒起点保守扣减启动/停机时间，不伪装为
精确毫秒或恢复未知历史单调时间。时钟已明显倒退、字段缺失、原始记录删除或身份
不匹配均保持阻塞；这不是防时钟篡改/记录伪造方案，原始APP记录由可信宿主管理。

宿主先核对现有副作用收据，再用CAS和审计引用建立剩余时限。无收据的running
业务操作不迁移、不自动重放。已明确side_effect_free的读取仍按SDK安全重放规则。
已耗尽原预算则返回deadline_exceeded，保留检查点和原日志；不能把remaining=0
误用为无限预算，也不能自动提升额度，需新建任务。已完成缓存无需迁移或模型调用。

模型请求的timeout_ms取管理员单次策略与Invocation剩余总时限的较小值；已经停止
不发出新请求。传输仍为协作取消，不保证强制终止任意阻塞回调。审计使用真实本地
request_timeout_ms，而不把该传输配置作为模型HTTP协议字段发送。

真实Java/Rust进程退出测试各10场景覆盖有效/收据迁移、临近到期、过期、未来/缺失/
浮点时间、缺少策略/原始记录和未知效果。验证不重复提案，迁移后的预算较原始
预算减少，拒绝场景不修改SDK文件。未核实的其它旧日志及C++ APP日志继续待接入。

## 宿主只读恢复诊断

六语言已有的 governance_call / Governance.call / governance 入口新增
`executions.inspect`，不需要完整编程后端。输入是包含五个字符串的 JSON 对象：
`scope`、`revision`、`state_json`、原始 `input` 和原始 `instructions`。
宿主先通过自己的执行存储取得快照并释放租约，再传入快照副本；诊断不会访问
文件、获取真实租约、提交状态、调用模型或工具，也不会自动确认外部收据。
输入整个外层 JSON 仍受 2MiB/深度64/65536节点限制；JSON 转义开销也计入大小。

诊断复用正常恢复的日志结构、身份、对话/调用配对和计数校验；无法核实返回
`ok=false`，不猜测剩余进度。正常结果含版本1、快照 revision、phase/cursor、
模型/工具计数、时限存在/到期/时钟倒退标记及每项 effect 的 key、调用ID、名称、
原始参数、执行版本、state、attempts、证据和无副作用声明。
`resume_policy` 分别为 reuse_result、validate_and_authorize、
validate_and_authorize_replay、host_receipt_required。

`recovery` 按优先级提示 new_task、host_reconciliation、clock_reconciliation、
cached_result、cached_output_failure、deadline_adoption、deadline_exceeded 或
resume_pending。未知业务意图优先提示核对，即使时限同时耗尽；完成缓存在正常
时钟下仍可复用。诊断时不推进 last_seen，历史时钟异常可显示，实际执行依旧失败
关闭。缺少旧时限只提示采用可信宿主迁移，不推断新的开始时间或无限预算。

这些字段描述日志，不是续跑授权。宿主仍须核对当前工具版本、当前权限、外部
收据、真实存储 revision/CAS 和执行期限；side_effect_free 是宿主声明。查询后
日志可能变化，不得仅凭旧快照确认副作用。参数和证据可能包含业务私密数据，
管理入口必须鉴权；不能直接向模型暴露快照、诊断或收据确认接口。
此接口为 APP 恢复管理提供统一核心，尚未代替三个 APP 的管理界面接入。

### Java/Rust编译命令收据保留

原生验证器在项目私有.libai_agent/validation_runs/<父键SHA256>/project建立
一次性验证范围；同父键已有目录拒绝创建，不清理或重跑旧命令。报告保存receipt_root
及parent_idempotency_key，命令在派发前关联父键。正常结束清理源码及构建输出，
保留.libai_agent命令意图、结果摘要和父关联。崩溃时尚未清理的快照留待宿主
核对；本阶段不自动清理未知执行，不提供跨进程任意回调自动确认或统一管理UI。

主目录Java/Rust重建并各通过10个真实Python/Go场景；检查正常清理后仅剩
私有元数据、父关联涵盖全部报告命令、finished退出码与报告匹配。初轮Java
跨线程读取Invocation失败已修正为派发前复制父键；Rust测试规范化macOS
路径别名后通过。未模拟进程崩溃；缓存报告的子收据再次核对见下节。

### Java/Rust缓存报告核对

复用当前成果的passed报告前，宿主按父键重新计算私有收据位置，检查父关联及
所有唯一命令键、项目和命令身份、finished/confirmed_applied状态、成功进程标志，
并核对报告的started、exit_code、cancelled、timed_out、elapsed_ms。缺失、损坏、
running、目录不匹配或父关联不完整均阻止完成，不自动编译、换键或清除收据。
旧报告没有这些字段时也不能仅凭passed复用；历史报告保留，不改写成编译失败。
当前修改需要编译时，旧not_required报告不能代替编译证据。

核对使用真实CodingService读取收据，不直接绕过存储验证；逐项读取不声明跨文件
原子快照，也不把宿主确认当成外部效果签名。后续仍需统一确认与父提交的边界。

主目录两个APP重建，各15项真实编译/故障注入场景通过，五种命令收据问题
均阻止完成且无额外模型请求或编译。测试复原注入记录后核对全部原始命令事实。
未模拟物理断电或分布式存储，也未将历史编译成功报告自动改为业务成功。
