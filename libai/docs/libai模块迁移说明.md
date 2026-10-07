# libai 模块迁移完成

源码位置：`/Users/zsx/work/github/chat-client/aicool/libai/`

已生成静态库：`/Users/zsx/work/github/chat-client/aicool/libai/libai.a`

原 `webcool/ai/` 已移除，库源码、头文件、运行时、独立构建配置和测试已全部迁入同级 `libai/`。
webcool 的 Make、CMake 和 Visual Studio 工程均已改为引用同级库。

## 验证结果

- libai 独立 CMake 编译、安装、全归档链接检查：通过。
- 安装后的独立示例程序：通过，仅链接 `libai::ai`，输出 `Agent: coding` 和 `Tools: 19`。
- 外部消费项目及核心回归共 19 项：全部通过。
- libai.a 与 webcool 均构建为 arm64/x86_64 通用产物。
- 迁移后的鉴权 HTTP 集成、工具批处理、模拟模型编译修复流程：全部通过。
- 完成摘要、前端 reasoning 参数、修复上下文回放及 CTest 模板回归：通过。
- CMake 宿主集成配置、Windows 工程 XML 与路径完整性、git diff --check：通过。
- 本机未实际编译运行 Windows/Linux 版本。

## 构建和接入说明

# libai：独立 AI 编程静态库

`aicool/libai/` 与 `aicool/webcool/` 是同级模块。libai 可独立构建、安装并供多个可执行程序链接，
不需要编译 webcool。Unix/macOS 产物是 `libai.a`，Windows 是 `libai.lib`。
推荐 CMake 目标为 `libai::ai`，公开聚合头文件为 `<libai/coding.h>`。

## 模块边界

本目录包含模型协议与传输、Provider 配置及密钥存储、Agent 协议、项目/会话/运行记录、
工作区修改与审核、工具链、沙盒执行，以及 `runtime/` 中的编程循环、调度、暂停、取消、
恢复和后台 worker。图片助手共用运行时，也归库所有。

HTTP 路由、用户鉴权、项目目录授权和前端保留在 `webcool/action/ai/` 等宿主代码中。
宿主鉴权后显式传入存储根目录、用户目录、模型配置和沙盒策略。库不依赖 webcool 的 HTTP 实现。
已有 C++ 命名空间 `webcool::ai`、`webcool::ai::coding` 保留，以避免改变现有接口和行为；
后者是原运行时命名空间 `action::agent_detail` 的别名。

ACL/fiber、OpenSSL 是外部依赖，不合并进归档。宿主负责 ACL/fiber 初始化和线程生命周期。
沙盒执行仍需部署 `webcool-sandbox-helper`，并按 `program_sandbox.h`、`ai_admin_policy.h` 配置。

## 独立构建

在 `aicool/` 下执行（默认复用同级 `third-party/` 中的依赖）：

```sh
cmake -S libai -B libai/build -DCMAKE_BUILD_TYPE=Release \
  -DAI_CODING_BUILD_TESTS=ON
cmake --build libai/build --parallel
ctest --test-dir libai/build --output-on-failure
cmake --install libai/build --prefix /absolute/path/to/libai-sdk
```

也可使用 `make -C libai`、`make -C libai install PREFIX=/absolute/path/to/libai-sdk`。
依赖位于其他目录时指定 `ACL_PATH`、`OPENSSL_PATH`；Windows 预编译 ACL 产物目录可用
`ACL_LIBRARY_DIRS` 指定。macOS 双架构构建添加
`-DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0`，
依赖必须提供对应架构和部署版本。

## 可执行程序集成

安装后的库支持：

```cmake
find_package(libai CONFIG REQUIRED)
add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE libai::ai)
```

```cpp
#include <libai/coding.h>
// 使用 webcool::ai 中的模型、工作区、存储等接口。
```

配置消费项目时将 `CMAKE_PREFIX_PATH` 指向 SDK 安装目录，并指定其依赖的 `ACL_PATH`、
`OPENSSL_PATH`（必要时 `ACL_LIBRARY_DIRS`）。头文件和链接依赖通过 CMake 目标传递。

`examples/minimal/` 是可独立构建的消费程序，不包含 webcool 头文件，也不编译库源码：

```sh
cmake -S libai/examples/minimal -B example-build \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/libai-sdk \
  -DACL_PATH=/absolute/path/to/acl \
  -DOPENSSL_PATH=/absolute/path/to/openssl
cmake --build example-build
./example-build/libai_example
```

在同一源码工程中也可使用 `add_subdirectory(/path/to/libai libai-build)` 后链接 `libai::ai`。
库的全归档链接测试会强制链接每个实现对象，检查是否意外依赖 webcool 宿主符号。

## webcool 接入

- CMake 引入同级 `../libai` 并链接 `libai::ai`。
- `make -C webcool ai-coding-lib` 生成 `libai/libai.a`；正常构建 webcool 自动链接该归档。
- `webcool.sln` 通过项目引用链接 `../libai/libai.vcxproj`，不重复编译库源码。
- webcool 及测试使用 `libai/...` 头文件引用，原 `webcool/ai/` 已移除。

持久化格式、HTTP API 和权限逻辑沿用原有行为。公共平台兼容及互斥锁头文件归 libai 所有，
webcool 原有头文件转发到库版本。
