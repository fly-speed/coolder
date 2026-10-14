# coolder-java

独立 Java 浏览器 AI 编程 APP，通过 `libai-java` 的公开 `Agent`、`CodingService` 接口使用 libai。
Java 实现 HTTP 监听、请求限制、流式响应、CLI、日志和生命周期；不用启动或连接 C++ coolder。
默认监听 `http://127.0.0.1:18096`，数据目录 `var-java`，日志 `coolder-java.log`。

## 构建、运行

需要 Python 3（打包 JAR，可用 `PYTHON` 指定）、JDK 8+（含 JNI 头文件）、CMake、C++17 编译器及同级预编译 ACL/OpenSSL。
macOS 自动匹配 JVM 的 arm64/x86_64 架构，依赖库也必须支持该架构。

```sh
cd /Users/zsx/work/github/fly-speed/coolder
./coolder-java/build.sh
./coolder-java/run.sh
```

可设置 `JAVA_HOME`，或分别设置 `JAVA`/`JAVAC`。自定义 JNI 头文件及依赖：

```sh
DIST_DIR=/tmp/coolder-java-dist ./coolder-java/build.sh /tmp/coolder-java-build \
  -DLIBAI_JNI_INCLUDE_DIR=/path/to/jdk/include \
  -DACL_PATH=/path/to/acl -DOPENSSL_PATH=/path/to/openssl
/tmp/coolder-java-dist/bin/coolder-java --port 18096 \
  --data /path/to/java-data --workspace /path/to/projects --log-file /path/to/java.log
```

`--html DIR` 覆盖前端目录；`--help` 列出参数。`JOBS` 设置构建并行度。
输出包含 Java classes、JNI 库、浏览器资源和独立沙箱辅助程序；移动 APP 时需保留同级根目录 `html/`，或通过 `--html` 指定。
仅清理生成文件：`./coolder-java/clean.sh [build-directory]`，不会删除运行数据或日志。

## 独立语言实现

HTTP 路由、账号、会话令牌、权限、项目注册和共享、附件、异步任务、历史会话、
计划、索引、工作流与草稿审查分别由本 APP 的语言实现。共享原 coolder 的浏览器资源。
不调用 C++ APP 的路由，不启动或连接原 APP，也不把 APP 业务代码编译进 libai。

只有通用 AI 能力通过本语言的 libai 绑定调用：Agent 模型/工具循环、Provider 传输与加密配置、
源码轮廓、项目脚手架、已确认的变更事务和固定命令沙箱。
沙箱隔离依赖操作系统；不支持时明确失败，不退回普通进程执行。
Java/Rust 使用独立 cookie、数据目录与 APP 状态文件，可并行运行。
启动时把中断任务标记为 interrupted；用户明确恢复后才重新调用模型。
保存的会话 transcript 会在后续任务中重放，完成状态与会话持久化在同一 APP 锁内发布。

```sh
python3 coolder/tests/native_apps_architecture_test.py
python3 coolder/tests/language_apps_test.py --java ./coolder-java/dist/bin/coolder-java
python3 coolder/tests/native_apps_test.py ./coolder-java/dist/bin/coolder-java
```

测试覆盖原 APP 的账号、共享和完整 HTTP 行为，以及索引、工作流、沙箱确认/重放、
GET 禁止修改、二进制资源、日志和退出。当前构建与运行验证平台为 macOS。
详见 [原生 APP 架构](../coolder/docs/NATIVE_APPS.md)。

JNI 构建只需要头文件，不依赖 AWT 或链接 JVM。脚本按选定 Java 的 `java.home` / `JAVA_HOME` 探测完整 JDK；macOS/Linux 上也可自动使用已安装 Android NDK 的 VM 通用 JNI 头文件，支持 Android Studio 内置 JDK 缺少 `include/` 的情况。NDK 默认位置为 macOS 的 `~/Library/Android/sdk` 或 Linux 的 `~/Android/Sdk`，也可设置 `ANDROID_SDK_ROOT` / `ANDROID_NDK_HOME`。没有可用头文件时，配置阶段给出完整 JDK 或显式头文件路径的修复提示。无需额外指定临时目录中的 JNI 头文件。

