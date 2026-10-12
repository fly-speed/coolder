# coolder-rust

独立 Rust 浏览器 AI 编程 APP，通过 `libai-rust` 的公开 `Agent`、`CodingService` 接口使用 libai。
Rust 实现 HTTP/1.1 监听、请求解析/限制、二进制与 SSE 响应、CLI、日志和生命周期；
不用启动或连接 C++ coolder。默认 `http://127.0.0.1:18097`，数据 `var-rust`，日志 `coolder-rust.log`。

## 构建、运行

需要 Rust/Cargo 1.77+、CMake、C++17 编译器及同级预编译 ACL/OpenSSL。
Rust APP 使用 serde_json、sha2、pbkdf2、base64、getrandom。首次构建需下载 Cargo.lock 中的依赖；缓存后可通过 `CARGO_NET_OFFLINE=true` 离线构建。

```sh
cd /Users/zsx/work/github/fly-speed/coolder
./coolder-rust/build.sh
./coolder-rust/run.sh
```

自定义路径或并行度：

```sh
JOBS=8 ACL_PATH=/path/to/acl OPENSSL_PATH=/path/to/openssl \
  DIST_DIR=/tmp/coolder-rust-dist ./coolder-rust/build.sh /tmp/coolder-rust-build
/tmp/coolder-rust-dist/bin/coolder-rust --port 18097 \
  --data /path/to/rust-data --workspace /path/to/projects --log-file /path/to/rust.log
```

`CARGO`/`CMAKE` 可以指定工具位置；`--html DIR` 覆盖浏览器资源，`--help` 显示参数。
输出包含 executable、libai 动态库、网页和沙箱辅助程序。macOS/Linux 配置相对库加载路径，
移动 APP 时一并保留根目录 `html/`，也可通过 `--html` 指定资源目录；Windows 需部署 DLL 和 ACL/OpenSSL 依赖，尚未进行 Windows 实机验证。
`./coolder-rust/clean.sh [build-directory]` 只清理生成文件，不删除数据和日志。

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
python3 coolder/tests/language_apps_test.py --rust ./coolder-rust/dist/bin/coolder-rust
python3 coolder/tests/native_apps_test.py ./coolder-rust/dist/bin/coolder-rust
```

测试覆盖原 APP 的账号、共享和完整 HTTP 行为，以及索引、工作流、沙箱确认/重放、
GET 禁止修改、二进制资源、日志和退出。当前构建与运行验证平台为 macOS。
详见 [原生 APP 架构](../coolder/docs/NATIVE_APPS.md)。

目录约定：`build/` 只保存 CMake/Cargo 缓存、对象文件和临时 classes；`dist/` 保存可运行的 `bin/`、`lib/`；网页资源放在 APP 根目录的 `html/`，与 `src/` 同级。Java classes 打包为 `dist/lib/coolder-java.jar`。`clean.sh` 只清理临时编译目录，保留 `dist/`、运行数据及日志；发布包可以在清理后独立运行。自定义编译目录时发布目录默认是其同级 `dist/`，可用 `DIST_DIR` 指定（须在编译目录之外）。

`html/` 是本 APP 的前端资源目录；构建仅在该目录更新 `coolder.js` bundle，不会从原 coolder 覆盖编辑过的网页文件。`clean.sh` 保留 `html/`。

`./run.sh [APP 参数...]` 从本 APP 根目录启动，默认数据和日志也位于该目录。支持 `DIST_DIR` 指定发布目录；所有参数原样透传，例如 `./run.sh --port 18097 --data /path/to/data`。未构建时会提示先运行 `build.sh`。

持久化数据使用 JSON，旧格式启动时迁移并保留 `.legacy` 备份。存储结构、迁移规则与跨 APP 使用范围见 [JSON_STORAGE.md](../coolder/docs/JSON_STORAGE.md)。

## 完成前验证

模型结束前，APP 在私有临时目录中合并正式源码和本会话的待确认修改，通过 libai 的固定命令沙箱执行编译或语法检查。正式目录只在用户接受修改后写入。模型自行调用工具链发现或返回空诊断，不能代替实际验证。

编译失败时，真实退出码和 stdout/stderr 会反馈给模型，最多允许三轮修复；同一文件的新修改替换旧草稿并增加 generation。工具缺失、项目没有受控编译命令、沙箱不可用或达到修复上限时，不标记 completed，可以查看验证记录并从检查点继续。没有源码修改或只有文档/文本修改时标记 not_required。

验证状态及每轮命令输出保存到运行记录的 `validation` / `validation_attempts`，会话界面显示结果。验证遵守管理员的语言工具、网络和资源限制；取消任务同时取消正在执行的沙箱编译。临时目录拒绝符号链接，并在验证结束后清理。

```sh
python3 coolder/tests/rust_completion_validation_test.py ./coolder-rust/run.sh
```
