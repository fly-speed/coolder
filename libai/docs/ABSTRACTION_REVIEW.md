# libai 源码分析与公共能力抽象

本次分析基线为 111 个 C++ 源码/头文件，共 36,029 行（包含示例和链接测试，不含构建产物）。按职责、局部依赖和重复函数审查；此次实施聚焦可以保持行为一致的共用机制。

## 已实施的抽象

| 组件 | 统一能力 | 仍由调用方负责 |
|---|---|---|
| common/record_codec.h | 十六进制编码、解码、TAB 字段切分、非负数解析 | 记录版本、字段约束、有符号数规则 |
| common/file_ops.h | 路径拼接、目录检查、私有目录创建、路径项检查、文件替换 | 路径授权、事务、锁、权限修正、临时文件清理 |
| common/identifiers.h | 32 位十六进制 ID 校验、128 bit 随机 ID | 可空规则、只允许小写的业务约束 |
| common/json_value.h | 文本、数字、布尔、对象和数组读取 | 严格字符串、可空文本、宽松布尔的具体选择 |
| workspace/diff_preview.h | 分行、差异操作、增删统计、上下文可见范围、行格式 | 文件标签、多文件操作、字节/行数截断 |

130 个局部辅助函数定义已改为公共实现引用或兼容转发，涉及 20 个翻译单元；另合并了两个差异预览构造流程中的重复机制。五个小型组件采用内联头文件，使现有静态库和单对象回归测试均可复用；不引入新运行时依赖。Provider 和 runtime 已对外可见的函数保留转发入口。

## 模块边界与保留差异

- agent 保留协议、代理注册、执行策略、运行槽位及进度监督；Provider 请求限流和代理运行调度管理不同资源，保持独立。
- storage、project、workspace 的持久化复用编码与文件机制。会话 SQLite、文本记录、检查点和变更事务的生命周期及失败恢复不同，不合并为统一 Store 基类。
- provider 保留请求构造、响应解析、流解析和 HTTP 传输层次；runtime 通过既有模型接口组合它们。
- runtime 继续负责上下文、循环、工具、草稿及后台工作；context 负责证据和预算，validation 负责业务判断，prompt 负责提示词策略。
- sandbox 保留平台隔离与执行封装；workspace 继续拥有路径安全、审查及变更提交规则。词法路径拼接不承担工作区越界校验。
- 严格 JSON 字符串读取不把 null 或数值变成模型文本；宽松布尔读取仍接受字符串 true/1、false/0；数值读取保留默认值规则。
- 工作流 ID 的仅小写约束、进度/结果记录的可空 ID、两个解码器对奇数长度输入保留原输出的行为均保留。
- Provider storage_paths 在空根目录时具有不同语义，未替换为通用 join_path。
- 文件替换保留原平台实现；POSIX rename 不等于持久化落盘，不改变原先各存储模块的事务和清理顺序。
- 单文件补丁按增删行数截断，多文件预览按字节上限截断，两个策略保留在各自调用方。

## 逐文件审查清单

同名头文件负责声明/内联能力，cpp 负责实现。下表列出基线的全部 111 个文件；未修改项保留现有职责，不表示已消除所有潜在重构空间。

