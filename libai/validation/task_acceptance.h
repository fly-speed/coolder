#pragma once
#include <string>
#include <algorithm>
#include <cctype>
namespace libai
{
namespace ai
{
// Identify UI requests without treating ordinary build checks as browser evidence.
inline bool needs_browser_interaction(std::string request)
{
  std::transform(request.begin(), request.end(), request.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const char *term : {"网页", "页面", "界面", "颜色模式", "深色", "浅色",
           "html", "website", "browser", "frontend", "dark mode", "light mode"})
    if (request.find(term) != std::string::npos) return true;
  return false;
}
// Summarize the available browser evidence without claiming untested coverage.
inline std::string browser_evidence_summary(bool current,
    const std::string &status, bool chinese)
{
  if (!current) return chinese ? "浏览器交互待验收：缺少当前版本证据。" :
      "Browser interaction pending: no current-revision evidence.";
  if (status == "passed") return chinese ?
      "已配置浏览器用例通过；只覆盖所测环境和断言，不能替代原始页面在用户访问方式下的验收。" :
      "Configured browser cases passed; only their environment and assertions are covered, not every user access mode.";
  if (status == "disabled") return chinese ?
      "浏览器交互未执行：管理员未启用浏览器验收；脚本或模拟 DOM 测试不能证明实际跨页交互正常。" :
      "Browser interaction not run: disabled by administrator; script or mock DOM tests do not prove real navigation behavior.";
  if (status == "failed") return chinese ? "浏览器交互验收失败。" : "Browser interaction checks failed.";
  return chinese ? "浏览器交互尚未验证；不能报告页面功能已修复。" :
      "Browser interaction unverified; do not claim the page behavior is fixed.";
}
// Runtime evidence only. A passing suite proves its assertions, never coverage
// of an arbitrary natural-language task. Narrow compile repair is explicit.
inline std::string task_acceptance_status(bool current, bool executed,
    bool passed, bool compile_scope, bool compile_passed)
{
	if (!current || !executed)
		return "pending_verification";
	if (!(compile_scope && compile_passed))
		return passed ? "checks_passed" : "verification_failed";
	return "scope_verified";
}
}
}