目录约定：`build/` 只保存 CMake/Cargo 缓存、对象文件和临时 classes；`dist/` 保存可运行的 `bin/`、`lib/`；网页资源放在 APP 根目录的 `html/`，与 `src/` 同级。Java classes 打包为 `dist/lib/coolder-java.jar`。`clean.sh` 只清理临时编译目录，保留 `dist/`、运行数据及日志；发布包可以在清理后独立运行。自定义编译目录时发布目录默认是其同级 `dist/`，可用 `DIST_DIR` 指定（须在编译目录之外）。

`html/` 是本 APP 的前端资源目录；构建仅在该目录更新 `coolder.js` bundle，不会从原 coolder 覆盖编辑过的网页文件。`clean.sh` 保留 `html/`。

`./run.sh [APP 参数...]` 从本 APP 根目录启动，默认数据和日志也位于该目录。支持 `DIST_DIR` 指定发布目录；所有参数原样透传，例如 `./run.sh --port 18096 --data /path/to/data`。未构建时会提示先运行 `build.sh`。

持久化数据使用 JSON，旧格式启动时迁移并保留 `.legacy` 备份。存储结构、迁移规则与跨 APP 使用范围见 [JSON_STORAGE.md](../coolder/docs/JSON_STORAGE.md)。

### 增量生成与运行进度

标准模式单次模型输出最多 16,384 tokens，大项目模式最多 32,768 tokens，
同时受管理员总输出预算约束；快速模式仍使用管理员配置。
GLM-5.3 系列使用 `reasoning_effort=low`，避免一次请求长期只进行推理。
请求开始即持久化交互记录，错误工具调用也保留在任务记录中。
AI 可通过 `workspace_read` 读取当前任务尚未接受的文件草稿，逐步修改实现，
无需等待用户接受后才能检查生成的文件。此操作不会写入正式项目目录。


### 完成前编译验证

Java宿主在模型返回最终回答前检查当前会话全部待审核修改。它在有界临时副本中
合并草稿，通过libai的固定命令沙盒执行编译或语法检查，并等待已启动进程结束后
清理副本。正式目录保持不变。模型不能提供任意命令、执行目录或验证报告。

`project_validation_result`返回真实started/ok/exit_code/取消/超时及诊断。失败交给模型
修复，最多四次失败验证（首次加三轮修复）；工具链不可用明确未验证。模型直接请求
该工具也受同一门槛，不能绕过次数限制。仅文档或无待审核修改标记not_required。

执行前保存工具意图，报告、版本、调用记录及宿主收据一起保存后才反馈给模型。
存储失败停止执行；未知效果不自动重跑。报告验证临时副本与成果指纹一致，复核编译
前已有文件没有被修改/删除，允许编译器生成临时产物。外部源码变化使旧证据失效。
诊断按UTF-8/JSON包络限制，可能明确截断；当前不自动运行全部业务测试或浏览器验收。

验证命令（需要对应Go/Python工具链）：

```sh
python3 coolder/tests/java_completion_validation_test.py --binary coolder-java/dist/bin/coolder-java
```

原SDK执行上下文的instructions保持兼容，新增验证规则由宿主门槛和工具目录执行。
APP旧日志核实与迁移管理入口仍待实现，不能通过手动清除日志解除未知副作用。

## 恢复诊断

已停止且启用了 SDK 持久执行的任务，可在原会话点击“恢复诊断”。
界面显示快照版本、运行阶段、调用计数、时限标志、工具幂等键、尝试次数和收据
核对需求；参数以纯文本展示。查询不会运行模型/工具或提交日志，不会授予续跑权限。

登录后可使用 `GET /api/v1/ai/runs/recovery?id=<run-id>` 查询自己账户的运行记录。
服务端根据已保存的冻结上下文和执行 scope 读取私有存储，不接受客户端覆盖
scope、存储路径、输入或系统指令。正在执行、冻结身份无法核实或日志损坏时
拒绝查询；其他账户的运行 ID 返回 404。存储读取短暂取得执行租约，可能创建
锁文件，不改变检查点内容。未知效果仍须可信宿主核对，未增加强制重放按钮。
