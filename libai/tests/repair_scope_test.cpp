#include "../validation/repair_scope.h"
#include "../validation/diagnostic_source.h"
#include <cassert>
using namespace webcool::ai;
int main()
{
	assert(unique_diagnostic_source("server_test.go",
					{ "src/adapters/httpapi/server_test.go",
					  "src/main.go" }) ==
	       "src/adapters/httpapi/server_test.go");
	assert(unique_diagnostic_source(
		       "server_test.go",
		       { "a/server_test.go", "b/server_test.go" })
		       .empty());
	assert(unique_diagnostic_source("missing.go", { "src/main.go" })
		       .empty());
	assert(focused_compile_repair("编译报错：undefined: next"));
	assert(compile_command("cpp.make"));
	assert(focused_compile_repair("请修复这个编译错误：undefined: next"));
	assert(focused_compile_repair(
		"Fix this compiler error: undeclared identifier"));
	assert(!focused_compile_repair(
		"Fix this compiler error and run all tests"));
	assert(!focused_compile_repair("全面修复编译错误并完整验收"));
	assert(!focused_compile_repair("解释这个编译错误"));
	assert(!focused_compile_repair("不要修复，只解释这个编译错误"));
	assert(!focused_compile_repair(
		"Do not fix this compiler error; explain it only."));
	assert(!focused_compile_repair(
		"Implement an application and fix compiler errors"));
	assert(!focused_compile_repair("修复游戏方向键问题"));
	assert(compile_command("go.build") &&
	       compile_command("cpp.cmake.build"));
	assert(!compile_command("cpp.cmake.configure") &&
	       !compile_command("go.test"));
	const auto packages = changed_go_packages(
		"snake",
		{ "snake/src/main.go", "snake/src/a.go", "snake/core/game.go",
		  "other/x.go", "snake/../bad.go", "snake/.../x.go",
		  "snake/README.md" });
	assert((packages == std::vector<std::string>{ "./core", "./src" }));
}
