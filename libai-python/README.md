# Python binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
先准备 Python 3.9+、pip、setuptools>=61 和 wheel（`python3 -m pip install -U setuptools wheel`）。
设置 `PYTHON` 选择解释器；脚本离线构建 C ABI 动态库、Python wheel 及可直接导入的包。
wheel 为纯 Python 包，安装后仍需设置 `LIBAI_LIBRARY` 指向 `build/native/install/lib/` 中的动态库。

Python 3.9+，使用标准库 ctypes，模型请求及运行结果为 Python 字典。
先构建上一级 `libai-c`；Python 包不会自动安装或下载原生库。

```sh
python3 -m pip install ./libai-python
export LIBAI_LIBRARY="$PWD/build/libai-c/libai_c.dylib" # Linux 改为 .so；Windows 设置 DLL 绝对路径
PYTHONPATH=libai-python python3 -m unittest discover -s libai-python/tests -v
```

```python
from libai import Agent, ModelResponse, ToolCall

def complete(request, invocation):
    # 此处调用应用自己的模型客户端，将模型输出转换成 ModelResponse。
    return ModelResponse(text="你好，" + request["messages"][-1]["text"])

with Agent(complete, instructions="你是编程助手") as agent:
    agent.add_tool("echo", lambda call, ctx: call.arguments["text"],
                   parameters={"type": "object", "properties": {"text": {"type": "string"}}})
    result = agent.run("介绍一下接口", max_model_calls=8)
    print(result["status"], result["text"])
```

也可向 `Agent(..., library="/absolute/path/to/library")` 指定共享库。
用上下文管理器或 `close()` 显式释放 native agent；reset 清空会话，cancel 可从另一线程调用。
模型/工具异常被转成原生错误；观察者异常取消运行并在 run 返回后抛出 RuntimeError。

## 共享语义

所有绑定都执行同一个 `libai/sdk` C++ agent 循环，支持多轮会话、工具调用、
流式文本、逐次权限检查、执行预算、重试、取消和截止时间。
应用提供 model 回调，在回调中使用自己的 HTTP/模型 SDK；这些绑定没有内置网络客户端，
也没有打包依赖 ACL/OpenSSL 的旧版 provider、工作区或进程工具。

`run` 同步执行，请放在工作线程。回调也在该线程执行，Invocation 只在回调期间且在
该线程有效。取消是协作式的：网络请求和长任务需要检查 `stopped()` 并设置自己的超时。
同一个 agent 只有一个会话；并行会话应使用不同实例。执行时不能修改工具、reset 或关闭实例。

模型请求 JSON 包含 `instructions`、`max_output_tokens`、`messages` 和 `tools`。
消息字段为 `role/text/call_id/reasoning/tool_error/calls`，角色为 `user/assistant/tool`；
`calls` 中的 `arguments` 和 `tools` 中的 `parameters` **是 JSON 字符串**。
结果包含 `status/text/error/model_calls/tool_calls/input_tokens/output_tokens`；
状态包括 `completed/cancelled/deadline_exceeded/budget_exceeded/model_error/protocol_error/callback_error`。
工具失败会作为工具消息返回给模型；它不一定意味着整个 run 失败。
需要审批的工具在缺少授权回调时默认拒绝。所有跨 ABI 字符串为无内嵌 NUL 的 UTF-8。

## 清理构建产物

运行 `./clean.sh` 清理默认构建输出；自定义输出目录使用
`./clean.sh /absolute/output/path` 或 `./clean.sh --output /absolute/output/path`。
只删除本绑定生成的目录和文件，保留输出目录中的其他文件；重复执行也可使用。

## 完整编码功能

`CodingService(root, library=...).available / .call(operation, arguments_dict)` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。
