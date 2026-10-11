# libai-c++：第三方 C++ APP 接口

本目录与 `libai` 同级，提供可安装的 C++11 库和拥有独立会话的 `libai::cpp::Agent`。
应用只需包含 `<libai_cpp/agent.h>`，链接 `libai::cpp`，无需 ACL、OpenSSL 或第三方 JSON 库。
公开头文件只使用封装自身定义的类型和标准库，不包含或导出原始 `libai` 类型。
`Message`、`ToolCall`、`ModelRequest`、`ModelResponse`、`Model`、`RunOptions` 等均独立定义，
`Agent`、`Cancellation` 的底层状态通过 Pimpl 隐藏。实现层转换请求、回调和结果，复用 agent 循环。
模型网络客户端和具体业务工具由应用提供。

## 构建及清理

```sh
./build.sh
./build.sh /absolute/output/path
./build.sh /absolute/shared/output -DBUILD_SHARED_LIBS=ON
# 不指定输出目录也可直接传参数：
./build.sh -DBUILD_SHARED_LIBS=ON
./clean.sh
./clean.sh /absolute/output/path
```

可以从任意工作目录调用脚本。默认安装产物位于本目录 `build/install/`，
默认只生成静态库：macOS/Linux 为 `libai_cpp.a`，Windows 为 `ai_cpp.lib`。
显式传入 `-DBUILD_SHARED_LIBS=ON` 才生成动态库：macOS `libai_cpp.dylib`、
Linux `libai_cpp.so`、Windows `ai_cpp.dll`（以及导入库）。每次构建只选择一种库类型。
直接使用 CMake 时同样默认静态库；`cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON`
启用动态库。`build.sh` 每次默认指定 OFF，再由额外参数覆盖，避免沿用旧的动态库缓存。
支持 `CMAKE`、`BUILD_TYPE`、`JOBS` 环境变量和额外 CMake 参数。
`clean.sh` 只删除对应 `cmake/`、`install/`，保留输出目录中的其他文件。
构建依赖 CMake 3.16+、C++11 编译器和线程运行库。可使用系统 CMake 命令在 Windows 构建。

## 第三方 APP 集成

在应用的 CMakeLists.txt 中：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_agent_app LANGUAGES CXX)
find_package(libai_cpp 1 CONFIG REQUIRED)
add_executable(my_agent_app main.cpp)
target_link_libraries(my_agent_app PRIVATE libai::cpp)
```

配置时传入 `-DCMAKE_PREFIX_PATH=/absolute/path/to/libai-c++/build/install`。
也可源码集成：`add_subdirectory(/path/to/coolder/libai-c++ libai-cpp-build)`，
然后链接相同目标。应用直接使用安装后的 include/lib/cmake 目录即可，不需要整个 coolder 仓库。
分发 C++ 二进制库时，应用和库需要使用兼容的编译器、C++ ABI、标准库及运行库设置。
动态库部署到应用的运行时搜索路径；Windows 同时分发 DLL，静态链接由 CMake 携带线程依赖。

```cpp
#include <libai_cpp/agent.h>
#include <iostream>

int main()
{
    using namespace libai::cpp;
    Agent app([](const ModelRequest &request, const StreamSink &sink) {
        ModelResponse response;
        // 在这里调用应用的模型客户端，转换回答及 ToolCall 到 response。
        response.text = "你好，" + request.messages.back().text;
        if (sink) sink(response.text);
        return response;
    }, "你是应用的 AI 助手");
    const auto result = app.run("介绍你的功能");
    if (result.status != RunStatus::completed) return 1;
    std::cout << result.text << '\n';
}
```

完整工具循环示例见 `examples/main.cpp`。安装后的 `share/libai_cpp/examples` 是独立消费项目：

```sh
cmake -S build/install/share/libai_cpp/examples -B build/app-example \
    -DCMAKE_PREFIX_PATH="$PWD/build/install"
