#pragma once

#include "../sandbox/program_sandbox.h"

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct project_toolchain_t {
	// Informational languages may include disabled runtimes such as Python/JS.
	std::vector<std::string> detected_languages;
	// Independent of whether a project has created the optional test script yet.
	bool functional_acceptance_enabled = false;
	bool browser_acceptance_enabled = false;
	std::string browser_unavailable_reason;
	// Only commands safe to present for explicit confirmation are listed here.
	std::vector<sandbox_command_t> commands;
	std::vector<std::string> unavailable_tools;
};

// Resolves the administrator-configured Go executable or performs safe
// read-only discovery. Exposed so the settings page can report what startup
// discovery found without executing the tool.
std::string discover_go_executable(const std::string &configured_path);

// Resolves any persisted language-tool path with the same trusted fallback
// rules used by project discovery. Supported names are node, python, javac,
// java, go, cargo, make, swift, dotnet, kotlinc, php, and dmd.
std::string discover_language_executable(
    const std::string &name, const std::string &configured_path);

// Startup and project discovery use the same local capability check.
bool browser_debug_available(std::string &reason);
bool browser_debug_enabled(std::string &reason);

// Read-only manifest detector and fixed command policy factory. It never runs
// package managers or evaluates project scripts. Runtime discovery checks only
// administrator configuration and trusted server startup environment paths.
class project_toolchain_catalog_t {
public:
	explicit project_toolchain_catalog_t(const std::string &user_root);

	bool discover(const std::string &project_path,
	    project_toolchain_t &result, std::string &err) const;
	bool is_git_repository(const std::string &project_path, bool &result,
	    std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
