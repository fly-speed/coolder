# 智能体结构化回复展示

界面统一使用 aiAgentDisplayText 对智能体回复进行只读投影，覆盖会话历史、流式更新、结果面板和任务总结。存储和协议解析仍保留原数据，不从展示内容执行工具或修改文件。

识别 final/tool_call 协议及带明确协议字段的结束报告，兼容完整 JSON、JSON 代码围栏、转义/有限层嵌套 JSON，以及正文中夹带的协议对象。展示 text、completion_summary、逐项需求的实现状态、依据和剩余工作；内部记忆、文件完整内容、工具参数等留在原始记录中。保留 JSON 前后的 WebCool 验证提示，不把模型“已实现”改写为“已验收”。

流式协议尚不完整时，尽量提取可解码的 text，剩余阶段显示整理提示；普通正文、非协议 JSON 示例以及用户消息保持原样。内容仍以 textContent 渲染，不将模型 HTML 作为界面代码执行。

测试：在 webcool 目录运行 node --test tests/agent_display_text_test.cjs。修改后运行 node html/build-js-bundle.js，并刷新页面。
