#include "stdafx.h"
#include "project_toolchain.h"
#include "project_toolchain_internal.h"
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

namespace webcool
{
namespace ai
{
namespace toolchain_detail
{

bool has_name(const std::vector<workspace_entry_t> &entries,
    const std::string &project_path, const std::string &name)
{
	const std::string expected =
	    project_path.empty() ? name : project_path + "/" + name;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (!(!entries[i].directory && entries[i].path == expected))
			continue;
		return true;
	}
	return false;
}

// Repository metadata is intentionally hidden from the model-facing workspace
// API. Git capability detection therefore checks only whether the protected
// .git marker exists; it never reads configuration, hooks, credentials or
// object data.
bool has_safe_git_marker(
    const std::string &user_root, const std::string &project_path)
{
	std::string root;
	std::string ignored;
	if (!agent_workspace_t::resolve_project_root(
	        user_root, project_path, root, ignored))
		return false;
	const std::string marker = root + "/.git";
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(marker.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return lstat(marker.c_str(), &st) == 0 && !S_ISLNK(st.st_mode) &&
	    (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode));
#endif
}

std::string first_executable(const char *const *candidates, size_t count)
{
#ifdef _WIN32
	(void)candidates;
	(void)count;
	return "";
#else
	for (size_t i = 0; i < count; ++i) {
		if (!(access(candidates[i], X_OK) == 0))
			continue;
		return candidates[i];
	}
	return "";
#endif
}

bool executable_file(const std::string &path)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return false;
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
	struct stat st;
	return !path.empty() && path[0] == '/' &&
	    stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
	    access(path.c_str(), X_OK) == 0;
#endif
}

std::string executable_from_search_path(const std::string &name)
{
	const char *configured_path = getenv("PATH");
	if (configured_path == NULL || *configured_path == '\0')
		return "";
	const std::string value(configured_path);
#ifdef _WIN32
	const char separator = ';';
	const char slash = '\\';
	const std::string filename = name + ".exe";
#else
	const char separator = ':';
	const char slash = '/';
	const std::string filename = name;
#endif
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find(separator, begin);
		std::string directory = value.substr(begin,
		    end == std::string::npos ? std::string::npos : end - begin);
		// Ignore relative and empty PATH entries: the server must never resolve a
		// project-controlled executable through its current working directory.
		bool absolute = false;
#ifdef _WIN32
		absolute = (directory.size() >= 3 && directory[1] == ':' &&
		    (directory[2] == '\\' || directory[2] == '/'));
#else
		absolute = !directory.empty() && directory[0] == '/';
#endif
		if (absolute) {
			while (!directory.empty() &&
			    (directory[directory.size() - 1] == '/' ||
			        directory[directory.size() - 1] == '\\'))
				directory.resize(directory.size() - 1);
			const std::string candidate =
			    directory + slash + filename;
			if (executable_file(candidate))
				return candidate;
		}
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return "";
}

std::string administrator_executable(
    const char *variable, const char *const *unix_candidates, size_t count)
{
#ifdef _WIN32
	(void)unix_candidates;
	(void)count;
	const char *configured = getenv(variable);
	if (configured == NULL || *configured == '\0')
		return "";
	const std::string path(configured);
	if (path.size() < 3 || path[1] != ':' ||
	    (path[2] != '\\' && path[2] != '/'))
		return "";
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide))
		return "";
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES &&
	        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
	        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 ?
	    path :
	    "";
#else
	// The value is controlled by the server administrator, never by a browser
	// or model. Accept only an executable absolute path; argv remains fixed.
	const char *configured = getenv(variable);
	if (configured != NULL && configured[0] == '/' &&
	    access(configured, X_OK) == 0) {
		return configured;
	}
	return first_executable(unix_candidates, count);
#endif
}

