# 工具执行与工作区实现拆分

## runtime/ai_agent_tools.cpp

原文件 2025 行，拆分为 9 个实现文件。

| 文件 | 职责 | 行数 |
|---|---|---:|
| `ai_agent_tools.cpp` | 工具入口、授权、工作视图与处理器分派 | 190 |
| `ai_agent_tools_protocol.cpp` | 协议适配、原生工具结果及提案错误 | 270 |
| `ai_agent_tools_context.cpp` | 读取覆盖、修复证据、上下文保留与路径映射 | 368 |
| `ai_agent_tools_batch.cpp` | 并行读取与私有修订批处理 | 231 |
| `ai_agent_tools_validation.cpp` | 验证工具执行 | 54 |
| `ai_agent_tools_read.cpp` | 目录列表、单文件和批量读取 | 203 |
| `ai_agent_tools_search.cpp` | 搜索、符号与代码大纲 | 209 |
| `ai_agent_tools_proposals.cpp` | 单条/批量提案、目录及文件创建 | 322 |
| `ai_agent_tools_edits.cpp` | 补丁集和精确文本替换 | 220 |

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

## 验证结果

静态库独立构建、全归档链接、webcool arm64/x86_64 严格构建、27 项库与模块测试、旧 Make 工作区测试、认证 HTTP 集成、真实编译失败后的模型修复和工具批处理隔离回归全部通过。make clean 演练覆盖全部 16 个实现文件的对象和依赖文件；MSBuild 文件清单校验通过，Windows/Linux 尚未实机编译。
