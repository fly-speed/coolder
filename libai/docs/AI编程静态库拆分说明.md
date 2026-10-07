# webcool AI 编程静态库拆分结果

已在 `/Users/zsx/work/github/chat-client/aicool/webcool` 完成修改。

## 交付

- 独立模块：`ai/`，包含 46 个实现文件。
- 已生成 macOS 通用静态库：`ai/libwebcool_ai_coding.a`，架构为 arm64、x86_64。
- API 入口：`ai/coding.h`；CMake 目标：`webcool::ai_coding`。
- 构建支持：独立 CMake / Make、安装与 find_package 导出、Visual Studio 静态库工程。
- webcool 的 Make、CMake、Visual Studio 工程均改为链接静态库，避免再次直接编译库内实现。
- HTTP、鉴权与访问授权保留在宿主；模型、存储、工作区、沙盒、工具循环和后台运行时由库拥有。

## 验证

| 检查 | 结果 |
| --- | --- |
| 静态库独立 CMake 编译、安装 | 通过 |
| 强制链接全部归档对象，排查宿主反向依赖 | 通过 |
| 安装后外部 CMake 消费项目及 18 项核心回归（共 19 项） | 全部通过 |
| webcool Make 完整构建 | 通过，arm64 + x86_64 |
| webcool CMake 集成配置与生成 | 通过 |
| 已有鉴权 HTTP 集成测试 | 通过 |
| 本地模拟模型：真实编译失败、模型修复、编译和测试成功 | 通过 |
| 工具批处理隔离与响应映射 | 通过 |
| 完成摘要及前端 reasoning 参数回归 | 通过 |
| Windows 工程 XML、源文件完整性与重复编译检查 | 通过 |
| git diff --check | 通过 |

Windows/Linux 未在本机实际编译运行。静态库依赖 ACL/fiber、OpenSSL；沙盒执行仍需按原有方式部署 helper。

HTTP 回归中同步修复了两个过时断言：固定 5 个脚手架文件改为逐项比对当前真实文件；会话导出标题改为匹配现有首条请求命名规则。没有为通过测试修改业务行为。

下面是仓库内 `ai/README.md` 的构建与接入说明。

---

# AI 编程静态库

`ai/` 是可单独复制、构建和安装的 C++ 静态库目录。产物为
`libwebcool_ai_coding.a`（Unix/macOS）或 `webcool_ai_coding.lib`（Windows），
CMake 目标为 `webcool::ai_coding`。库仍使用 ACL/fiber 和 OpenSSL；这些依赖不合并进归档。

## 边界与接口

- 本目录拥有模型协议与传输、Provider 配置与密钥存储、Agent 注册与协议、项目/会话/运行记录、工作区修改与审核、工具链及沙盒执行。
- `runtime/` 拥有上下文组装、编程工具循环、后台 worker、调度/取消/暂停/恢复及结果持久化。图片助手共用这一运行时，也随库构建。
- webcool 的 `action/ai/` 只保留 HTTP 请求处理、用户鉴权、路径访问授权及响应适配；前端资源和路由不变。
- `coding.h` 是聚合入口。底层类型位于 `webcool::ai`，运行时通过 `webcool::ai::coding` 访问；为兼容原调用代码，运行时原命名空间 `action::agent_detail` 暂时保留。
- 宿主在鉴权后显式传入 upload root、user root、provider、sandbox policy 等已有参数。库不读取 webcool HTTP 请求或宿主的全局上传目录。
- 使用现有 fiber 运行模型；宿主负责 ACL/fiber 初始化和工作线程生命周期。进程沙盒仍需要部署独立的 `webcool-sandbox-helper`，路径/策略按 `program_sandbox.h`、`ai_admin_policy.h` 配置。

## 独立构建、测试与安装

```sh
cmake -S ai -B build-ai -DCMAKE_BUILD_TYPE=Release \
  -DACL_PATH=/absolute/path/to/acl \
  -DOPENSSL_PATH=/absolute/path/to/openssl \
  -DAI_CODING_BUILD_TESTS=ON
cmake --build build-ai --parallel
ctest --test-dir build-ai --output-on-failure
cmake --install build-ai --prefix /absolute/path/to/ai-sdk
```

ACL 默认使用各组件 `lib/` 中的预编译库；Windows 可用
`-DACL_LIBRARY_DIRS=/absolute/path/to/x64/Release` 指定已有产物。
OpenSSL 未在指定目录提供 Unix 静态库时通过 `find_package(OpenSSL)` 查找。
macOS 通用库可额外传入 `-DCMAKE_OSX_ARCHITECTURES='arm64;x86_64'`；
依赖必须包含相同架构，部署版本也必须一致。

外部项目：

```cmake
find_package(webcool_ai_coding CONFIG REQUIRED)
add_executable(my_agent main.cpp)
target_link_libraries(my_agent PRIVATE webcool::ai_coding)
```

配置外部项目时设置 `CMAKE_PREFIX_PATH` 指向 SDK 安装目录，并提供 `ACL_PATH`、
`OPENSSL_PATH`（必要时 `ACL_LIBRARY_DIRS`）。公开头文件及所需依赖包含目录由目标传递。
独立链接测试强制加载归档中全部对象，能发现运行时对宿主实现的意外反向依赖。

## webcool 集成

- CMake：`add_subdirectory(ai)` 后链接 `webcool::ai_coding`。
- Make：`make ai-coding-lib` 只构建库；`make` 自动构建并链接 `ai/libwebcool_ai_coding.a`。
  AI 对象不会再次直接参与 webcool 链接；配置变化、头文件依赖和 clean 同样覆盖库对象。
- Visual Studio：`webcool.sln` 包含静态库工程，webcool 通过 `ProjectReference` 链接，
  不再编译库拥有的源文件。各配置沿用 webcool 对应的 CRT 设置。

持久化格式、HTTP API、权限检查和模型行为保持现有约定。共享的平台兼容和互斥锁实现
归库所有，原 webcool 头文件转发到库版本，避免两份实现产生差异。