| 文件 | 职责 | 本次处理 |
|---|---|---|
| `agent/agent_execution_mode.cpp` | 执行模式和预算配置 | 保留模块边界 |
| `agent/agent_execution_mode.h` | 执行模式和预算配置 | 保留模块边界 |
| `agent/agent_progress_supervisor.cpp` | 运行进度与停滞判定 | 保留模块边界 |
| `agent/agent_progress_supervisor.h` | 运行进度与停滞判定 | 保留模块边界 |
| `agent/agent_protocol.cpp` | 代理工具消息解析 | 复用 json_value.h |
| `agent/agent_protocol.h` | 代理工具消息解析 | 保留模块边界 |
| `agent/agent_registry.cpp` | 代理与工具定义注册 | 保留模块边界 |
| `agent/agent_registry.h` | 代理与工具定义注册 | 保留模块边界 |
| `agent/agent_run_scheduler.cpp` | 按用户和工程分配运行槽位 | 保留模块边界 |
| `agent/agent_run_scheduler.h` | 按用户和工程分配运行槽位 | 保留模块边界 |
| `agent/ai_admin_policy.cpp` | 管理策略及策略持久化 | 保留模块边界 |
| `agent/ai_admin_policy.h` | 管理策略及策略持久化 | 保留模块边界 |
| `coding.h` | 库的聚合入口 | 保留模块边界 |
| `common/ai_error_log.h` | 错误日志适配 | 保留模块边界 |
| `common/platform_compat.h` | 跨平台文件和路径兼容 | 保留模块边界 |
| `common/stdafx.h` | 公共依赖头 | 保留模块边界 |
| `common/utf8_text.h` | UTF-8 文本处理 | 保留模块边界 |
| `common/webcool_mutex.h` | 线程与协程锁适配 | 保留模块边界 |
| `context/agent_context_window.h` | 上下文窗口预算 | 保留模块边界 |
| `context/agent_read_coverage.h` | 文件读取覆盖记录 | 保留模块边界 |
| `context/agent_staged_context.h` | 分阶段上下文组织 | 保留模块边界 |
| `context/repair_context_evidence.h` | 修复上下文证据 | 保留模块边界 |
| `context/repair_edit_evidence.h` | 编辑证据 | 保留模块边界 |
| `context/repair_review_context.h` | 修复审查上下文 | 保留模块边界 |
| `context/text_read_page.h` | 文本分页读取 | 保留模块边界 |
| `examples/minimal/main.cpp` | 独立集成示例 | 保留模块边界 |
| `project/agent_project_index.cpp` | 工程索引持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `project/agent_project_index.h` | 工程索引持久化 | 保留模块边界 |
| `project/agent_project_store.cpp` | 工程记录持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `project/agent_project_store.h` | 工程记录持久化 | 保留模块边界 |
| `project/preview_project_context.cpp` | 预览工程上下文 | 保留模块边界 |
| `project/preview_project_context.h` | 预览工程上下文 | 保留模块边界 |
| `project/project_diagnostics.cpp` | 工程诊断 | 保留模块边界 |
| `project/project_diagnostics.h` | 工程诊断 | 保留模块边界 |
| `project/project_plan_template.cpp` | 工程计划模板 | 保留模块边界 |
| `project/project_plan_template.h` | 工程计划模板 | 保留模块边界 |
| `project/project_scaffold.cpp` | 工程脚手架 | 保留模块边界 |
| `project/project_scaffold.h` | 工程脚手架 | 保留模块边界 |
| `project/project_toolchain.cpp` | 工具链识别与执行配置 | 保留模块边界 |
| `project/project_toolchain.h` | 工具链识别与执行配置 | 保留模块边界 |
| `project/supported_languages.h` | 支持语言定义 | 保留模块边界 |
| `prompt/prompt_language.h` | 提示词语言策略 | 保留模块边界 |
| `prompt/prompt_templates.h` | 提示词模板 | 保留模块边界 |
| `provider/ai_provider_client.cpp` | 模型调用门面 | 保留模块边界 |
| `provider/ai_provider_client.h` | 模型调用门面 | 保留模块边界 |
| `provider/ai_provider_client_internal.h` | Provider 内部接口 | 保留模块边界 |
| `provider/ai_provider_common.cpp` | Provider 公共协议工具 | 复用 json_value.h |
| `provider/ai_provider_diagnostics.cpp` | Provider 诊断 | 保留模块边界 |
| `provider/ai_provider_request.cpp` | 模型请求构造 | 保留模块边界 |
| `provider/ai_provider_response.cpp` | 模型响应解析 | 保留模块边界 |
| `provider/ai_provider_store.cpp` | Provider 配置持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `provider/ai_provider_store.h` | Provider 配置持久化 | 保留模块边界 |
| `provider/ai_provider_stream.cpp` | 流式响应解析 | 保留模块边界 |
| `provider/ai_provider_transport.cpp` | HTTP 传输 | 保留模块边界 |
| `provider/output_token_limit.h` | 输出 token 限制 | 保留模块边界 |
| `provider/provider_output_limit_cache.h` | 输出上限缓存 | 保留模块边界 |
| `provider/provider_request_scheduler.cpp` | Provider 请求并发调度 | 保留模块边界 |
| `provider/provider_request_scheduler.h` | Provider 请求并发调度 | 保留模块边界 |
| `provider/provider_stream_progress.h` | 流式进展统计 | 保留模块边界 |
| `provider/storage_paths.h` | Provider 存储路径规则 | 保留模块边界 |
| `runtime/ai_agent_context.cpp` | 运行上下文组装 | 复用 identifiers.h |
| `runtime/ai_agent_drafts.cpp` | 运行草稿处理 | 保留模块边界 |
| `runtime/ai_agent_loop.cpp` | 代理执行循环 | 保留模块边界 |
| `runtime/ai_agent_runtime.cpp` | 运行状态与入口实现 | 保留模块边界 |
| `runtime/ai_agent_tools.cpp` | 工具分派 | 保留模块边界 |
| `runtime/ai_agent_worker.cpp` | 后台执行与恢复 | 保留模块边界 |
| `runtime/assistant_images.cpp` | 助手图像输入 | 保留模块边界 |
| `runtime/coding_runtime.h` | 宿主集成接口与运行时声明 | 保留模块边界 |
| `runtime/coding_tool_loop.cpp` | 工具调用循环 | 保留模块边界 |
| `runtime/coding_tool_loop.h` | 工具调用循环 | 保留模块边界 |
| `runtime/runtime_common.cpp` | 运行时公共工具 | 复用 json_value.h |
| `sandbox/program_sandbox.cpp` | 程序运行与平台隔离 | 保留模块边界 |
| `sandbox/program_sandbox.h` | 程序运行与平台隔离 | 保留模块边界 |
| `sandbox/sandbox_execution.cpp` | 沙箱执行封装 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `sandbox/sandbox_execution.h` | 沙箱执行封装 | 保留模块边界 |
| `storage/agent_checkpoint_store.cpp` | 检查点持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `storage/agent_checkpoint_store.h` | 检查点持久化 | 保留模块边界 |
| `storage/agent_draft_store.cpp` | 草稿持久化 | 复用 file_ops.h, identifiers.h |
| `storage/agent_draft_store.h` | 草稿持久化 | 保留模块边界 |
| `storage/agent_progress_store.cpp` | 进度持久化 | 复用 file_ops.h, identifiers.h, json_value.h |
| `storage/agent_progress_store.h` | 进度持久化 | 保留模块边界 |
| `storage/agent_request_store.cpp` | 请求持久化 | 复用 file_ops.h, identifiers.h |
| `storage/agent_request_store.h` | 请求持久化 | 保留模块边界 |
| `storage/agent_result_store.cpp` | 结果持久化 | 复用 file_ops.h, identifiers.h, json_value.h |
| `storage/agent_result_store.h` | 结果持久化 | 保留模块边界 |
| `storage/agent_run_store.cpp` | 运行记录持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `storage/agent_run_store.h` | 运行记录持久化 | 保留模块边界 |
| `storage/agent_session_store.cpp` | 代理会话持久化 | 复用 file_ops.h, identifiers.h, record_codec.h |
| `storage/agent_session_store.h` | 代理会话持久化 | 保留模块边界 |
| `storage/agent_workflow_store.cpp` | 工作流持久化 | 复用 file_ops.h, record_codec.h |
| `storage/agent_workflow_store.h` | 工作流持久化 | 保留模块边界 |
| `storage/assistant_session_store.cpp` | 助手会话 SQLite 存储与导入 | 复用 file_ops.h, json_value.h |
| `storage/assistant_session_store.h` | 助手会话 SQLite 存储与导入 | 保留模块边界 |
| `tests/coding_link_test.cpp` | 完整静态库链接验证 | 保留模块边界 |
| `validation/ctest_io_template.h` | C 测试输入输出模板 | 保留模块边界 |
| `validation/draft_cycle_tracker.h` | 草稿循环检测 | 保留模块边界 |
| `validation/placeholder_proposal.h` | 占位提案识别 | 保留模块边界 |
| `validation/proposal_validation.h` | 提案有效性判断 | 保留模块边界 |
| `validation/repair_failure_tracker.h` | 修复失败收敛跟踪 | 保留模块边界 |
| `validation/validation_test_evidence.h` | 验证测试证据 | 保留模块边界 |
| `validation/verification_task.h` | 验证任务构造 | 保留模块边界 |
| `workspace/agent_change_limits.h` | 变更规模限制 | 保留模块边界 |
| `workspace/agent_review_state.cpp` | 审查状态持久化 | 保留模块边界 |
| `workspace/agent_review_state.h` | 审查状态持久化 | 保留模块边界 |
| `workspace/agent_workspace.cpp` | 工作区访问和安全边界 | 复用 file_ops.h, record_codec.h |
| `workspace/agent_workspace.h` | 工作区访问和安全边界 | 保留模块边界 |
| `workspace/line_diff.h` | 行级差异算法 | 保留模块边界 |
| `workspace/workspace_change_set.cpp` | 多文件变更事务与预览 | 复用 file_ops.h, identifiers.h, record_codec.h；复用 diff_preview.h |
| `workspace/workspace_change_set.h` | 多文件变更事务与预览 | 保留模块边界 |
| `workspace/workspace_patch.cpp` | 单文件补丁和预览 | 复用 file_ops.h, identifiers.h, record_codec.h；复用 diff_preview.h |
| `workspace/workspace_patch.h` | 单文件补丁和预览 | 保留模块边界 |

