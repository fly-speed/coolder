# coolder

`aicool` 下独立的浏览器 AI 编程应用。后端为 C++17 + ACL 协程 HTTP 服务，链接同级 `libai`；前端为无 npm 依赖的 HTML/CSS/JavaScript。无需运行或构建 webcool。

## 构建与启动

需要 CMake 3.16+、C++17 编译器以及 `third-party/acl` 和 OpenSSL。复用总项目已有依赖：

```sh
cd /Users/zsx/work/github/chat-client/aicool
make coolder
cd coolder
./build/coolder
```

也可以独立构建：

```sh
cmake -S coolder -B coolder/build -DCMAKE_BUILD_TYPE=Release
cmake --build coolder/build --parallel 4
ctest --test-dir coolder/build --output-on-failure
```

首次检出尚未构建 ACL 时，先执行 `make acl`，或者配置 coolder 时指定 `-DBUILD_ACL=ON`。依赖不在默认位置时可设置 `-DACL_PATH=/absolute/acl -DOPENSSL_PATH=/absolute/openssl`。

Linux 上，libai 自动查找并链接 liburing（优先 `uring-ffi`，其次 `uring`），以支持启用了 io_uring 的 ACL 静态库。`make -C coolder HAS_IO_URING=yes` 或 CMake 的 `-DHAS_IO_URING=ON` 可要求配置阶段检查此依赖；自定义库路径可通过 `-DAI_URING_LIBRARY=/absolute/path/to/liburing.a` 指定。此选项不改变预编译 ACL 的功能。

在浏览器打开 **http://127.0.0.1:18095**。首次进入时创建管理员账户，之后使用用户名和密码登录。普通用户由管理员创建，不开放自助注册。用户名允许 1–32 位英文字母、数字、下划线或连字符，密码为 10–128 字节。默认数据目录是启动时当前目录下的 `var/`。

```sh
./build/coolder --port 18095 --data /absolute/coolder-data \
  --workspace /absolute/authorized-projects
```

`--workspace` 指定管理员的项目工作区，默认 `var/workspace/`；普通用户的工作区位于 `var/users/<随机账户 ID>/workspace/`，彼此隔离。创建或导入项目时，可点击“选择目录”浏览运行 coolder 的电脑上的文件夹并回填绝对路径；新项目可选择空目录，或选择父目录后追加新目录名。也可以填写服务器上的绝对目录路径（允许位于 workspace 外），也可以填写工作区内的相对路径，例如工作区为 `/work`，项目为 `/work/myapp`，填写 `myapp`。管理员可以使用外部目录，普通用户需由管理员启用本地目录项目权限。数据目录、账户存储和用户目录不能与管理员工作区重叠。建议使用专门工作区，并为重要项目保留 Git 提交。

## Windows / Visual Studio 2022

直接打开本目录的 `coolder.sln`，将 `coolder` 设为启动项目。工程沿用 webcool 的 v143 工具集和同级依赖工程，无需编译 webcool 主程序。

1. 安装 Visual Studio 2022 的“使用 C++ 的桌面开发”、Windows SDK 和“用于 Windows 的 C++ CMake 工具”（用于合并前端脚本）。如选择 ARM64 / ARM64EC，需要安装相应的 MSVC 编译工具。
2. 保持 `aicool/coolder`、`aicool/libai`、`aicool/third-party/acl` 和 `aicool/third-party/zlib-1.2.11` 的目录结构。解决方案会自动构建 libai、ACL、ACL 协程库及 zlib。
3. 准备目标架构的 Windows OpenSSL：头文件放在 `third-party/openssl/include`，`libssl.lib`、`libcrypto.lib` 放在 `third-party/openssl/lib`，动态库放在其 `bin` 或 `lib` 目录。不能使用 macOS/Linux 的 `.a` 文件。头文件与库须为相同版本，架构须匹配所选平台；使用静态 OpenSSL 时还需匹配工程的运行库（Debug 为 /MDd，Release 为 /MD）。
4. 推荐先选择 `Release | x64`，生成解决方案，然后启动调试。在浏览器访问 `http://127.0.0.1:18095`。

支持 Debug / Release，以及 Win32、x64、ARM64、ARM64EC。也可在 VS Developer Command Prompt 中执行：

```bat
msbuild coolder.sln /m /p:Configuration=Release /p:Platform=x64
cd bin\x64\Release
coolder.exe --html html --data "..\..\..\var"
```

输出位于 `bin/<平台>/<配置>/`，包含 `coolder.exe`、`webcool-sandbox-helper.exe`、前端 `html/`、zlib DLL 及找到的 OpenSSL DLL。前端脚本在构建时自动合并。VS 调试默认使用上述输出目录和本项目的 `var/` 数据目录；辅助程序名称与 libai 的查找约定保持一致。

