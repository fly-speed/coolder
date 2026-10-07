## workspace/agent_workspace.cpp

原文件 1938 行，拆分为 7 个实现文件。

| 文件 | 职责 | 行数 |
|---|---|---:|
| `agent_workspace.cpp` | 路径规范化、敏感路径策略、路径组件检查 | 213 |
| `agent_workspace_roots.cpp` | 外部项目根注册、解析、状态目录及注册锁 | 305 |
| `agent_workspace_io.cpp` | 受限文本读取、临时文件写入与内容摘要 | 144 |
| `agent_workspace_query.cpp` | 列表、读取、搜索、大纲 | 325 |
| `agent_workspace_mutations.cpp` | 创建、替换、删除、可执行权限等变更 | 365 |
| `agent_workspace_fingerprint.cpp` | 文件/目录指纹及线程缓存 | 248 |
| `agent_workspace_dependencies.cpp` | 依赖清单解析、依赖文件与构建树复制 | 335 |

## 维护边界

- 两个 internal.h 只承载各实现文件共用的状态类型和辅助函数声明，原公开接口保持不变。
- 工作区根注册互斥锁与注册缓存仍归 roots 文件单独管理；指纹缓存为一份 thread_local 定义，通过内部 extern 声明供依赖复制复用，避免多份缓存造成语义差异。
- 107 个原函数定义已逐项比较，函数体原样保留；仅跨文件辅助函数调整内部声明和链接可见性。已有函数长度限制保持成立。
- CMake/Make 的源码通配规则覆盖新文件；MSBuild 已登记全部新增实现与内部头文件。
- 旧 Make 测试改用 AI_WORKSPACE_OBJECTS 集合，保证工作区方法拆分后仍完整链接。工具批处理源码回归同步读取新的实现文件。
