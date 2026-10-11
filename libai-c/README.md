# libai C ABI

该目录为 Python、Java、Rust、Objective-C 绑定提供统一的原生桥接层。
公共头文件为 `include/libai.h`，当前 `libai_abi_version()` 为 1。

```sh
cmake -S libai-c -B build/libai-c -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-c --parallel
ctest --test-dir build/libai-c --output-on-failure
cmake --install build/libai-c --prefix "$PWD/build/install"
```

共享库为 Linux `libai_c.so`、macOS `libai_c.dylib`、Windows `ai_c.dll`。
`-DBUILD_SHARED_LIBS=OFF` 生成静态库。只依赖 C++11 标准库、线程支持和 CMake。
C 程序链接共享库即可；静态链接还需相应 C++ 运行库。
安装后可使用 `find_package(libai_c CONFIG REQUIRED)` 和 `libai::c`。

完整工具循环见 `tests/bridge_test.c`。创建 agent 时传入模型函数及 user 数据，
`libai_agent_add_tool` 注册能力，`libai_agent_run` 返回 JSON，最后 `libai_agent_destroy`。
模型用 `libai_response_*` 填充输出；工具用 `libai_tool_result_set` 填充结果。
构建器复制输入字符串，不接管回调的内存。

所有 opaque builder/invocation 指针仅在当前回调有效。应用必须保持 callback/user 数据
存活至 agent 销毁，且不能让异常跨越 C ABI。run 返回的字符串必须用 `libai_string_free`
释放。错误返回 NULL/-1，调用 `libai_last_error()` 读取当前线程的诊断。
销毁必须由调用方与所有其他 API 调用（包括 cancel）串行化；不能在活跃 run 中销毁。
模型/工具回调中的分配失败会转成运行错误，不允许 C++ 异常穿过外语回调。

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