`coolder.windows.props` 提供共享设置。若 OpenSSL 库按平台/配置分别存放，可创建不纳入版本控制的 `coolder.local.props`：

```xml
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <CoolderOpenSSLLibDir>$(ProjectDir)..\third-party\openssl\lib\$(Platform)\$(Configuration)</CoolderOpenSSLLibDir>
    <!-- 未安装 VS CMake 工具时，可指定已安装的 cmake.exe 完整路径。 -->
    <CoolderCMake>C:\Program Files\CMake\bin\cmake.exe</CoolderCMake>
  </PropertyGroup>
</Project>
```

也可设置 `CoolderOpenSSLRoot`，但 libai 自身工程仍从同级 `third-party/openssl/include` 读取头文件，须保持该处头文件与自定义库版本一致。复制动态库时会查找 OpenSSL 根目录的 `bin` 和配置的库目录；多架构安装建议把对应 DLL 与 `.lib` 放在同一个平台/配置目录，并避免根目录 `bin` 混入其他架构的 DLL。若启动提示 TLS runtime unavailable，可通过 `COOLDER_LIBCRYPTO`、`COOLDER_LIBSSL` 指定实际 DLL 的完整路径。

工程结构可用 `python tests/vs_project_test.py` 检查。当前仅完成工程结构、依赖引用和源码覆盖检查，尚未在 Windows/MSVC 上实际编译、运行验证。

## 使用流程

1. 管理员打开“管理控制台 → 大模型厂商”，填写名称、协议、Base URL、模型 ID 和 API Key。编辑时密钥留空表示保留；密钥通过 libai 加密保存，列表不会返回明文或密文。
2. 勾选“启用此模型”和“允许向此模型发送项目代码”，保存后可测试连接。普通用户只能使用已启用的模型，不能新增、修改、删除或测试厂商配置。支持 libai 中的 OpenAI compatible、Chat、Responses、Anthropic、Gemini、Ollama 协议。
3. 在“用户管理”中创建普通用户；可停用/启用账户及重置密码。在“智能体设置”中配置总开关、角色权限、每用户并行任务数、语言工具、输出/调用上限及超时。用户登录后创建项目脚手架，或导入已有目录/Git 仓库。
4. 输入任务，选择快速/标准/大型模式，启动 libai 模型与工具循环。可查看状态、工具记录、暂停、继续、取消。
5. 任务生成完成后，如仍有未接受的修订，界面显示“生成完成 · 待写入项目”。文件保存在修订区，尚未写入正式项目目录。在“变更”中检查差异，逐个接受/拒绝，或点击“全部接受并写入项目”一次接受所有待处理变更。审核会校验文件版本和哈希；不会强行覆盖之后的外部编辑。刷新页面后可重新选择会话，从历史任务中继续审核已有成果。
6. 文件面板支持浏览、编辑、预览差异、确认保存；补丁使用乐观并发校验。会话可继续、导出，历史运行可重新查看；异常中断后的检查点由 libai 恢复。
7. “项目计划”支持生成模块/任务计划、保存版本、更新任务状态。

`Ctrl/Cmd+Enter` 可提交任务。服务重启后，用户账户、智能体策略、模型配置、项目、会话和运行记录保留；浏览器登录会话需要重新建立。正在运行的任务的恢复由状态查询触发；历史工具输出仅在 libai 允许的保留期内可用。

## 结构与复用来源

- `main.cpp`：独立入口、数据目录、账户和策略初始化、ACL accept 协程与每连接协程、退出处理。
- `server/host.*`：账户身份/工作目录适配、Host/Origin 校验、静态资源白名单、角色策略校验和 HTTP 分发。
- `server/auth.*`：管理员初始化、用户管理、密码哈希、登录会话、账户持久化。
- `server/ai_policy.cpp`：从 webcool 管理模块适配的智能体设置接口，使用 libai 策略存储。
- `server/routes.inc`：明确注册的 AI 路由，沿用 `/api/v1/ai/` 协议。
- `action/ai/`：从 `webcool/action/ai` 复制并适配的 Provider、项目、会话、工作区、任务和审核模块；移除图片助手/标签模块和网盘备份回调。禁用 HTTP 请求自行指定外部目录，统一使用启动时授权的工作区。
- `action/actions.h`、`action/action_util.h`：独立宿主适配接口，不依赖 webcool 的用户数据库或其他业务模块。
- `html/`：独立工作台。
- `sandbox/sandbox_helper_main.cpp`：从 webcool 复制的沙盒辅助进程。二进制名保留 `webcool-sandbox-helper`，因为 libai 按此名称寻找相邻 helper。主程序仍为 `coolder`。
- `tests/http_test.py`、`tests/accounts_test.py`：本地模拟模型、多用户隔离、并发任务和账户生命周期的真实 HTTP 集成测试。

