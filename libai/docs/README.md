# libai 开发说明文档

本目录集中保存本轮模块提取、重构及验证说明。文档按阶段保留，早期文档中的旧路径和构建状态属于当时记录；当前集成和构建方式以 [libai README](../README.md) 为准。

| 文档 | 内容 |
|---|---|
| [AI编程静态库拆分说明](AI编程静态库拆分说明.md) | 初次从 webcool 提取静态库 |
| [libai模块迁移说明](libai模块迁移说明.md) | 迁移至 aicool/libai |
| [libai功能目录说明](libai功能目录说明.md) | 按功能整理目录 |
| [libai公共能力抽象说明](ABSTRACTION_REVIEW.md) | 全部源码分析及公共能力抽象 |
| [libai长函数拆分说明](libai长函数拆分说明.md) | 超过 200 行函数的拆分 |
| [coding_tool_loop功能拆分说明](coding_tool_loop功能拆分说明.md) | 编码循环按阶段分文件 |
| [工具与工作区功能拆分说明](TOOLS_WORKSPACE_SPLIT.md) | 工具处理与工作区分文件 |
| [运行时维护说明](RUNTIME.md) | 编码循环和工具实现的职责边界 |
| [工作区维护说明](WORKSPACE.md) | 工作区文件职责、缓存和构建依赖 |
| [沙箱超时与贪吃蛇故障修复](SANDBOX_TIMEOUT_SNAKE_FIX.md) | 阻塞原因、超时及取消修复、游戏验证 |
| [局部编译修复完成条件](FOCUSED_COMPILE_REPAIR.md) | 范围判定、相关测试、停止条件与回归 |
| [自动编译与错误修复](AUTOMATIC_BUILD_REPAIR.md) | 代码修订后执行脚本、自动反馈诊断与重编译 |
| [工具额度与断点修复](TOOL_BUDGET_RECOVERY.md) | 统一计数、有限补读、诊断源码定位与无效重试限制 |
| [工具调用效率优化](TOOL_CALL_EFFICIENCY.md) | 批量续读、源码保留、提前调查检查点与有界测试修复 |
| [snake2 可玩性诊断](SNAKE2_PLAYABILITY_DIAGNOSIS.md) | 玩家控制与画面错位、无效推进测试及调速数据竞争 |

- [任务契约与验收闭环（第一版）](TASK_CONTRACT_ACCEPTANCE.md)

- [浏览器运行与验收](BROWSER_ACCEPTANCE.md)：草稿服务、真实 Chrome 运行、声明式交互断言和环境阻塞处理。

- [智能体回复展示](AI_REPLY_DISPLAY.md)：从协议 JSON 提取可读正文、需求进度及剩余事项。
