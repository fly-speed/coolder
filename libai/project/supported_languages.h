#pragma once

#include <string>

namespace webcool
{
namespace ai
{

// Canonical backend contract for languages exposed by the project wizard.
// Keep policy validation and scaffold validation on this shared list so a
// browser option can never be accepted by one service and rejected by another.
inline bool supported_project_language(const std::string &value)
{
	return value == "cpp" || value == "c" || value == "javascript" ||
	    value == "python" || value == "java" || value == "go" ||
	    value == "rust" || value == "objective-c" || value == "swift" ||
	    value == "csharp" || value == "kotlin" || value == "php" ||
	    value == "d";
}

inline bool supported_configurable_language_tool(const std::string &value)
{
	return supported_project_language(value) && value != "cpp" &&
	    value != "c";
}

inline bool supported_project_platform(const std::string &value)
{
	return value == "cross-platform" || value == "windows" ||
	    value == "linux" || value == "macos";
}

inline bool project_language_supports_platform(
    const std::string &language, const std::string &platform)
{
	return language != "objective-c" || platform == "macos";
}

} // namespace ai
} // namespace webcool