libai 仍使用原 `webcool::ai` / `action::agent_detail` 命名空间和内部 `.webcool_*` 持久化格式；它们是库的兼容约定，不代表依赖 webcool 应用。脚手架文字及模型系统提示中的少量 WebCool 标识来自 libai，未为此修改共享库。

## 服务范围与配置

当前支持多用户账户及独立编程工作区，服务固定绑定 `127.0.0.1`，只接受对应 `127.0.0.1:端口` 的 Host；尚未开放其他电脑直连。可以使用不同浏览器/浏览器配置文件同时登录不同账户。

登录使用 HttpOnly、SameSite=Strict Cookie，有效期 8 小时，令牌不放入 localStorage。退出登录会撤销该会话；停用账户或重置密码会撤销该用户所有会话，用户修改自己的密码后保留新会话。密码使用随机盐和 PBKDF2-HMAC-SHA256（210000 轮）保存，不存储明文；账户文件仅当前操作系统用户可读写。管理员可管理普通用户，但不会自动获得普通用户项目的浏览入口。

用户停用及智能体策略变更会阻止后续请求/新任务，已经开始的模型任务继续沿用启动时的策略快照。若要停止已开始的任务，先由用户取消任务或停止服务。

旧单用户版本升级时：保留原数据目录，直接用用户名和密码创建一次管理员，无需原访问令牌；管理员继承原项目工作区，共用模型存储仍使用原加密主体以保留密钥。旧 `access-token` 文件不再读取，也不能作为 Cookie/Bearer 登录；可自行移除。管理员创建完成后，注册接口拒绝再次创建，重启后仍然有效。账户库损坏时服务拒绝启动，不会重新开放管理员注册。

共享目录、任意本地目录、邮件、文件备份和浏览器调试扩展均不在此应用内。构建/运行隔离沿用 libai 的沙盒能力，未放开任意 shell API；各语言是否可执行取决于本机工具链与沙盒支持。模型联网、收费和输出质量由用户配置的服务商决定。

默认可配置语言工具为 Python、JavaScript、Go、Java、Rust、Swift；C/C++ 属于 libai 固定基础工具，实际工具链需自行安装。ACL 当前依赖已静态链接 OpenSSL 时可直接使用 HTTPS；使用动态 ACL/OpenSSL 构建的环境可通过 `COOLDER_LIBCRYPTO`、`COOLDER_LIBSSL` 指定兼容动态库。证书验证保持由 libai 执行。

退出使用 Ctrl+C 或 SIGTERM。POSIX 平台对数据目录加进程锁，避免两个进程同时使用同一数据目录；每个独立服务实例应指定不同数据目录和端口。

## 安装

```sh
cmake --install build --prefix /absolute/install
/absolute/install/bin/coolder --data /absolute/coolder-data \
  --html /absolute/install/share/coolder/html
```

安装时主程序和 sandbox helper 位于同一 `bin/` 目录。

## 验证

会话正文解析回归测试：`node tests/chat_display_test.js`。覆盖最终结果、服务商响应封装、流式 JSON、普通 JSON 示例及错误消息。

已在本机 macOS 完成编译和本地模型 HTTP 集成测试，覆盖未授权请求、Host/Origin 校验、路径越界/符号链接、模型密钥不回传、项目创建、真实 libai 工具读取/文件提案、逐版本审核、文件补丁、暂停/取消及重启持久化。本次新增测试覆盖首次管理员创建竞争、普通用户越权拒绝、同名项目/任务隔离、多用户并发、退出与停用后的会话撤销、密码修改/重置、策略持久化、旧数据免令牌初始化及账户库损坏时拒绝启动。浏览器验收覆盖首次初始化页、管理员/普通用户登录、管理控制台、智能体设置、厂商表单和退出登录。

测试使用临时目录和本地模拟模型，不读取用户原有 webcool 数据，也不需要真实 API Key。**尚未调用真实外部模型，也未在 Linux/Windows 上构建验证**；这些平台使用同一 CMake/libai 依赖配置，需在目标环境继续验证。

## 账户与管理 API