cmake --build build/app-example
```

## 接口及运行语义

- `Agent(ModelFunction, instructions)` 直接接收应用回调，也可传 `std::shared_ptr<Model>` 子类。
- `add_tool(ToolSpec, ToolHandler)` 注册唯一能力。工具的 arguments/schema 为 JSON 文本，应用负责校验。
- `run(input, RunOptions)` 返回类型化状态、回答、错误和调用/token 统计；自动保留多轮会话。
- `RunOptions` 设置调用、token、上下文、工具结果预算、重试、截止时间、授权、流式输出及事件。
- `messages()` 复制空闲会话，`reset()` 清空会话，`cancel()` 请求当前运行协作停止。

需要审批的工具在没有 authorize 时默认拒绝。工具错误会作为消息交给模型，模型可以继续处理。
模型、工具及观察者异常由 SDK 转成相应运行状态；无效构造/配置或并行状态访问抛标准异常。
同步 run 应放在应用工作线程；回调也在该线程执行。同一实例不允许并行或回调重入
run/add_tool/reset/messages，跨线程 cancel 可用。共享同一个 Model 的多个 Agent 需要模型自行保证线程安全。
模型和长工具检查 cancellation/deadline，自行设置网络超时；取消不会强制终止应用回调。
若需要在 run 开始前也能可靠请求取消，在 RunOptions 中传入应用持有的 `shared_ptr<Cancellation>`。
Agent 不可复制/移动；RAII 析构前必须等待正在执行的 run/cancel 及相关应用线程结束。

## 验证

```sh
./build.sh build/test -DLIBAI_CPP_BUILD_TESTS=ON
ctest --test-dir build/test/cmake --output-on-failure
```

测试覆盖工具循环、Unicode、流式输出、会话/重置、默认权限拒绝、预算、异常、
并行访问、回调重入和跨线程取消。默认构建封装 portable SDK。启用完整组件后还包含编码基础设施。

## 完整编码组件

```sh
./build.sh build/full -DLIBAI_CPP_FULL=ON -DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl
```

`<libai_cpp/coding.h>` 是轻量入口，只引入 `CodingService` 和完整构建中的
`NativeProviderModel`。Provider 使用独立的 `ProviderConfig` 端点配置，模型继承
公开的 `Model` 接口；原生请求、存储记录、工作区、沙箱、运行时和 ACL 类型均不公开。
公共头文件只依赖标准库，不需要 ACL/OpenSSL 或原始 libai 的头文件。

```cpp
#include <libai_cpp/coding.h>
libai::cpp::ProviderConfig config;
config.protocol = "openai_chat";
config.base_url = "https://example.com/v1";
config.model = "your-model";
auto model = std::make_shared<libai::cpp::NativeProviderModel>(config, api_key);
libai::cpp::Agent agent(model);
```

所有构建均提供 `<libai_cpp/coding_service.h>` 的 `CodingService(root)`；
`available()` 检测完整组件，`call(operation, arguments_json)` 返回 UTF-8 JSON。
完整编码组件要求桌面 ACL/OpenSSL；默认轻量构建无这些依赖。Windows 完整类型化封装
使用 `-DBUILD_SHARED_LIBS=OFF`，VS 工程同样生成静态库。

```cpp
#include <libai_cpp/coding_service.h>
libai::cpp::CodingService service("/absolute/application-workspace");
if (service.available()) {
    auto result = service.call("workspace.read", R"({"path":"src/main.cpp"})");
}
```

安装后继续使用 `find_package(libai_cpp CONFIG REQUIRED)` 和 `libai::cpp`。
安装包只需要本绑定的头文件、库和 CMake 元数据，不要求原始 libai 头文件或其 CMake 包。
完整静态库的独立消费者设置 `ACL_PATH` / `OPENSSL_PATH` 或 `OPENSSL_ROOT_DIR`，
以定位对应架构的链接依赖；公共头文件无需这些库的头文件。
统一操作协议、写入授权与各语言示例见 [CODING_SERVICE.md](../libai/docs/CODING_SERVICE.md)。

## 公开接口与内部实现

`include/libai_cpp/` 根目录只保留 7 个公共头文件：`agent.h`、`types.h`、`export.h`、
`coding.h`、`coding_service.h`、`provider_config.h`、`provider_model.h`。
第三方编码能力通过 `CodingService::call()` 使用，操作协议包含工作区、项目、会话、
沙箱、Provider 和运行记录，无需包含内部头文件。

`src/` 仅保存封装实现、私有值转换及宿主兼容适配，不保存 libai 模块的头文件副本。
完整构建通过 `libai/cmake/CodingBackend.cmake` 直接编译 libai 的原始源码；
同一 CMake 构建中的 C/C++/JNI/Objective-C 绑定共用 `libai_coding_objects`。
库中嵌入这些对象，安装包不要求消费者安装原始 libai 头文件或 CMake 包。

coolder 是同仓库的服务端宿主，显式链接只在源码构建中提供的 `libai::cpp_host`，
使用 `src/coding_host.h` 与 `src/platform.h` 接入内部工作线程及 HTTP 状态展示。
这个目标不导出或安装；第三方 APP 只链接公开的 `libai::cpp`。
宿主兼容层仍用于 coolder 的内部工作线程与状态展示，不视为稳定的第三方 SDK。
ACL/OpenSSL 的编译定义和头文件路径只给实现与内部宿主使用；静态库的链接依赖
由 CMake 携带，动态库安装包不要求消费者配置 ACL/OpenSSL 的开发包。

`libai_cpp_internal_headers` 检查无后端副本及公开头文件隔离，`libai_cpp_types` 检查类型独立性，
`tests/consumer` 只用安装后的公共头文件验证 C++11 第三方集成。
VS 工程直接编译同一份 libai 原始源码，不生成改名的实现副本。