std::string go_executable(const std::string &configured_path)
{
	// WebCool's persistent administrator setting has highest priority. The old
	// environment variable remains supported so existing deployments upgrade
	// without breaking their startup scripts.
	if (executable_file(configured_path))
		return configured_path;
	const char *legacy = getenv("WEBCOOL_AI_GO");
	if (legacy != NULL && executable_file(legacy))
		return legacy;

	const char *goroot = getenv("GOROOT");
	if (goroot != NULL && *goroot != '\0') {
#ifdef _WIN32
		const std::string candidate =
		    std::string(goroot) + "\\bin\\go.exe";
#else
		const std::string candidate = std::string(goroot) + "/bin/go";
#endif
		if (executable_file(candidate))
			return candidate;
	}
	const std::string from_path = executable_from_search_path("go");
	if (!from_path.empty())
		return from_path;

#ifdef _WIN32
	const char *program_files = getenv("ProgramFiles");
	if (program_files != NULL && *program_files != '\0') {
		const std::string candidate =
		    std::string(program_files) + "\\Go\\bin\\go.exe";
		if (executable_file(candidate))
			return candidate;
	}
	const char *local_app_data = getenv("LOCALAPPDATA");
	if (local_app_data != NULL && *local_app_data != '\0') {
		const std::string candidate =
		    std::string(local_app_data) + "\\Programs\\Go\\bin\\go.exe";
		if (executable_file(candidate))
			return candidate;
	}
	return "";
#else
	const char *standard_paths[] = { "/usr/local/go/bin/go", "/usr/bin/go",
		"/usr/local/bin/go", "/opt/homebrew/bin/go",
		"/opt/homebrew/opt/go/libexec/bin/go",
		"/usr/local/opt/go/libexec/bin/go", "/snap/bin/go",
		"/usr/lib/go/bin/go" };
	return first_executable(
	    standard_paths, sizeof(standard_paths) / sizeof(standard_paths[0]));
#endif
}

bool has_relative_file(agent_workspace_t &workspace,
    const std::string &project_path, const std::string &relative)
{
	const size_t slash = relative.rfind('/');
	const std::string relative_parent =
	    slash == std::string::npos ? "" : relative.substr(0, slash);
	const std::string parent = project_path.empty() ?
	    relative_parent :
	    (relative_parent.empty() ? project_path :
	                               project_path + "/" + relative_parent);
	const std::string name =
	    slash == std::string::npos ? relative : relative.substr(slash + 1);
	std::vector<workspace_entry_t> entries;
	std::string ignored;
	return workspace.list(parent, entries, ignored) &&
	    has_name(entries, parent, name);
}

void add_language(
    std::vector<std::string> &languages, const std::string &language)
{
	if (std::find(languages.begin(), languages.end(), language) ==
	    languages.end())
		languages.push_back(language);
}

void add_fixed_command(project_toolchain_t &result, const std::string &id,
    const std::string &executable, const std::vector<std::string> &arguments)
{
	if (executable.empty()) {
		result.unavailable_tools.push_back(id);
		return;
	}
	sandbox_command_t command;
	command.id = id;
	command.executable = executable;
	command.fixed_arguments = arguments;
	command.allow_dynamic_arguments = false;
	result.commands.push_back(command);
}

void add_http_service_command(project_toolchain_t &result,
    const std::string &id, const std::string &executable,
    const std::vector<std::string> &arguments)
{
	add_fixed_command(result, id, executable, arguments);
	if (!result.commands.empty() && result.commands.back().id == id) {
		// The project opts in only through the fixed marker. Port/path and argv are
		// server policy, never model-controlled request parameters.
		result.commands.back().allow_loopback_network = true;
		result.commands.back().http_probe_port = 18080;
		result.commands.back().http_probe_path = "/";
	}
}

bool language_tool_enabled(const std::string &language)
{
	// This is a server-administrator policy, not a model or user preference.
	// Values are a validated comma-separated allowlist; package managers are
	// never enabled here. The durable policy defaults to Go and Java.
	if (ai_language_tool_enabled(language))
		return true;
	const char *configured = getenv("WEBCOOL_AI_LANGUAGE_TOOLS");
	if (configured == NULL)
		return false;
	const std::string value(configured);
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find(',', begin);
		std::string item = value.substr(begin,
		    end == std::string::npos ? std::string::npos : end - begin);
		while (!item.empty() && item[0] == ' ')
			item.erase(0, 1);
		while (!item.empty() && item[item.size() - 1] == ' ')
			item.resize(item.size() - 1);
		if (item == language)
			return true;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return false;
}

} // namespace toolchain_detail
using namespace toolchain_detail;

project_toolchain_catalog_t::project_toolchain_catalog_t(
    const std::string &user_root)
        : user_root_(user_root)
{
}

