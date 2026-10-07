# AI 代码修改后的自动编译与修复

每批增量修订保存、物化到隔离草稿后，由运行时自动调用构建。普通实现任务不再依赖模型主动调用 `workspace.validate` 或用户手动编译。互相依赖的多个文件应作为同一批补丁提交。

macOS/Linux 项目根目录存在 `build.sh` 且有管理员允许的构建工具链时，在现有沙箱内通过固定解释器执行该文件。自动构建检查点只执行脚本，避免再跑一遍默认构建；没有脚本时使用固定工具链命令。完整验收仍保留原有构建与测试，以生成其需要的标准产物。Windows 当前继续使用固定工具链构建，本次没有新增 `build.bat` 解释器支持。

脚本不接收模型生成的命令行字符串或 WebCool 服务环境。PATH 来自服务器发现的编译器目录；自定义安装位置的工具链只读授权、macOS CC/CXX/SDKROOT 来自已有工具链配置。脚本沿用沙箱超时、资源限制、取消和管理员网络策略。脚本失败不会静默切换为其他构建命令掩盖失败。

编译报告包含命令标识、退出码、标准输出与错误诊断及可解析的文件行号。报告进入下一轮 AI 请求和持久化工具记录，要求 AI 优先修复编译错误；再次保存修订后自动重新编译。运行界面显示验证工具执行状态。

普通任务的中间检查标记为 `validation_scope=build-only`、事件来源 `automatic_build`，不运行全量测试，也不将编译成功冒充整个需求已完成。明确的局部编译修复仍沿用 `FOCUSED_COMPILE_REPAIR.md` 的收敛规则；最终回复携带的补丁仍进入既有最终验证流程。

所有执行发生在隔离草稿中，不提前覆盖正式项目或停止正在运行的程序；正式目录下的旧可执行文件不会因为草稿验证而被替换。持续失败受原有调用额度、无进展控制与取消约束。

## 验证命令

在 webcool 目录运行：

```sh
python3 -B tests/compile_repair_http_test.py --automatic-build
python3 -B tests/compile_repair_http_test.py --automatic-build-no-script
python3 -B tests/compile_repair_http_test.py --focused-go
python3 -B tests/compile_repair_http_test.py --full-compile
python3 -B tests/compile_repair_http_test.py
```

新增自动构建用例使用本地模拟模型：第一轮提交有真实编译错误的 C++ 源码，第二轮必须收到编译诊断并修复，第三轮提交最终回复。测试检查脚本输出标记，确认脚本确实执行，并检查失败和修复后的自动编译报告。无脚本用例验证固定工具链回退。测试在临时项目和独立服务进程内执行，不调用真实模型。

新逻辑需要重启现有 webcool 进程后生效。