- `GET /api/v1/auth/status`：初始化状态、当前身份及 AI 权限。
- `POST /api/v1/auth/register`：仅首次创建管理员，提供 `username`、`password`，无需访问令牌；初始化后再次调用返回 409。
- `POST /api/v1/auth/login`、`POST /api/v1/auth/logout`：登录/退出。
- `POST /api/v1/auth/password`：使用 `current_password` 和新 `password` 修改自己的密码。
- `GET /api/v1/auth/users`：管理员查看用户。
- `POST /api/v1/auth/users/create`：管理员提供 `username`、`password` 创建普通用户。
- `POST /api/v1/auth/users/update`：管理员提供 `username`，修改 `enabled` 或重置 `password`。
- `GET/POST /api/v1/admin/ai-policy`：管理员读取或更新智能体策略，POST 支持字段增量更新。
- `/api/v1/ai/providers/save`、`delete`、`test`：仅管理员；普通用户的 `providers` 列表只返回启用的配置，密钥不会返回。

账户的创建/更新通过原子替换账户文件保存；同时启动两个服务访问同一数据目录会被进程锁拒绝。应备份整个数据目录及管理员外置工作区，包括 libai 的加密密钥文件。

智能体设置界面的字段、语言工具选项和输入范围与 `webcool/html/main.html`、`webcool/html/js/admin-ai-policy.js` 对齐，支持单项保存及批量保存修改项。可执行文件路径会显示自动检测结果；留空使用自动查找。敏感路径、各模式额度、模型超时、读取限制及沙盒资源限制由 libai 保存和校验。单项保存不会覆盖其他未提交的编辑。

共享目录、本地磁盘、浏览器调试以及加密主密钥轮换入口保留在设置界面，但因 coolder 尚未接入相应宿主模块而禁用，并显示原因；它们不会显示保存成功却不生效。共享目录、本地磁盘及浏览器调试权限提交为启用时，后端明确拒绝。


### Monaco 代码编辑器

代码查看与编辑使用从 webcool 复制的 Monaco Editor 0.55.1（`html/vendor/monaco-editor`），保留上游 LICENSE。所有编辑器资源与语言分析 Worker 均由本机服务提供，无需 CDN 或运行时 npm 安装。

文件预览支持语法高亮、行号、折叠和查找。点击“编辑文件”打开编辑窗口，支持自动补全、搜索替换、自动换行、撤销/重做及最大化；语言服务能力随文件类型而定，未接入外部 LSP。Ctrl/Cmd+S 生成差异预览，确认后才保存。窗口关闭会提示未保存的修改；截断文件禁止编辑。保存仍使用服务端补丁预览/应用接口，校验文件版本，避免覆盖其他任务的修改。

编辑器随个人设置切换明暗主题与字号。Monaco 内置菜单语言在首次加载时确定，改变语言后刷新页面生效。加载失败时保留文本查看/编辑和差异保存流程。更新 vendor 时应同时更新 `code-editor.js` 的 Worker 文件名。CSP 仅允许同源脚本、连接及本地 Worker；为 Monaco 动态布局/高亮允许内联样式，不允许内联脚本或 eval。

### 会话附件

会话输入框支持粘贴剪贴板中的图片或文件，也可以拖入文件或点击“＋”选择附件。普通文本粘贴保持不变；附件显示缩略图/文件名，可以单独移除。图片支持 PNG、JPEG、GIF、WebP，需要所选模型支持视觉输入；其他附件按 UTF-8 文本处理（例如源码、Markdown、JSON、需求文档），不支持 PDF、Office 等二进制文档。

沿用 libai 的限制：最多 8 个附件，单个不超过 8 MiB，总计不超过 16 MiB，文本上下文合计不超过 32 KiB。附件在点击发送时上传到当前账户的临时草稿区，任务接收后删除临时文件；发送失败保留浏览器中的附件以便重试。切换项目、会话或退出登录时清空待发送附件。附件不会自动写入正式项目目录。


源文件和差异确认采用两层标签：上层切换正文类型，下层切换文件。源文件标签保留各自的编辑内容、撤销记录和滚动位置，关闭未保存文件时会确认；关闭已审核差异仅隐藏视图。刷新浏览器仍会丢失未保存编辑，请先确认保存。

多文件标签回归测试：`node tests/source_tabs_test.js`、`node tests/diff_views_test.js`。

第二层文件标签支持右键菜单“关闭所有 / 关闭其它 / 关闭”，操作范围限于当前源文件或差异标签栏，“其它”以右键点击的标签为保留对象。源文件批量关闭前统一检查未保存内容，取消后不关闭任何标签。关闭差异仅隐藏视图，不接受或拒绝修改；AI 差异可通过“显示已关闭差异”恢复，手动编辑差异可从源文件重新预览。标签聚焦后也可按 Shift+F10 打开菜单，方向键选择，Escape 关闭菜单。