std::string discover_go_executable(const std::string &configured_path)
{
	return go_executable(configured_path);
}

std::string discover_language_executable(
    const std::string &name, const std::string &configured_path)
{
	if (name == "go")
		return go_executable(configured_path);
	if (executable_file(configured_path))
		return configured_path;
	if (name == "node") {
		const char *paths[] = { "/usr/bin/node", "/usr/local/bin/node",
			"/opt/homebrew/bin/node" };
		const std::string configured =
		    administrator_executable("WEBCOOL_AI_NODE", paths, 3);
		if (!configured.empty())
			return configured;
#ifdef _WIN32
		const std::string bundled =
		    windows_browser_runtime::bundled_node(
		        windows_browser_runtime::application_directory());
		if (!bundled.empty())
			return bundled;
		const std::string from_path =
		    executable_from_search_path("node");
		if (!from_path.empty())
			return from_path;
		const char *program_files = getenv("ProgramFiles");
		if (program_files && *program_files) {
			const std::string candidate =
			    std::string(program_files) + "\\nodejs\\node.exe";
			if (executable_file(candidate))
				return candidate;
		}
#endif
		return "";
	}
	if (name == "python") {
		const char *paths[] = { "/usr/bin/python3",
			"/usr/local/bin/python3", "/opt/homebrew/bin/python3" };
		return administrator_executable("WEBCOOL_AI_PYTHON", paths, 3);
	}
	if (name == "javac") {
		const char *paths[] = { "/usr/bin/javac",
			"/usr/local/bin/javac", "/opt/homebrew/bin/javac" };
		return administrator_executable("WEBCOOL_AI_JAVAC", paths, 3);
	}
	if (name == "java") {
		const char *paths[] = { "/usr/bin/java", "/usr/local/bin/java",
			"/opt/homebrew/bin/java" };
		return administrator_executable("WEBCOOL_AI_JAVA", paths, 3);
	}
	if (name == "cargo") {
		const char *paths[] = { "/usr/bin/cargo",
			"/usr/local/bin/cargo", "/opt/homebrew/bin/cargo" };
		return administrator_executable("WEBCOOL_AI_CARGO", paths, 3);
	}
	if (name == "make") {
		const char *paths[] = { "/usr/bin/make" };
		return administrator_executable("WEBCOOL_AI_MAKE", paths, 1);
	}
	if (name == "swift") {
		const char *paths[] = { "/usr/bin/swift",
			"/Applications/Xcode.app/Contents/Developer/Toolchains/"
			"XcodeDefault.xctoolchain/usr/bin/swift" };
		return administrator_executable("WEBCOOL_AI_SWIFT", paths, 2);
	}
	if (name == "dotnet") {
		const char *paths[] = { "/usr/bin/dotnet",
			"/usr/local/share/dotnet/dotnet",
			"/opt/homebrew/bin/dotnet" };
		return administrator_executable("WEBCOOL_AI_DOTNET", paths, 3);
	}
	if (name == "kotlinc") {
		const char *paths[] = { "/usr/bin/kotlinc",
			"/usr/local/bin/kotlinc", "/opt/homebrew/bin/kotlinc" };
		return administrator_executable("WEBCOOL_AI_KOTLINC", paths, 3);
	}
	if (name == "php") {
		const char *paths[] = { "/usr/bin/php", "/usr/local/bin/php",
			"/opt/homebrew/bin/php" };
		return administrator_executable("WEBCOOL_AI_PHP", paths, 3);
	}
	if (name == "dmd") {
		const char *paths[] = { "/usr/bin/dmd", "/usr/local/bin/dmd",
			"/opt/homebrew/bin/dmd" };
		return administrator_executable("WEBCOOL_AI_DMD", paths, 3);
	}
	return "";
}

bool project_toolchain_catalog_t::is_git_repository(
    const std::string &project_path, bool &result, std::string &err) const
{
	std::string normalized;
	if (!agent_workspace_t::normalize_path(
	        project_path, normalized, false, err)) {
		return ai_error(
		    "project.toolchain", "normalize-git-project", err);
	}
	// Only the .git marker type is inspected. Repository configuration, hooks,
	// credentials, refs and objects remain protected from this API.
	result = has_safe_git_marker(user_root_, normalized);
	return true;
}

