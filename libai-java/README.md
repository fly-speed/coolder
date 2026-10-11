# Java / JNI binding

Java 8+；包名 `io.coolder.libai`，类 `Agent`。桌面 JNI 需要完整 JDK 的 JNI 头文件。
Java 回调接收 JSON 文本，可选择应用已有 JSON 库。Android 打包入口见 `../Android`。

```sh
export JAVA_HOME=/path/to/full/jdk
cmake -S libai-java -B build/libai-java -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-java --parallel
javac -encoding UTF-8 -d build/java-classes libai-java/src/main/java/io/coolder/libai/Agent.java libai-java/tests/AgentTest.java
java -Djava.library.path=build/libai-java -cp build/java-classes AgentTest
jar cf build/libai-java.jar -C build/java-classes io
```

原生库 `libai_jni.so` / `libai_jni.dylib` / `ai_jni.dll` 必须与 JVM 架构一致。
CMake 可用 `-DLIBAI_JNI_INCLUDE_DIR=/path/to/jdk/include` 手动指定头文件；
macOS 可用 `-DCMAKE_OSX_ARCHITECTURES=x86_64` 对应 Intel JVM。

```java
import io.coolder.libai.Agent;

try (Agent agent = new Agent((requestJson, invocation) -> {
    return new Agent.Response("模型回答"); // 替换为应用的模型客户端
}, "你是编程助手")) {
    agent.addTool("echo", "Echo input", "{\"type\":\"object\"}", false,
        (call, invocation) -> new Agent.ToolResult(call.arguments));
    System.out.println(agent.run("hello"));
}
```

必须使用 try-with-resources 或显式 close：JNI 为活跃回调持有 Java owner 的 global reference。
执行时 close 会报错，cancel 可从另一线程调用。模型/工具 Java 异常成为模型/工具错误；
观察者异常触发取消。参数/schema 为 JSON 文本。反射调用字段必须保留；Android AAR 已带 R8 规则。

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
