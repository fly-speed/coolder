# 跨浏览器运行与验收

## 默认行为

macOS 上，标记为 `.webcool-http-service` 的项目在完整 workspace.validate 和最终草稿验证时，启动一次草稿服务，然后依次运行 Chrome（Chromium）和 Playwright Firefox。每个引擎都检查桌面 1100×800、窄屏 390×844。所有组合使用相同验收步骤，每次创建独立浏览器会话。自动编译、仅修编译错误的任务保持原范围。

项目契约可增加 WebKit；无法通过契约删除默认 Chrome/Firefox。修复后的全量验证仍运行整个矩阵，不只重测失败的 Firefox。全部目标通过才返回 passed；缺失引擎返回 unavailable；任何实际检查失败返回 failed，并保留其他引擎结果。外层沙箱的管理员超时/资源限制仍生效，超时不算通过。

## 独立端口

创建空的 `.webcool-http-port-env`，服务读取 `WEBCOOL_HTTP_PORT`（未设置时默认 18080），绑定 127.0.0.1。验证分配独立空闲端口，浏览器使用相同地址；前端使用同源相对 URL。原服务不被终止。旧项目未迁移且端口占用时明确显示“浏览器未执行”。

## 检查和修复反馈

收集页面异常、console.error、请求失败及 HTTP 错误；检查 HTML 脚本未闭合和 2D Canvas 绘制。布局断言记录元素尺寸、位置、overflow、裁切祖先和中心点遮挡。默认抽查最多 24 个语义控件/画布/标题，比较带稳定 ID 元素的跨引擎显隐。项目需给所有关键面板配置 layout 断言，避免默认抽样漏检。正常垂直页面滚动和可滚动容器不自动判错。

WEBCOOL_BROWSER_REPORT 返回每个引擎/视口的结果和 differences，失败包含具体元素、几何位置和 CSS 信息。智能体使用这些诊断修复 CSS/JS/API 兼容性，再次 workspace.validate 验证全部组合，不允许删去失败引擎或弱化断言。浏览器结果依然不证明所有自然语言需求均已覆盖。

## 截图与证据

每个成功进入页面的组合保存 PNG 和 SHA-256；完整 JSON 保存到正式项目 `.webcool_agent/browser-evidence/matrix-*/report.json`，草稿清理不会移除它们。发给模型的反馈只保留失败检查及摘要，完整细节在报告中。当前模型主要使用结构化布局诊断，未接入截图图像语义识别；截图用于人工复核，不能据此声称进行了像素语义验收。导航/浏览器启动失败时可能没有截图。

历史证据会保留，可按需清理 browser-evidence 目录。报告对应当次草稿，应用后或外部改动后应重新验收。

## 契约示例

```json
{
  "version": 1,
  "browsers": ["webkit"],
  "steps": [
    {"requirement":"工具栏完整", "action":"layout", "selector":"#toolbar"},
    {"requirement":"画布完整", "action":"layout", "selector":"#board"},
    {"requirement":"初始画面", "action":"canvas-painted", "selector":"#board"},
    {"action":"press", "selector":"#board", "value":"Space"},
    {"requirement":"暂停状态", "action":"text", "selector":"#status", "value":"暂停"}
  ]
}
```

契约路径 tests/browser.acceptance.json，版本 1，最多 32 KiB/20 步。browsers 可省略（默认 Chrome+Firefox），指定 webkit 时增加 WebKit。动作支持 visible、layout、text（包含）、click、press、fill、canvas-painted、canvas-changed。后者与同选择器此前画面断言比较。press 支持 Space/Enter/Escape/Tab/方向键/WASD。服务端状态由各组合共享，契约应通过 UI 重置或新建独立房间，让每个组合起始状态可重复；独立浏览器会话不会自动清空服务端数据库。

## 安装和部署

```sh
cd webcool/browser
npm ci --ignore-scripts
PLAYWRIGHT_BROWSERS_PATH="$PWD/.browsers" npx playwright install firefox webkit
```

宿主需 Node 和 Chrome。Playwright 的测试内核固定部署在 browser/.browsers，不依赖沙箱 HOME 下的缓存。macOS 打包会连同 browser 目录复制，构建机器须安装与目标 OS/架构匹配的内核。Firefox 使用 Playwright 配套版本，不是用户普通 Firefox；WebKit 不等于实际 Safari。其他操作系统 broker 尚未接入此机制，报告 unsupported-platform。

测试网页只访问当前服务同源 HTTP/WebSocket；下载、Service Worker 禁用，Chrome 保留自身沙箱。项目输入是声明式 JSON，不在宿主 Node 中运行项目测试脚本。这些请求限制不是完整 OS 网络隔离。

## 回归验证

make browser-acceptance-test program-sandbox-test：覆盖未闭合 script、Canvas/按键、Firefox 专有裁切、修复后双引擎双视口通过、窄屏溢出、引擎缺失、可选 WebKit、显隐差异，以及真实 broker 的端口冲突、超时取消。已用实际 snake2 源码在独立端口复现脚本缺失，确认原 18080 服务不受影响。
