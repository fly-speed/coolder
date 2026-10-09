#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Bounded, provider-independent compiler/test diagnostic. `path` is always
// relative to the selected project; physical user-root paths are never exposed.
struct project_diagnostic_t {
	std::string path;
	long long line = 0;
	long long column = 0;
	std::string severity;
	std::string message;
};

// Parses common GCC/Clang/Go/Java and MSVC diagnostic lines. The source text is
// already bounded by the sandbox broker; this layer additionally caps count and
// message length and drops absolute paths outside `absolute_project_root`.
void parse_project_diagnostics(const std::string &source,
			       const std::string &absolute_project_root,
			       const std::string &project_path,
			       std::vector<project_diagnostic_t> &diagnostics);

} // namespace ai
} // namespace webcool
