#pragma once
#include <string>
namespace webcool
{
namespace ai
{
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
