# Objective-C binding

推荐入口：`./build.sh`，也可用 `./build.sh /absolute/output/path` 指定输出目录。
脚本可从任意工作目录调用，默认输出到本目录 `build/`。`./build.sh --help` 查看参数。
额外参数透传给 CMake；支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量。安装产物在 `build/install/`。

Foundation + ARC 的 Objective-C/Objective-C++ 接口，公共头文件 `LAIAgent.h`。
支持 macOS 和 iOS，包含 Clang module `LibAI`；iOS 分发见 `../iOS`。
静态库将 C bridge 和 C++ runtime 一起打包。

```sh
cmake -S libai-objc -B build/libai-objc -DCMAKE_BUILD_TYPE=Release
cmake --build build/libai-objc --parallel
ctest --test-dir build/libai-objc --output-on-failure
```

```objc
#import <LibAI/LAIAgent.h>
NSError *error = nil;
LAIAgent *agent = [[LAIAgent alloc] initWithModel:
    ^LAIModelResponse *(NSString *requestJSON, LAIInvocation *invocation) {
        LAIModelResponse *response = [LAIModelResponse new];
        response.text = @"你好"; // 替换为应用自己的模型客户端
        return response;
    } instructions:@"你是编程助手" error:&error];
NSDictionary *result = [agent run:@"hello" options:nil hooks:nil error:&error];
NSLog(@"%@", result[@"text"]);
[agent close:&error];
```

引用为普通 include 目录时使用 `#import "LAIAgent.h"`；作为 module 可用 `@import LibAI`。
运行结果是 NSDictionary；NSError 表示桥接调用失败，模型失败用结果的 status/error 表达。
reset 清空会话；cancel 可跨线程；close/dealloc 释放原生资源。模型/工具异常会转成运行错误，
观察者异常触发取消。block 避免强引用捕获 agent 自身，以免构成 retain cycle。
链接静态库时启用 ARC，并链接 Foundation 与 libc++；纯 Objective-C 应用增加 `-lc++`。

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

`[LAICodingService available] / initWithRoot: / call:arguments:error:` 暴露同一套工作区、项目、provider、会话与运行审计功能。
桌面构建传入 `-DLIBAI_CODING_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`；
默认轻量及移动端构建的 available 返回 false，调用会报告未启用完整组件。

详见 [统一操作协议和调用示例](../libai/docs/CODING_SERVICE.md)。

新增会话快照、独立取消令牌及内置 Provider Agent。接口对应关系和生命周期见
[Agent 接口能力补齐](../libai/docs/CODING_SERVICE.md#agent-接口能力补齐)。