会话中每条用户提问显示本地日期和时间；历史提问优先读取任务审计中的开始时间，已淘汰或缺失的时间显示“时间未知”。左侧短横线索引支持摘要/时间预览、点击定位和当前提问高亮，随会话清空或切换重建。时间解析回归测试：`node tests/chat_time_test.js`。

### 前端脚本合并

首页仅加载 `html/coolder.js`，按原执行顺序合并 i18n、app、personal、code-editor、attachments 和 admin 六个应用脚本，首次加载的应用 JS 请求由 6 次降为 1 次。原始文件保留用于维护及回归测试，不再由首页逐个加载。修改源文件后执行 `cmake -P tools/bundle_frontend.cmake`；正常 `cmake --build build` 也会自动更新合并文件，无需 npm 或额外打包依赖。不要直接编辑生成的 `coolder.js`。

Monaco 的运行时、语言模块和 Worker 保留原有按需加载：Worker 在独立线程执行，不能与页面主线程脚本直接拼接；整个 vendor 目录约 20 MB，合并进首页反而会增加初次加载开销。首次打开代码文件才加载编辑器资源。部署时需同步新的服务器、首页和合并文件。

会话中的“任务进展”根据当前运行状态显示关键阶段，重复轮询不重复追加，最多保留最近 60 条，可折叠。提示只表示已观察到的阶段，不推断验证成功。阶段记录目前保留在当前页面，刷新后从最新状态重新显示。测试：`node tests/run_progress_test.js`。

### 项目共享

「项目操作」下拉菜单中的「项目设置」显示拥有者和参与者。拥有者可从用户列表中搜索、选择已有账号添加成员、将权限设为「只读」或「读写」，也可随时移除成员。参与者刷新项目列表后会看到共享项目及自己的权限。已有项目自动以所属账号为拥有者，无需迁移。

共享成员访问同一份项目文件：只读成员可以浏览，读写成员可以预览差异并保存编辑。保存采用内容摘要进行版本校验，文件已被其他成员修改时拒绝覆盖，需重新打开后合并修改。成员授权在每次请求时校验；权限降级和移除对后续请求立即生效。只有拥有者能管理成员或删除项目，删除项目登记后共享访问也失效。

当前共享范围是文件浏览和编辑，AI 会话、运行记录及任务计划仍由拥有者管理；暂不提供实时光标或自动合并编辑。成员关系保存在服务端 `auth/project-members.v1`，重启后保留，不会向参与者公开拥有者的其他项目、账号设置或会话。

### 静态页面项目

创建项目时，语言选择“静态页面（HTML/CSS/JS）”，即可生成 `index.html`、`style.css`、`script.js` 和使用说明。直接用浏览器打开 `index.html` 即可查看页面与交互，无需安装依赖或构建；支持工作区相对路径及授权的外部绝对路径。

### 日志文件

默认将 ACL 日志、标准输出及标准错误追加到启动时当前工作目录的 `coolder.log`。可使用 `--log-file /path/to/coolder.log` 指定文件，相对路径也基于当前工作目录。日志文件的父目录必须已存在且可写；无法打开时启动失败并报告错误。`--help` 不创建日志文件。

```sh
./coolder/build/coolder --log-file ./coolder.log
```

### HTML 网页预览

打开 `.html` 或 `.htm` 文件后，点击“网页预览”。预览包含当前未保存的编辑内容及项目内 CSS、普通 JavaScript 脚本和图片，点击“刷新预览”重新加载。预览在隔离框架中运行，支持 `onclick`、`onchange` 等内联事件；支持项目内 HTML 链接跳转；不支持外部网络、后端请求或需要构建的模块；无法加载的资源会显示提示。单个图片/字体上限为 2 MiB。

### Markdown 文档预览

打开 `.md`、`.markdown`、`.mdown` 或 `.mkdn` 文件后，点击“Markdown 预览”。支持标题、列表、任务列表、引用、表格、代码块及项目内图片，读取当前未保存的编辑内容；可刷新、最大化及还原。复用 aicool 的 Markdown 解析器，嵌入 HTML 显示为文本，文档脚本不执行，外部资源及页面跳转受预览隔离限制。

编辑文件后可点击工具栏的 **Vim** 开关启用 monaco-vim；每个文件独立切换，状态栏显示模式和命令。支持常用移动、插入、可视选择、搜索及撤销；`:w` 生成差异供确认，`:q` 退出编辑并检查未保存内容。插件及许可证复用自 aicool 的 vendor 目录，按需从本地加载。
