# libai 功能目录整理结果

已在 `/Users/zsx/work/github/chat-client/aicool/libai/` 完成目录整理。
根目录仅保留统一入口 `coding.h`、构建文件和说明；实现及细分头文件归入 11 个功能目录。
CMake、Make、Visual Studio、webcool 引用和测试路径均已同步更新。

验证：独立编译、全归档链接、SDK 安装与外部示例、19 项验证、macOS 双架构 webcool 构建、
HTTP 集成及模拟模型编译修复流程均通过。`make clean` 已验证覆盖全部功能目录。
Windows 完成了工程 XML 和引用路径检查，未实机编译。

下面是仓库内 README 的目录及使用说明。

---

# libai：独立 AI 编程静态库

`aicool/libai/` 与 `aicool/webcool/` 是同级模块。libai 可独立构建、安装并供多个可执行程序链接，
不需要编译 webcool。Unix/macOS 产物是 `libai.a`，Windows 是 `libai.lib`。
推荐 CMake 目标为 `libai::ai`，公开聚合头文件为 `<libai/coding.h>`。

## 目录结构

根目录仅保留聚合入口 `coding.h`、构建文件和说明。实现与细分头文件按功能放置：

| 目录 | 职责 |
| --- | --- |
| `agent/` | Agent 注册、协议、执行模式、调度与运行策略 |
| `provider/` | 模型客户端、请求/响应/流处理、限流及 Provider 配置 |
| `storage/` | 会话、运行记录、检查点、草稿、结果和工作流持久化 |
| `project/` | 项目元数据、索引、规划、脚手架、工具链与诊断 |
| `workspace/` | 文件读写、补丁、变更集、差异及审核状态 |
| `sandbox/` | 沙盒进程与执行边界 |
| `context/` | 上下文窗口、读取覆盖、分页及修复上下文 |
| `prompt/` | 提示词目录与语言选择 |
| `validation/` | 提案验证、编译测试证据、修复收敛与 CTest 模板 |
| `common/` | 通用头文件、日志、互斥锁、平台兼容与 UTF-8 工具 |
| `runtime/` | 编程循环、上下文组装、后台 worker 与任务运行时 |
| `cmake/`、`tests/`、`examples/` | 构建包配置、独立链接测试和可执行程序示例 |

`#include <libai/coding.h>` 保持不变。直接引用细分接口时使用功能目录，
例如 `#include <libai/provider/ai_provider_client.h>`；类型和命名空间保持原有定义。

## 模块边界

本目录包含模型协议与传输、Provider 配置及密钥存储、Agent 协议、项目/会话/运行记录、
工作区修改与审核、工具链、沙盒执行，以及 `runtime/` 中的编程循环、调度、暂停、取消、
恢复和后台 worker。图片助手共用运行时，也归库所有。

HTTP 路由、用户鉴权、项目目录授权和前端保留在 `webcool/action/ai/` 等宿主代码中。
宿主鉴权后显式传入存储根目录、用户目录、模型配置和沙盒策略。库不依赖 webcool 的 HTTP 实现。
已有 C++ 命名空间 `webcool::ai`、`webcool::ai::coding` 保留，以避免改变现有接口和行为；
后者是原运行时命名空间 `action::agent_detail` 的别名。

ACL/fiber、OpenSSL 是外部依赖，不合并进归档。宿主负责 ACL/fiber 初始化和线程生命周期。
沙盒执行仍需部署 `webcool-sandbox-helper`，并按 `sandbox/program_sandbox.h`、`agent/ai_admin_policy.h` 配置。

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
`make -C libai clean` 同时清理全部功能目录下的 `.o`、`.obj`、`.d`、
根目录静态库，以及已配置的 CMake 构建产物；保留 CMake 配置缓存以便下次构建。
尚未配置 CMake 或重复执行 clean 都可正常完成。
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
