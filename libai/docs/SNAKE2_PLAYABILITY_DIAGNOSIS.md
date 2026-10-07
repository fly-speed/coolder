# snake2 不可玩诊断（2026-09-23）

项目：`webcool/uploads/共享目录/soft/deepseek/snake2`。本轮只分析；原项目、游戏状态与运行服务均未修改。复现代码在 Codex 工作目录的隔离副本中执行。

## 确认的主要问题

最新运行 `6fb5543c678f86dedd30322a31ca3195` 使用 26/32 次工具调用，完成构建、Go 测试及 HTTP 就绪检查，未耗尽额度。报告同时标明 `functional_acceptance: not-verified`、`functional.acceptance.not-configured`。

本轮 AI 对“不能玩”的修复修改 server.go、player.go、board.go、hub.go，将房间逻辑改成每位玩家独立的 PlayerSnake，但没有同步修改 index.go 的页面协议和渲染。

- `src/application/hub.go:155`：响应通过 h.view(room) 产生，没有传入请求玩家身份。
- `src/application/hub.go:438`：兼容字段 snake、score 永远取第一位玩家。
- `src/adapters/index.go:203`：页面仅绘制 view.snake，忽略多玩家集合 view.snakes；按键却按当前 name 操控自己的 PlayerSnake。
- `src/adapters/index.go:89`：每次刷新随机生成新玩家名；没有离开接口/心跳超时清理接入，旧玩家及其蛇继续占据第一位。Hub.Leave 虽然存在，但没有页面或路由调用它。
- 页面的暂停、结束提示取房间全局状态；第一位玩家死亡而其它玩家仍存活时，房间继续 running，但页面绘制第一位玩家已死亡的旧蛇，形成“运行中但不动”。

## 隔离复现

先运行原有 `go test ./...`，所有包通过。然后在副本新增两个诊断测试，分别稳定复现并以失败断言报告：

1. 第一位玩家从 (12,9) 移动至 (13,9)，第二位玩家向下从 (9,11) 移至 (9,12)。第二位玩家响应的兼容渲染字段仍为 (13,9)，不是自己的蛇头。
2. 模拟刷新产生新身份并将第一条蛇置为死亡后，新玩家向下移动至 (9,12)，房间状态为 running，但页面对应蛇头停在 (12,9)。

这些测试直接复现状态与控制对象错位；未声称执行了真实浏览器端到端验收。

## 为什么自动验证没有发现

`src/adapters/server_test.go:167` 的 TestTickAdvancesRunningRoom 只调用一次 loop.tick：首次调用仅设置下一次推进时间。随后只检查 Board.Snake.Body 非空，没有比较推进前后坐标，也没有检查实际多人路径 Room.Snakes。该测试在游戏没有移动时照样通过。

页面测试只检查返回 HTML 包含 foodRange、togglePause 等字符串；未执行 JavaScript、按键或刷新后多玩家流程。原 Board 单蛇测试也不能覆盖新增 PlayerSnake 的在线路径。HTTP 冒烟仅证明服务就绪，最终报告明确没有功能验收。

## 其它已确认的问题

调速数据竞争：游戏循环 `src/adapters/loop.go:60` 在 Hub 锁外读取 room.Board.Speed，而配置请求在 Hub.HandleInput 内写 Board.Speed。隔离 `go test -race -run TestDiagnosisSpeedRace ./src/adapters` 报告 DATA RACE，读写栈分别指向 loop.go:60 与 board.go:62。此项是独立并发缺陷，不将它当成已证明的画面不动主因。

页面轮询 `.catch(function () {})` 静默吞掉连接失败，没有断线状态展示；服务停止时页面可能继续显示旧画面。检查当时未发现 18080 监听进程，但这不能证明用户尝试玩时服务也未启动。

## 建议修复顺序

1. 对齐玩家身份、响应视图及页面渲染：稳定玩家标识，返回 selfId/自己的状态，绘制全部蛇并突出本人，得分和死亡提示关联本人。
2. 完成刷新重连与离线清理，明确个人复活与房间重开规则。
3. 将速度读取放到同一同步边界，避免暴露房间可变状态给循环。
4. 加入真正的移动、按键转向、暂停、死亡重开、刷新及双客户端验收；没有功能证据时继续明确标记“可玩性未验证”。不要以新增不验证行为的测试代替修复。

## 2026-09-23: confirmed blank-canvas root cause

Run 74d20f8fa02c8e642d9e416f777fac74 did not fix the common initialization blocker: src/adapters/index.go opens a script element but never closes it. The embedded HTML ends immediately after the JavaScript IIFE. The browser reaches EOF without executing that inline script.

Evidence:
- The live service on 127.0.0.1:18080 returns HTML identical to current source.
- Initial generation ed61f3ac and subsequent index.go revisions 105badfb, 1f804387, and 74d20f8f all omit the closing script tag.
- The latest saved model request includes the file ending with truncated=false. The model received the relevant evidence.
- Chrome and the in-app browser both show a blank canvas, static lobby name despite the room query, and no speed buttons.
- A separate diagnostic room's backend API returns a 24-by-18 board, three snake segments, three food positions, and one player snake.
- An isolated HTTP fixture serves the same HTML with only closing script/body/html tags appended, and replays that real API snapshot. The browser then shows the snake, food, grid, and speed buttons. This confirms the startup blocker, not full dynamic multiplayer acceptance. No game source was changed and the live service was not restarted.
- The run used 27 of 32 tool calls and returned a final answer voluntarily. Model metadata: deepseek-v4-flash, thinking disabled, reasoning_tokens=0. This alone does not establish the model's general capability.
- Build, Go tests, and HTTP smoke passed; functional acceptance was not verified. TestIndexServesFrontend checks HTTP 200 and text markers only. Go treats the HTML as a string, while extracting the JavaScript for a syntax check also passes: neither executes the HTML document.

Assessment: the model missed a visible root cause and asserted an unproven stale-executable explanation. The agent failed to supply browser startup/rendering evidence, allowing repeated repairs without observing the user's failure. Requirements were retained, all five revisions were accepted, and tool exhaustion was not the cause. Better summaries alone cannot fix this verification gap.

Priority: browser startup, console/network observations, and rendering assertions tied to the user's symptoms; minimal before/after reproduction for root-cause claims; diagnose shared initialization when multiple UI features fail together. Evaluate reasoning mode/model alternatives on the same failing example rather than assuming a model replacement will remove the need for verification.