## 后续独立 Web AI Coding 的边界

当前复用单位为 libai 静态库，宿主仍须提供 HTTP 路由、鉴权、页面、配置装配和运行生命周期。独立产品可新建宿主连接 coding_runtime 接口，复用本库；前端资源与 webcool 的账户/工程适配仍需单独迁出。此次重构不把 HTTP 或 UI 合入算法和存储公共组件。

## 验证

- libai 独立 CMake 构建通过；公共组件语义测试及完整归档链接测试 2/2 通过。
- 安装后的 `find_package(libai CONFIG REQUIRED)` 消费者与模块测试 24/24 通过（包含 Provider、工作区、审查、运行/请求/结果/会话/工作流/检查点/进度/草稿/工程存储）。助手会话测试依赖 SQLite 相对路径，设置为 webcool 工作目录后通过。
- 独立 minimal 示例构建并运行成功，输出 coding 代理及 19 个工具。
- webcool 严格 Make 构建通过；库与可执行文件均包含 x86_64、arm64。
- 认证 AI HTTP 集成、真实编译失败后的模型修复、工具批处理隔离回归均通过。
- 公共测试覆盖二进制编码、错误输入、数值溢出、JSON 类型差异、随机 ID、差异行格式、目录权限、软链接拒绝及文件替换失败保留原目标。
- 原有源码加新增五个公共头文件净减少 712 行；新增测试及文档不计入该数值。
- Windows、Linux 未在本机实际编译。平台分支保持原有实现，仍需对应平台 CI 验证。
