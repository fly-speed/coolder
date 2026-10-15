# 最终 JSON 输出校验

通用 SDK `libai::agent_t` 与独立 C++ facade `libai::cpp::Agent` 提供：

```cpp
agent.configure_output(
    R"({"type":"object","required":["answer"],"properties":{"answer":{"type":"string"}},"additionalProperties":false})",
    1); // 最多一次校验反馈；0 表示失败后不请求修正，硬上限8。
```

这是宿主校验模式：Schema 被加入模型指令，SDK 检查最终回答的原始 JSON。
它不承诺模型厂商原生 response_format 支持。工具调用阶段仍可正常执行；最终
回答不是合法 JSON 或违反 Schema 时，保存反馈并继续模型循环。修正阶段提供空
工具目录，并在执行器拒绝任何返回的工具调用，不重新规划或重跑此前的业务操作。

Schema 沿用 [工具治理](GOVERNANCE.md) 的受限 Draft-04 配置，不加载远程引用。
不抽取 Markdown 代码块，不接受多个 JSON 值、重复键或超限 JSON。空 Schema
关闭校验；配置错误不替换原配置。SDK 用户也可通过 run_options_t.output_schema /
max_output_repairs 配置；显式 Agent 配置优先。独立 C++ 的公开头文件不引用 SDK。

反馈次数并非额外请求额度：实际请求、重试、tokens、上下文、期限和取消继续受
原运行预算约束。非重试模型错误后恢复可以重试传输，但不会重置反馈或总调用数。
反馈额度耗尽或修正阶段尝试调用工具会返回 protocol_error，并在日志中保存失败；
同 scope 再次运行不联系模型。修正不会重试业务工具。

启用此模式时，流式文本只在内部参与字节限制和取消检查，不提前传给 on_text。
最终 JSON 校验通过后，on_text 一次收到完整文本。实时进度可使用 on_event。
SDK run_result_t.output_validated / output_repairs 表示校验事实与已发出的反馈数量；
结构符合 Schema 不证明事实、编译、测试或人工验收通过，不改变 TaskContract 证据。

持久日志冻结原始 Schema 字符串和反馈额度，保存反馈次数、对话和终态失败。
恢复时配置必须相同（包括 Schema 文本）；变更返回 recovery_required。完成结果
复用前再次校验 Schema。副作用收据确认保持原输出契约，不清空反馈或允许业务重跑。
旧日志缺少新字段时视为未配置输出契约，不能直接升级成已验证的结构化结果。

## 各语言接口

| 语言 | 配置接口 | 校验元数据 |
| --- | --- | --- |
| SDK | configure_output(schema, repairs) | run_result_t.output_validated / output_repairs |
| C++ facade | configure_output(schema, repairs) | 空闲时 output_validation() 读取上一次返回结果 |
| C | libai_agent_configure_output(agent, schema_json, repairs) | run JSON 字段 output_validated / output_repairs |
| Python | configure_output(schema_dict_or_json, repairs=1) | run 字典中的同名字段；None 禁用 |
| Rust | configure_output(schema_json, repairs) | run JSON 中的同名字段 |
| Java | configureOutput(schemaJson, repairs) | run JSON 中的同名字段 |
| Objective-C | configureOutput:repairs:error: 或 configureOutputJSON:repairs:error: | run 字典中的同名字段；nil 禁用 |

配置仅允许空闲 Agent，非法 Schema/额度保留原配置。各接口共用宿主校验、反馈、
工具阻断、流式发布及执行日志规则。C ABI 与既有选项布局不变；C++ 使用独立的
OutputValidation 查询类型，保留已有 RunResult 和 ModelRequest 值布局。

C 及其绑定的模型回调，在启用模式时收到 output_schema（JSON 文本字符串）与
output_repair（布尔值）；未启用时不增加这两个请求字段。C++ 自定义模型可从
instructions 获取规则，修正轮 tools 为空。结果新增元数据是独立事实，不改变
TaskContract。未启用时 output_validated=false、output_repairs=0。

Objective-C 字典入口使用 Foundation 排序键编码，要求 macOS10.13/iOS11 或更新
版本；原始 JSON 文本入口支持使用其它绑定保存的原始 Schema，避免重新编码
改变已冻结规则。数组顺序与 Schema 文本变化仍受原冻结契约约束。

Provider 原生结构化输出及 APP 管理界面仍需继续接入。MCP 六语言接口及 POSIX stdio 传输见 [MCP.md](MCP.md)，内置 Streamable HTTP 传输仍待补齐。进程存储与断电边界继续适用执行存储契约，测试不等于物理断电验证。
