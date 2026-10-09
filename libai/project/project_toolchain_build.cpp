#include "stdafx.h"
#include "project_toolchain.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../agent/ai_admin_policy.h"

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include "../common/platform_compat.h"
#include "../common/windows_browser_runtime.h"
#include <windows.h>
#endif

#include "project_toolchain_internal.h"
namespace webcool
{
namespace ai
{
namespace toolchain_detail
{
void discover_git_commands(const std::string &normalized,
    project_toolchain_t &result, const std::string &user_root_)
{
	if (has_safe_git_marker(user_root_, normalized)) {
		// Prefer real developer-tool binaries on macOS. /usr/bin/git can be an
		// xcrun shim that writes a cache outside the project before launching Git,
		// which the sandbox correctly denies.
		const char *git_paths[] = {
			"/Applications/Xcode.app/Contents/Developer/usr/bin/git",
			"/Library/Developer/CommandLineTools/usr/bin/git",
			"/usr/bin/git", "/usr/local/bin/git"
		};
		const std::string git =
		    administrator_executable("WEBCOOL_AI_GIT", git_paths, 4);
#ifdef _WIN32
		const char *disabled_hooks_path = "NUL";
#else
		const char *disabled_hooks_path = "/dev/null";
#endif
		// Git is exposed only as fixed argv policies. No ref name, revision,
		// pathspec, hook, alias, pager, remote or arbitrary argument comes from
		// the browser or model.
		add_fixed_command(result, "git.status", git,
		    std::vector<std::string>{ "--no-pager", "-c",
		        "core.quotepath=false", "-c", "core.fsmonitor=false",
		        "-c",
		        std::string("core.hooksPath=") + disabled_hooks_path,
		        "status", "--short", "--branch" });
		add_fixed_command(result, "git.diff", git,
		    std::vector<std::string>{ "--no-pager", "-c",
		        "core.quotepath=false", "-c", "core.fsmonitor=false",
		        "-c",
		        std::string("core.hooksPath=") + disabled_hooks_path,
		        "diff", "--no-ext-diff", "--no-textconv", "--" });
		add_fixed_command(result, "git.diff-staged", git,
		    std::vector<std::string>{ "--no-pager", "-c",
		        "core.quotepath=false", "-c", "core.fsmonitor=false",
		        "-c",
		        std::string("core.hooksPath=") + disabled_hooks_path,
		        "diff", "--cached", "--no-ext-diff", "--no-textconv",
		        "--" });
		// The first command creates and switches to one deliberately fixed local
		// branch; it fails safely if that branch already exists. The checkpoint
		// command commits tracked changes only, so unknown/untracked files are
		// never swept into history unexpectedly.
		add_fixed_command(result, "git.branch-create", git,
		    std::vector<std::string>{ "--no-pager", "-c",
		        std::string("core.hooksPath=") + disabled_hooks_path,
		        "checkout", "-b", "webcool-agent-work" });
		add_fixed_command(result, "git.checkpoint-tracked", git,
		    std::vector<std::string>{ "--no-pager", "-c",
		        "user.name=WebCool Agent", "-c",
		        "user.email=webcool-agent@localhost", "-c",
		        std::string("core.hooksPath=") + disabled_hooks_path,
		        "commit", "-am", "WebCool agent checkpoint" });
	}
}

static void infer_cmake_service(
    const std::string &cmake_source, std::string &service_relative)
{
	std::istringstream lines(cmake_source);
	std::string line, source;
	while (std::getline(lines, line))
		source += line.substr(0, line.find('#')) + "\n";
	const std::regex target(
	    "add_executable[ \t\r\n]*\\([ \t\r\n]*([A-Za-z0-9_.-]+)[ \t\r\n]",
	    std::regex::icase);
	std::set<std::string> candidates;
	bool standard = false;
	for (std::sregex_iterator it(source.begin(), source.end(), target), end;
	     it != end; ++it) {
		const std::string name = (*it)[1];
		if (name == "webcool_app")
			standard = true;
		if (!(name == "server" || name == "http_server" ||
		        (name.size() > 4 &&
		            name.substr(name.size() - 4) == "_web")))
			continue;
		candidates.insert(name);
	}
	if (!standard && candidates.size() == 1) {
		service_relative = ".webcool-build/" + *candidates.begin();
#ifdef _WIN32
		service_relative += ".exe";
#endif
	}
}

bool discover_cmake_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool c_project, bool cmake, std::string &err)
{
	if (!cmake)
		return true;
	// Search fixed system installation locations, never project-controlled PATH.
	const char *cmake_paths[] = { "/usr/bin/cmake",
		"/opt/homebrew/bin/cmake", "/usr/local/bin/cmake" };
	const char *ctest_paths[] = { "/usr/bin/ctest",
		"/opt/homebrew/bin/ctest", "/usr/local/bin/ctest" };
	const std::string native_prefix = c_project ? "c" : "cpp";
	std::vector<std::string> configure_args{ "-S", ".", "-B",
		".webcool-build", "-DCMAKE_BUILD_TYPE=Debug" };
#ifdef __APPLE__
	// Use installed compiler binaries directly: /usr/bin compiler/make shims
	// invoke xcrun and attempt to write outside the private build directory.
	const std::string xcode = "/Applications/Xcode.app/Contents/Developer";
	const std::string clt = "/Library/Developer/CommandLineTools";
	const bool full_xcode = executable_file(
	    xcode + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++");
	const std::string developer = full_xcode ? xcode : clt;
	const std::string compiler_root = full_xcode ?
	    developer + "/Toolchains/XcodeDefault.xctoolchain/usr/bin/" :
	    developer + "/usr/bin/";
	if (executable_file(compiler_root + "clang++") &&
	    executable_file(developer + "/usr/bin/make")) {
		configure_args.push_back(
		    "-DCMAKE_C_COMPILER=" + compiler_root + "clang");
		configure_args.push_back(
		    "-DCMAKE_CXX_COMPILER=" + compiler_root + "clang++");
		configure_args.push_back(
		    "-DCMAKE_MAKE_PROGRAM=" + developer + "/usr/bin/make");
		configure_args.push_back("-DCMAKE_OSX_SYSROOT=" + developer +
		    (full_xcode ?
		            "/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" :
		            "/SDKs/MacOSX.sdk"));
	}
#endif
	add_fixed_command(result, native_prefix + ".cmake.configure",
	    administrator_executable("WEBCOOL_AI_CMAKE", cmake_paths, 3),
	    configure_args);
	add_fixed_command(result, native_prefix + ".cmake.build",
	    administrator_executable("WEBCOOL_AI_CMAKE", cmake_paths, 3),
	    std::vector<std::string>{
	        "--build", ".webcool-build", "--parallel", "2" });
	for (auto &command : result.commands) {
		if (!(command.id == native_prefix + ".cmake.configure" ||
		        command.id == native_prefix + ".cmake.build"))
			continue;
		command.allow_outbound_network = policy.allow_build_network;
	}
	add_fixed_command(result, native_prefix + ".ctest",
	    administrator_executable("WEBCOOL_AI_CTEST", ctest_paths, 3),
	    std::vector<std::string>{
	        "--test-dir", ".webcool-build", "--output-on-failure" });
	if (!http_service)
		return true;
#ifdef _WIN32
	const std::string service_name = "webcool_app.exe";
#else
	const std::string service_name = "webcool_app";
#endif
	std::string service_relative = ".webcool-build/" + service_name;
	std::string marker, marker_error;
	bool truncated = false;
	const std::string prefix = normalized.empty() ? "" : normalized + "/";
	if (workspace.read(prefix + ".webcool-http-service", marker, truncated,
	        marker_error) &&
	    !truncated) {
		const size_t first = marker.find_first_not_of(" \r\n\t");
		if (first != std::string::npos) {
			marker = marker.substr(first,
			    marker.find_last_not_of(" \r\n\t") - first + 1);
			if (!agent_workspace_t::normalize_path(
			        marker, service_relative, false, err))
				return false;
		} else {
			// Conservative compatibility for simple existing CMake projects.
			// Explicit marker paths handle variables, subdirectories and OUTPUT_NAME.
			std::string cmake_source;
			if (workspace.read(prefix + "CMakeLists.txt",
			        cmake_source, truncated, marker_error) &&
			    !truncated) {
				infer_cmake_service(
				    cmake_source, service_relative);
			}
		}
	}
	const std::string service = project_root + "/" + service_relative;
	add_http_service_command(result, native_prefix + ".http-smoke", service,
	    std::vector<std::string>());

	return true;
}

void discover_make_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c, bool c_project, bool cmake, bool make)
{
	if (make && !objective_c) {
		add_fixed_command(result,
		    (c_project ? "c" : "cpp") + std::string(".make"),
		    discover_language_executable(
		        "make", policy.make_executable_path),
		    std::vector<std::string>{ "-j2" });
		if (http_service && !cmake) {
#ifdef _WIN32
			const std::string service_name = ".webcool-app.exe";
#else
			const std::string service_name = ".webcool-app";
#endif
			const std::string service =
			    project_root + "/" + service_name;
			add_http_service_command(result,
			    (c_project ? "c" : "cpp") +
			        std::string(".http-smoke"),
			    service, std::vector<std::string>());
		}
	}
}

void discover_acceptance_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result)
{
	// A language-independent functional acceptance contract. Projects may use
	// Node's test runner to start their server and assert HTTP/browser behavior.
	// This remains an administrator-enabled fixed command inside the sandbox.
	if (has_relative_file(
	        workspace, normalized, "tests/acceptance.test.cjs")) {
		if (language_tool_enabled("javascript")) {
			add_fixed_command(result, "functional.acceptance",
			    discover_language_executable(
			        "node", policy.node_executable_path),
			    std::vector<std::string>{
			        "--test", "tests/acceptance.test.cjs" });
			if (!result.commands.empty() &&
			    result.commands.back().id ==
			        "functional.acceptance") {
				result.commands.back().allow_loopback_network =
				    true;
			}
		} else
			result.unavailable_tools.push_back(
			    "functional.acceptance.disabled-by-admin");
	} else
		result.unavailable_tools.push_back(
		    "functional.acceptance.not-configured");
}
}
}
}
