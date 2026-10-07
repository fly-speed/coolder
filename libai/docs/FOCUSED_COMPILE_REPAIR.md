# 局部编译修复的完成条件

## 行为

当前请求明确报告编译错误或要求修复编译错误时，按局部编译修复处理。只分析、不修改的请求不进入该模式。明确要求全面修复、全量测试、完整验收或包含实现新功能、重构等需求时，保留原有项目级流程。分类只使用本轮原始请求，不使用历史项目目标。

局部模式中，代码修订保存后，运行时自动验证构建；编译未通过时将诊断返回模型，编译确认后立即结束本轮，不再请模型补写总结或继续排查业务问题。直接最终回复中的补丁、显式验证调用以及工具额度边界也遵守同一完成范围。

Go 项目将递归 `go test ./...` 收窄为改动文件所在包，包含移动前后路径并去重。参数直接传给固定沙箱命令，沿用沙箱限制和取消回调。所有构建先执行，再执行相关测试；Go 测试源码出现 `[build failed]` 时仍视为编译阻断，不会因主程序可编译而提前完成。

相关测试的运行失败会如实报告，结束本次编译修复，不自动继续改业务逻辑或测试断言。没有基线证据时，不声称这些失败一定是既有问题。其他语言目前没有可靠的测试到改动映射，因此跳过其测试并明确报告未验证；不默认执行全量测试、HTTP 启动检查和功能验收。当前 Go 映射粒度为包，不是精确到函数，也不覆盖所有反向依赖包。

## 报告与完成状态

- `validation_scope`：`compile-repair` 或 `project`。
- `compile_repair_completed`：局部任务的构建已确认，可以结束本轮。
- `validation_passed`：本次实际执行的验证是否通过；相关测试失败时仍为 `false`，不会伪造成功。
- `test_status`：实际执行状态，失败诊断显示在最终答复中。
- `out_of_scope_commands`：遍历过程中因任务范围而跳过的命令。

完整验收请求继续使用原来的编译、测试和验收修复流程。局部任务的完成不代表整个项目或业务功能已通过验收。

## 回归

在隔离临时目录、本地模拟模型和独立 webcool 测试进程中验证，未调用真实模型：

```sh
python3 -B tests/compile_repair_http_test.py --focused-compile
python3 -B tests/compile_repair_http_test.py --focused-go
python3 -B tests/compile_repair_http_test.py --focused-go-test-compile
python3 -B tests/compile_repair_http_test.py --focused-final
python3 -B tests/compile_repair_http_test.py --full-compile
python3 -B tests/compile_repair_http_test.py
python3 -B tests/compile_repair_http_test.py --batch-finish
```

以上用例均通过。局部用例先返回一个仍有编译错误的修订，再返回修复版本；恰好两次模型请求后结束。Go 用例确认相关失败诊断得到保留、无关测试包未运行；测试源码编译失败也不会误判完成。全量用例仍经过测试失败与修复，共四次请求。

独立库 CMake 测试新增 `ai_repair_scope`，覆盖中英文分类、全面验收优先、只读请求和包路径选择。webcool 已重新编译。

正在运行的 webcool 需要重启后才能加载新逻辑。本次没有自动重启用户进程，也没有改动贪吃蛇正式源码或旧任务状态。
