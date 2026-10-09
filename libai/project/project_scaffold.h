#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

class agent_workspace_t;

struct project_scaffold_result_t {
	std::string language;
	std::string platform;
	// True when the wizard had already created the selected empty directory.
	// Non-empty directories are never reused or overwritten.
	bool reused_empty_directory = false;
	// Relative paths only; callers must never expose the physical user root.
	std::vector<std::string> files;
};

// Creates a minimal project without invoking npm, pip, Maven, Gradle or any
// other external package manager. The selected language runtime is needed only
// when the user later builds or runs the generated project.
bool create_project_scaffold(agent_workspace_t &workspace,
			     const std::string &project_path,
			     const std::string &language,
			     const std::string &platform,
			     project_scaffold_result_t &result,
			     std::string &err);

} // namespace ai
} // namespace webcool
