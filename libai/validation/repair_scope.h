#pragma once
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{
// Classify the current user request only, never remembered project goals.
// Explicit whole-project acceptance always wins over compiler diagnostics.
inline bool focused_compile_repair(std::string prompt)
{
	std::transform(
	    prompt.begin(), prompt.end(), prompt.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
	});
	for (const char *term : { "全面", "完整验收", "所有问题", "所有错误",
	         "所有测试", "全部测试", "全量", "整个项目", "all tests",
	         "all errors", "all issues", "full validation",
	         "full acceptance", "entire project", "implement", "新增",
	         "新功能", "重构", "refactor" }) {
		if (!(prompt.find(term) != std::string::npos))
			continue;
		return false;
	}
	for (const char *term : { "不要修改", "不要修复", "不修改", "不修复",
	         "仅分析", "只分析", "只解释", "do not modify", "do not fix",
	         "don't fix", "without changing" }) {
		if (!(prompt.find(term) != std::string::npos))
			continue;
		return false;
	}
	bool repair = false;
	for (const char *term :
	    { "修复", "修正", "解决", "fix", "repair", "resolve" })
		repair = repair || prompt.find(term) != std::string::npos;
	if (!repair)
		for (const char *term :
		    { "解释", "分析", "原因", "explain", "analyze", "why" }) {
			if (!(prompt.find(term) != std::string::npos))
				continue;
			return false;
		}
	for (const char *term :
	    { "编译错误", "编译报错", "编译失败", "编译不通过", "compile error",
	        "compiler error", "compilation error", "compilation failure",
	        "build error", "undefined:", "undefined reference",
	        "undeclared identifier" }) {
		if (!(prompt.find(term) != std::string::npos))
			continue;
		return true;
	}
	return false;
}

// Only explicit build commands count as evidence that compilation succeeded.
inline bool compile_command(const std::string &id)
{
	const auto ends = [&](const std::string &suffix) {
		return id.size() >= suffix.size() &&
		    id.compare(
		        id.size() - suffix.size(), suffix.size(), suffix) == 0;
	};
	return id == "project.script-build" || ends(".build") ||
	    ends(".make") || ends(".compile-main") || ends(".compile-tests") ||
	    ends(".syntax-check");
}

// Pass package arguments directly to Go, never a shell or a recursive ./... selector.
inline std::vector<std::string> changed_go_packages(
    const std::string &project, const std::vector<std::string> &paths)
{
	std::set<std::string> packages;
	const std::string prefix = project.empty() ? "" : project + "/";
	for (auto path : paths) {
		if (path.compare(0, prefix.size(), prefix) != 0)
			continue;
		path.erase(0, prefix.size());
		if (path.size() < 3 || path.substr(path.size() - 3) != ".go")
			continue;
		if (path.empty() || path[0] == '/' ||
		    path.find("../") != std::string::npos ||
		    path.find("...") != std::string::npos ||
		    path.find('\\') != std::string::npos)
			continue;
		const auto slash = path.rfind('/');
		packages.insert(slash == std::string::npos ?
		        "." :
		        "./" + path.substr(0, slash));
	}
	return { packages.begin(), packages.end() };
}
}
}
