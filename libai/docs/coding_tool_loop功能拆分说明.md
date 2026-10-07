# Coding 工具循环实现

`coding_tool_loop_t` 仍由 `ai_agent_loop.cpp` 创建并同步执行。拆分保持同一个实例及原阶段调用顺序，各实现文件定义该类对应职责的方法。

| 文件 | 职责 | 行数 |
|---|---|---:|
| `coding_tool_loop.cpp` | 主循环、单轮阶段编排 | 80 |
| `coding_tool_loop_startup.cpp` | 初始上下文、检查点恢复、验证能力发现 | 356 |
| `coding_tool_loop_request.cpp` | 模型请求组装、请求日志 | 232 |
| `coding_tool_loop_transport.cpp` | 流式回调、后台响应续取、重试策略 | 392 |
| `coding_tool_loop_response.cpp` | 响应解析、预览处理、最终回答校验 | 325 |
| `coding_tool_loop_tools.cpp` | 工具准入、执行、草稿落地与进度观察 | 421 |
| `coding_tool_loop_repair.cpp` | 修复收敛、提案持久化与验证完成 | 270 |
| `coding_tool_loop_feedback.cpp` | 下一轮反馈、进度限制、压缩与预算结束 | 381 |
| `coding_tool_loop_context.cpp` | 修复证据、上下文压缩、进度快照保存 | 235 |

`coding_tool_loop.h` 保留调用参数、类声明和跨阶段状态；`coding_tool_loop_internal.h` 定义各阶段共用的单轮状态 `coding_turn_t`，以及 `coding_loop_detail` 命名空间下的内部辅助函数声明。流式观察器只在 transport 实现文件中定义。

主循环文件由 2,633 行缩至 80 行。41 个原函数定义已逐项对比确认未改变实现；共享辅助函数的默认参数仅从定义移至声明。没有改变公开入口、状态对象布局、重试规则、错误处理或持久化顺序；上次函数不超过 200 行的限制保持成立。

Make 和 CMake 的源码通配规则自动包含新增文件，MSBuild 已逐项登记，并校验路径及重复项。现有 make clean 通配规则也覆盖新增对象和依赖文件。

验证通过：libai 独立构建及全归档链接、webcool arm64/x86_64 严格构建、27 项库和模块测试、认证 HTTP 集成及恢复流程、真实编译失败后的模型修复。make clean 的演练确认覆盖全部 9 个实现文件的对象和依赖文件。Windows/Linux 尚未实机编译。
