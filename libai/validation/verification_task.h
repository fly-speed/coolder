#pragma once
#include <algorithm>
#include <string>

namespace webcool
{
namespace ai
{
// Only explicit inspection-only requests opt out of automatic final builds.
// Ambiguous and mixed tasks keep the existing validation policy.
inline bool read_only_analysis_task(std::string prompt)
{
	const size_t first = prompt.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return false;
	prompt = prompt.substr(
	    first, prompt.find_last_not_of(" \t\r\n") - first + 1);
	if (prompt.compare(0, 3, "请") == 0)
		prompt.erase(0, 3);
	if (prompt.size() >= 3 && prompt.substr(prompt.size() - 3) == "。")
		prompt.resize(prompt.size() - 3);
	if (!prompt.empty() && prompt.back() == '.')
		prompt.pop_back();
	std::transform(
	    prompt.begin(), prompt.end(), prompt.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
	});
	for (const char *value : { "仅分析当前项目", "只读分析当前项目",
	         "只分析当前项目，不修改文件",
	         "analyze this project without changing files",
	         "analyze the current project without changing files" }) {
		if (!(prompt == value))
			continue;
		return true;
	}
	return false;
}
// Conservative explicit verification requests only. Mixed implementation/analysis
// tasks still need model delivery; a clean baseline does not complete them.
inline bool verification_only_task(std::string prompt)
{
	const size_t first = prompt.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return false;
	prompt = prompt.substr(
	    first, prompt.find_last_not_of(" \t\r\n") - first + 1);
	if (prompt.compare(0, 3, "请") == 0)
		prompt.erase(0, 3);
	if (prompt.size() >= 3 && prompt.substr(prompt.size() - 3) == "。")
		prompt.resize(prompt.size() - 3);
	if (!prompt.empty() && prompt.back() == '.')
		prompt.pop_back();
	std::transform(
	    prompt.begin(), prompt.end(), prompt.begin(), [](unsigned char c) {
		return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
	});
	for (const char *value : { "仅验证当前项目", "只验证当前项目",
	         "运行现有构建和测试", "只运行现有构建和测试",
	         "检查当前项目是否能编译通过", "verify the current project",
	         "run the existing build and tests only" }) {
		if (!(prompt == value))
			continue;
		return true;
	}
	return false;
}
}
}