bool browser_debug_available(std::string &reason)
{
	if (!program_sandbox_t::browser_runtime_available(reason))
		return false;
	const ai_admin_policy_t policy = ai_runtime_policy_get();
	if (!discover_language_executable("node", policy.node_executable_path)
	         .empty())
		return true;
	reason = "Node.js is missing";
	return false;
}

bool browser_debug_enabled(std::string &reason)
{
	if (ai_runtime_policy_get().allow_browser_debug)
		return browser_debug_available(reason);
	reason = "browser debugging is disabled by administrator";
	return false;
}

bool project_toolchain_catalog_t::discover(const std::string &project_path,
    project_toolchain_t &result, std::string &err) const
{
	result = project_toolchain_t();
	// Manifest detection is intentionally shallow. We inspect names in the
	// selected project root and never parse or execute build/package scripts.
	std::string normalized;
	if (!agent_workspace_t::normalize_path(
	        project_path, normalized, true, err)) {
		return ai_error("project.toolchain", "normalize-project", err);
	}
	const ai_admin_policy_t policy = ai_runtime_policy_get();
	result.functional_acceptance_enabled =
	    language_tool_enabled("javascript");
	result.browser_acceptance_enabled =
	    browser_debug_enabled(result.browser_unavailable_reason);
	std::string project_root;
	if (!agent_workspace_t::resolve_project_root(
	        user_root_, normalized, project_root, err)) {
		return ai_error(
		    "project.toolchain", "resolve-project-root", err);
	}
	agent_workspace_t workspace(user_root_);
	std::vector<workspace_entry_t> entries;
	if (!workspace.list(normalized, entries, err)) {
		return ai_error("project.toolchain", "list-manifests", err);
	}
	const bool cmake = has_name(entries, normalized, "CMakeLists.txt");
	const bool http_service =
	    has_name(entries, normalized, ".webcool-http-service");
	const bool make = has_name(entries, normalized, "Makefile") ||
	    has_name(entries, normalized, "makefile") ||
	    has_name(entries, normalized, "GNUmakefile");
	const bool objective_c =
	    has_relative_file(workspace, normalized, "src/main.m");
	const bool c_project =
	    has_relative_file(workspace, normalized, "src/main.c");
	if (objective_c)
		add_language(result.detected_languages, "objective-c");
	else if (cmake || make)
		add_language(
		    result.detected_languages, c_project ? "c" : "cpp");
	discover_javascript_commands(
	    workspace, normalized, entries, policy, result, http_service);
	discover_python_commands(
	    workspace, normalized, entries, policy, result, http_service);
	discover_php_commands(
	    workspace, normalized, entries, policy, result, http_service);
	discover_java_commands(
	    workspace, normalized, entries, policy, result, http_service);
	discover_rust_commands(
	    normalized, entries, policy, result, http_service);
	discover_d_commands(workspace, normalized, entries, policy, result,
	    project_root, http_service);
	discover_go_commands(
	    normalized, entries, policy, result, project_root, http_service);
	discover_swift_commands(
	    normalized, entries, policy, result, http_service);
	discover_csharp_commands(
	    workspace, normalized, entries, policy, result, http_service);
	discover_kotlin_commands(
	    workspace, normalized, policy, result, http_service);
	discover_objective_c_commands(
	    policy, result, project_root, http_service, objective_c);
	discover_git_commands(normalized, result, user_root_);
	if (!discover_cmake_commands(workspace, normalized, policy, result,
	        project_root, http_service, c_project, cmake, err))
		return false;
	discover_make_commands(policy, result, project_root, http_service,
	    objective_c, c_project, cmake, make);
	// Standard installations retain HTTP readiness without requiring browser
	// engines. Never report an HTTP-only check as successful browser acceptance.
	if (result.browser_acceptance_enabled) {
		const std::string node = discover_language_executable(
		    "node", policy.node_executable_path);
		for (auto &command : result.commands) {
			if (!command.http_probe_port)
				continue;
			command.browser_probe_node = node;
		}
	} else if (http_service) {
		result.unavailable_tools.push_back(policy.allow_browser_debug ?
		        "browser.acceptance.disabled-runtime-unavailable" :
		        "browser.acceptance.disabled-by-admin");
	}

	discover_acceptance_commands(workspace, normalized, policy, result);
	return true;
}

} // namespace ai
} // namespace webcool
