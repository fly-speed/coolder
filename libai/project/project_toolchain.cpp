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

namespace webcool
{
namespace ai
{
namespace
{

bool has_name(const std::vector<workspace_entry_t> &entries,
	      const std::string &project_path, const std::string &name)
{
	const std::string expected =
		project_path.empty() ? name : project_path + "/" + name;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (!entries[i].directory && entries[i].path == expected)
			return true;
	}
	return false;
}

// Repository metadata is intentionally hidden from the model-facing workspace
// API. Git capability detection therefore checks only whether the protected
// .git marker exists; it never reads configuration, hooks, credentials or
// object data.
bool has_safe_git_marker(const std::string &user_root,
			 const std::string &project_path)
{
	std::string root;
	std::string ignored;
	if (!agent_workspace_t::resolve_project_root(user_root, project_path,
						     root, ignored))
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
		if (access(candidates[i], X_OK) == 0)
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
		std::string directory = value.substr(
			begin, end == std::string::npos ? std::string::npos :
							  end - begin);
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

std::string administrator_executable(const char *variable,
				     const char *const *unix_candidates,
				     size_t count)
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
			       (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ==
				       0 ?
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
		const std::string candidate = std::string(local_app_data) +
					      "\\Programs\\Go\\bin\\go.exe";
		if (executable_file(candidate))
			return candidate;
	}
	return "";
#else
	const char *standard_paths[] = { "/usr/local/go/bin/go",
					 "/usr/bin/go",
					 "/usr/local/bin/go",
					 "/opt/homebrew/bin/go",
					 "/opt/homebrew/opt/go/libexec/bin/go",
					 "/usr/local/opt/go/libexec/bin/go",
					 "/snap/bin/go",
					 "/usr/lib/go/bin/go" };
	return first_executable(standard_paths,
				sizeof(standard_paths) /
					sizeof(standard_paths[0]));
#endif
}

bool has_relative_file(agent_workspace_t &workspace,
		       const std::string &project_path,
		       const std::string &relative)
{
	const size_t slash = relative.rfind('/');
	const std::string relative_parent =
		slash == std::string::npos ? "" : relative.substr(0, slash);
	const std::string parent =
		project_path.empty() ?
			relative_parent :
			(relative_parent.empty() ?
				 project_path :
				 project_path + "/" + relative_parent);
	const std::string name = slash == std::string::npos ?
					 relative :
					 relative.substr(slash + 1);
	std::vector<workspace_entry_t> entries;
	std::string ignored;
	return workspace.list(parent, entries, ignored) &&
	       has_name(entries, parent, name);
}

void add_language(std::vector<std::string> &languages,
		  const std::string &language)
{
	if (std::find(languages.begin(), languages.end(), language) ==
	    languages.end())
		languages.push_back(language);
}

void add_fixed_command(project_toolchain_t &result, const std::string &id,
		       const std::string &executable,
		       const std::vector<std::string> &arguments)
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
			      const std::string &id,
			      const std::string &executable,
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
		std::string item = value.substr(
			begin, end == std::string::npos ? std::string::npos :
							  end - begin);
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

} // namespace

project_toolchain_catalog_t::project_toolchain_catalog_t(
	const std::string &user_root)
	: user_root_(user_root)
{
}

std::string discover_go_executable(const std::string &configured_path)
{
	return go_executable(configured_path);
}

std::string discover_language_executable(const std::string &name,
					 const std::string &configured_path)
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
				windows_browser_runtime::
					application_directory());
		if (!bundled.empty())
			return bundled;
		const std::string from_path =
			executable_from_search_path("node");
		if (!from_path.empty())
			return from_path;
		const char *program_files = getenv("ProgramFiles");
		if (program_files && *program_files) {
			const std::string candidate =
				std::string(program_files) +
				"\\nodejs\\node.exe";
			if (executable_file(candidate))
				return candidate;
		}
#endif
		return "";
	}
	if (name == "python") {
		const char *paths[] = { "/usr/bin/python3",
					"/usr/local/bin/python3",
					"/opt/homebrew/bin/python3" };
		return administrator_executable("WEBCOOL_AI_PYTHON", paths, 3);
	}
	if (name == "javac") {
		const char *paths[] = { "/usr/bin/javac",
					"/usr/local/bin/javac",
					"/opt/homebrew/bin/javac" };
		return administrator_executable("WEBCOOL_AI_JAVAC", paths, 3);
	}
	if (name == "java") {
		const char *paths[] = { "/usr/bin/java", "/usr/local/bin/java",
					"/opt/homebrew/bin/java" };
		return administrator_executable("WEBCOOL_AI_JAVA", paths, 3);
	}
	if (name == "cargo") {
		const char *paths[] = { "/usr/bin/cargo",
					"/usr/local/bin/cargo",
					"/opt/homebrew/bin/cargo" };
		return administrator_executable("WEBCOOL_AI_CARGO", paths, 3);
	}
	if (name == "make") {
		const char *paths[] = { "/usr/bin/make" };
		return administrator_executable("WEBCOOL_AI_MAKE", paths, 1);
	}
	if (name == "swift") {
		const char *paths[] = {
			"/usr/bin/swift",
			"/Applications/Xcode.app/Contents/Developer/Toolchains/"
			"XcodeDefault.xctoolchain/usr/bin/swift"
		};
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
					"/usr/local/bin/kotlinc",
					"/opt/homebrew/bin/kotlinc" };
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
	if (!agent_workspace_t::normalize_path(project_path, normalized, false,
					       err)) {
		return ai_error("project.toolchain", "normalize-git-project",
				err);
	}
	// Only the .git marker type is inspected. Repository configuration, hooks,
	// credentials, refs and objects remain protected from this API.
	result = has_safe_git_marker(user_root_, normalized);
	return true;
}

static void
discover_javascript_commands(agent_workspace_t &workspace,
			     const std::string &normalized,
			     const std::vector<workspace_entry_t> &entries,
			     const ai_admin_policy_t &policy,
			     project_toolchain_t &result, bool http_service)
{
	if (has_name(entries, normalized, "package.json")) {
		add_language(result.detected_languages, "javascript");
		result.unavailable_tools.push_back(
			"javascript.package-manager-disabled");
		if (language_tool_enabled("javascript")) {
			const char *candidates[] = { "index.js", "main.js",
						     "server.js",
						     "src/main.js" };
			bool added = false;
			for (size_t i = 0; i < 4 && !added; ++i) {
				if (!has_relative_file(workspace, normalized,
						       candidates[i]))
					continue;
				add_fixed_command(
					result, "javascript.syntax-check",
					discover_language_executable(
						"node",
						policy.node_executable_path),
					std::vector<std::string>{
						"--check", candidates[i] });
				added = true;
			}
			if (!added)
				result.unavailable_tools.push_back(
					"javascript.no-fixed-entrypoint");
			if (has_relative_file(workspace, normalized,
					      "tests/main.test.js")) {
				add_fixed_command(
					result, "javascript.test",
					discover_language_executable(
						"node",
						policy.node_executable_path),
					std::vector<std::string>{
						"--test",
						"tests/main.test.js" });
			} else
				result.unavailable_tools.push_back(
					"javascript.no-fixed-tests");
			if (http_service &&
			    has_relative_file(workspace, normalized,
					      "server.js")) {
				add_http_service_command(
					result, "javascript.http-smoke",
					discover_language_executable(
						"node",
						policy.node_executable_path),
					std::vector<std::string>{
						"server.js" });
			}
		} else
			result.unavailable_tools.push_back(
				"javascript.disabled-by-admin");
	}
}

static void
discover_python_commands(agent_workspace_t &workspace,
			 const std::string &normalized,
			 const std::vector<workspace_entry_t> &entries,
			 const ai_admin_policy_t &policy,
			 project_toolchain_t &result, bool http_service)
{
	if (has_name(entries, normalized, "pyproject.toml") ||
	    has_name(entries, normalized, "requirements.txt")) {
		add_language(result.detected_languages, "python");
		result.unavailable_tools.push_back(
			"python.package-manager-disabled");
		if (language_tool_enabled("python")) {
			const std::string python = discover_language_executable(
				"python", policy.python_executable_path);
			add_fixed_command(
				result, "python.syntax-check", python,
				std::vector<std::string>{ "-m", "compileall",
							  "-q", "." });
			if (has_relative_file(workspace, normalized,
					      "tests/test_main.py")) {
				add_fixed_command(
					result, "python.test", python,
					std::vector<std::string>{
						"-m", "unittest", "-v",
						"tests/test_main.py" });
			} else
				result.unavailable_tools.push_back(
					"python.no-fixed-tests");
			if (http_service &&
			    has_relative_file(workspace, normalized,
					      "server.py")) {
				add_http_service_command(
					result, "python.http-smoke", python,
					std::vector<std::string>{
						"server.py" });
			}
		} else
			result.unavailable_tools.push_back(
				"python.disabled-by-admin");
	}
}

static void discover_php_commands(agent_workspace_t &workspace,
				  const std::string &normalized,
				  const std::vector<workspace_entry_t> &entries,
				  const ai_admin_policy_t &policy,
				  project_toolchain_t &result,
				  bool http_service)
{
	if (has_name(entries, normalized, "composer.json") ||
	    has_name(entries, normalized, "index.php") ||
	    has_relative_file(workspace, normalized, "public/index.php") ||
	    has_relative_file(workspace, normalized, "src/main.php")) {
		add_language(result.detected_languages, "php");
		if (has_name(entries, normalized, "composer.json")) {
			result.unavailable_tools.push_back(
				"php.package-manager-disabled");
		}
		if (language_tool_enabled("php")) {
			const std::string php = discover_language_executable(
				"php", policy.php_executable_path);
			const char *candidates[] = { "index.php",
						     "public/index.php",
						     "src/main.php" };
			std::string entrypoint;
			for (size_t i = 0; i < 3 && entrypoint.empty(); ++i) {
				if (has_relative_file(workspace, normalized,
						      candidates[i])) {
					entrypoint = candidates[i];
				}
			}
			if (!entrypoint.empty()) {
				add_fixed_command(result, "php.syntax-check",
						  php,
						  std::vector<std::string>{
							  "-l", entrypoint });
			} else
				result.unavailable_tools.push_back(
					"php.no-fixed-entrypoint");
			if (has_relative_file(workspace, normalized,
					      "tests/main_test.php")) {
				add_fixed_command(
					result, "php.test", php,
					std::vector<std::string>{
						"tests/main_test.php" });
			} else
				result.unavailable_tools.push_back(
					"php.no-fixed-tests");
			if (http_service && !entrypoint.empty()) {
				const std::string document_root =
					entrypoint == "public/index.php" ?
						"public" :
						".";
				add_http_service_command(
					result, "php.http-smoke", php,
					std::vector<std::string>{
						"-S", "127.0.0.1:18080", "-t",
						document_root });
			}
		} else
			result.unavailable_tools.push_back(
				"php.disabled-by-admin");
	}
}

static void
discover_java_commands(agent_workspace_t &workspace,
		       const std::string &normalized,
		       const std::vector<workspace_entry_t> &entries,
		       const ai_admin_policy_t &policy,
		       project_toolchain_t &result, bool http_service)
{
	if (has_name(entries, normalized, "pom.xml") ||
	    has_name(entries, normalized, "build.gradle") ||
	    has_name(entries, normalized, "build.gradle.kts") ||
	    has_relative_file(workspace, normalized, "src/Main.java")) {
		add_language(result.detected_languages, "java");
		if (language_tool_enabled("java") &&
		    (has_relative_file(workspace, normalized,
				       "src/Main.java") ||
		     has_name(entries, normalized, "Main.java"))) {
			const std::string source =
				has_relative_file(workspace, normalized,
						  "src/Main.java") ?
					"src/Main.java" :
					"Main.java";
			add_fixed_command(
				result, "java.compile-main",
				discover_language_executable(
					"javac", policy.javac_executable_path),
				std::vector<std::string>{ source });
			if (has_relative_file(workspace, normalized,
					      "tests/MainTest.java")) {
				// Compile both classes into the existing src directory.  Keeping a
				// single class-path root avoids the ':' versus ';' separator
				// difference between Unix and Windows Java runtimes.
				add_fixed_command(
					result, "java.compile-tests",
					discover_language_executable(
						"javac",
						policy.javac_executable_path),
					std::vector<std::string>{
						"-d", "src", source,
						"tests/MainTest.java" });
				add_fixed_command(
					result, "java.test",
					discover_language_executable(
						"java",
						policy.java_executable_path),
					std::vector<std::string>{ "-ea", "-cp",
								  "src",
								  "MainTest" });
			} else
				result.unavailable_tools.push_back(
					"java.no-fixed-tests");
			if (http_service) {
				add_http_service_command(
					result, "java.http-smoke",
					discover_language_executable(
						"java",
						policy.java_executable_path),
					std::vector<std::string>{ "-cp", "src",
								  "Main" });
			}
		} else
			result.unavailable_tools.push_back(
				language_tool_enabled("java") ?
					"java.no-fixed-entrypoint" :
					"java.disabled-by-admin");
	}
}

static void
discover_rust_commands(const std::string &normalized,
		       const std::vector<workspace_entry_t> &entries,
		       const ai_admin_policy_t &policy,
		       project_toolchain_t &result, bool http_service)
{
	if (has_name(entries, normalized, "Cargo.toml")) {
		add_language(result.detected_languages, "rust");
		if (language_tool_enabled("rust")) {
			const std::string cargo = discover_language_executable(
				"cargo", policy.cargo_executable_path);
			add_fixed_command(result, "rust.build", cargo,
					  std::vector<std::string>{
						  "build", "--offline",
						  "--target-dir",
						  ".webcool-build/rust" });
			add_fixed_command(result, "rust.test", cargo,
					  std::vector<std::string>{
						  "test", "--offline",
						  "--target-dir",
						  ".webcool-build/rust" });
			if (http_service) {
				add_http_service_command(
					result, "rust.http-smoke", cargo,
					std::vector<std::string>{
						"run", "--offline",
						"--target-dir",
						".webcool-build/rust" });
			}
		} else
			result.unavailable_tools.push_back(
				"rust.disabled-by-admin");
	}
}

static void discover_d_commands(agent_workspace_t &workspace,
				const std::string &normalized,
				const std::vector<workspace_entry_t> &entries,
				const ai_admin_policy_t &policy,
				project_toolchain_t &result,
				const std::string &project_root,
				bool http_service)
{
	if (has_name(entries, normalized, "dub.json") ||
	    has_name(entries, normalized, "dub.sdl") ||
	    has_relative_file(workspace, normalized, "source/app.d") ||
	    has_relative_file(workspace, normalized, "src/main.d")) {
		add_language(result.detected_languages, "d");
		if (has_name(entries, normalized, "dub.json") ||
		    has_name(entries, normalized, "dub.sdl")) {
			result.unavailable_tools.push_back(
				"d.package-manager-disabled");
		}
		if (language_tool_enabled("d")) {
			const std::string dmd = discover_language_executable(
				"dmd", policy.dmd_executable_path);
			const std::string entrypoint =
				has_relative_file(workspace, normalized,
						  "source/app.d") ?
					"source/app.d" :
					(has_relative_file(workspace,
							   normalized,
							   "src/main.d") ?
						 "src/main.d" :
						 "");
			if (!entrypoint.empty()) {
				add_fixed_command(result, "d.build", dmd,
						  std::vector<std::string>{
							  "-of=.webcool-d-app",
							  entrypoint });
			} else
				result.unavailable_tools.push_back(
					"d.no-fixed-entrypoint");
			if (has_relative_file(workspace, normalized,
					      "tests/main.d")) {
				add_fixed_command(result, "d.test", dmd,
						  std::vector<std::string>{
							  "-unittest", "-run",
							  "tests/main.d" });
			} else
				result.unavailable_tools.push_back(
					"d.no-fixed-tests");
			if (http_service && !entrypoint.empty()) {
#ifdef _WIN32
				const std::string service_name =
					".webcool-d-app.exe";
#else
				const std::string service_name =
					".webcool-d-app";
#endif
				const std::string service =
					project_root + "/" + service_name;
				add_http_service_command(
					result, "d.http-smoke", service,
					std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
				"d.disabled-by-admin");
	}
}

static void discover_go_commands(const std::string &normalized,
				 const std::vector<workspace_entry_t> &entries,
				 const ai_admin_policy_t &policy,
				 project_toolchain_t &result,
				 const std::string &project_root,
				 bool http_service)
{
	if (has_name(entries, normalized, "go.mod")) {
		add_language(result.detected_languages, "go");
		if (language_tool_enabled("go")) {
			const std::string go_program = discover_go_executable(
				policy.go_executable_path);
			// An explicit output name is required for the conventional ./src
			// main package: plain `go build ./...` tries to create a root-level
			// executable named "src", which collides with the source directory.
			add_fixed_command(result, "go.build", go_program,
					  std::vector<std::string>{
						  "build", "-o",
						  ".webcool-go-app", "./src" });
			add_fixed_command(result, "go.test", go_program,
					  std::vector<std::string>{ "test",
								    "-count=1",
								    "./..." });
			if (http_service) {
				const std::string service =
					project_root + "/.webcool-go-app";
				add_http_service_command(
					result, "go.http-smoke", service,
					std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
				"go.disabled-by-admin");
	}
}

static void
discover_swift_commands(const std::string &normalized,
			const std::vector<workspace_entry_t> &entries,
			const ai_admin_policy_t &policy,
			project_toolchain_t &result, bool http_service)
{
	if (has_name(entries, normalized, "Package.swift")) {
		add_language(result.detected_languages, "swift");
		if (language_tool_enabled("swift")) {
			const std::string swift = discover_language_executable(
				"swift", policy.swift_executable_path);
			add_fixed_command(result, "swift.build", swift,
					  std::vector<std::string>{
						  "build", "--scratch-path",
						  ".webcool-build/swift" });
			add_fixed_command(result, "swift.test", swift,
					  std::vector<std::string>{
						  "test", "--scratch-path",
						  ".webcool-build/swift" });
			if (http_service) {
				add_http_service_command(
					result, "swift.http-smoke", swift,
					std::vector<std::string>{
						"run", "--skip-build",
						"--scratch-path",
						".webcool-build/swift" });
			}
		} else
			result.unavailable_tools.push_back(
				"swift.disabled-by-admin");
	}
}

static void
discover_csharp_commands(agent_workspace_t &workspace,
			 const std::string &normalized,
			 const std::vector<workspace_entry_t> &entries,
			 const ai_admin_policy_t &policy,
			 project_toolchain_t &result, bool http_service)
{
	std::string csharp_project;
	for (size_t i = 0; i < entries.size(); ++i) {
		const std::string &path = entries[i].path;
		if (csharp_project.empty() && !entries[i].directory &&
		    path.size() >= 7 &&
		    path.substr(path.size() - 7) == ".csproj") {
			// list() returns paths relative to the user root; commands execute with
			// the project itself as cwd and therefore need a project-relative path.
			csharp_project =
				normalized.empty() ?
					path :
					path.substr(normalized.size() + 1);
		}
	}
	if (!csharp_project.empty()) {
		add_language(result.detected_languages, "csharp");
		if (language_tool_enabled("csharp")) {
			const std::string dotnet = discover_language_executable(
				"dotnet", policy.dotnet_executable_path);
			add_fixed_command(
				result, "csharp.build", dotnet,
				std::vector<std::string>{
					"build", csharp_project, "-o",
					".webcool-build/dotnet",
					"-p:AssemblyName=webcool_app" });
			if (has_relative_file(workspace, normalized,
					      "tests/Tests.csproj")) {
				add_fixed_command(result, "csharp.test", dotnet,
						  std::vector<std::string>{
							  "run", "--project",
							  "tests/Tests.csproj",
							  "--configuration",
							  "Debug" });
			} else
				result.unavailable_tools.push_back(
					"csharp.no-fixed-tests");
			if (http_service) {
				add_http_service_command(
					result, "csharp.http-smoke", dotnet,
					std::vector<std::string>{
						".webcool-build/dotnet/webcool_app.dll" });
			}
		} else
			result.unavailable_tools.push_back(
				"csharp.disabled-by-admin");
	}
}

static void discover_kotlin_commands(agent_workspace_t &workspace,
				     const std::string &normalized,
				     const ai_admin_policy_t &policy,
				     project_toolchain_t &result,
				     bool http_service)
{
	if (has_relative_file(workspace, normalized, "src/Main.kt")) {
		add_language(result.detected_languages, "kotlin");
		if (language_tool_enabled("kotlin")) {
			add_fixed_command(
				result, "kotlin.compile-main",
				discover_language_executable(
					"kotlinc",
					policy.kotlinc_executable_path),
				std::vector<std::string>{
					"src/Main.kt", "-include-runtime", "-d",
					".webcool-kotlin-main.jar" });
			if (has_relative_file(workspace, normalized,
					      "tests/MainTest.kt")) {
				add_fixed_command(
					result, "kotlin.compile-tests",
					discover_language_executable(
						"kotlinc",
						policy.kotlinc_executable_path),
					std::vector<std::string>{
						"src/Main.kt",
						"tests/MainTest.kt",
						"-include-runtime", "-d",
						".webcool-kotlin-tests.jar" });
				add_fixed_command(
					result, "kotlin.test",
					discover_language_executable(
						"java",
						policy.java_executable_path),
					std::vector<std::string>{
						"-cp",
						".webcool-kotlin-tests.jar",
						"MainTest" });
			} else
				result.unavailable_tools.push_back(
					"kotlin.no-fixed-tests");
			if (http_service) {
				add_http_service_command(
					result, "kotlin.http-smoke",
					discover_language_executable(
						"java",
						policy.java_executable_path),
					std::vector<std::string>{
						"-jar",
						".webcool-kotlin-main.jar" });
			}
		} else
			result.unavailable_tools.push_back(
				"kotlin.disabled-by-admin");
	}
}

static void discover_objective_c_commands(const ai_admin_policy_t &policy,
					  project_toolchain_t &result,
					  const std::string &project_root,
					  bool http_service, bool objective_c)
{
	if (objective_c) {
		if (language_tool_enabled("objective-c")) {
			const std::string make_tool =
				discover_language_executable(
					"make", policy.make_executable_path);
			add_fixed_command(result, "objective-c.build",
					  make_tool,
					  std::vector<std::string>{ "-j2" });
			add_fixed_command(result, "objective-c.test", make_tool,
					  std::vector<std::string>{ "test" });
			if (http_service) {
				const std::string service =
					project_root + "/.webcool-objc-app";
				add_http_service_command(
					result, "objective-c.http-smoke",
					service, std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
				"objective-c.disabled-by-admin");
	}
}

static void discover_git_commands(const std::string &normalized,
				  project_toolchain_t &result,
				  const std::string &user_root_)
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
		const std::string git = administrator_executable(
			"WEBCOOL_AI_GIT", git_paths, 4);
#ifdef _WIN32
		const char *disabled_hooks_path = "NUL";
#else
		const char *disabled_hooks_path = "/dev/null";
#endif
		// Git is exposed only as fixed argv policies. No ref name, revision,
		// pathspec, hook, alias, pager, remote or arbitrary argument comes from
		// the browser or model.
		add_fixed_command(result, "git.status", git,
				  std::vector<std::string>{
					  "--no-pager", "-c",
					  "core.quotepath=false", "-c",
					  "core.fsmonitor=false", "-c",
					  std::string("core.hooksPath=") +
						  disabled_hooks_path,
					  "status", "--short", "--branch" });
		add_fixed_command(result, "git.diff", git,
				  std::vector<std::string>{
					  "--no-pager", "-c",
					  "core.quotepath=false", "-c",
					  "core.fsmonitor=false", "-c",
					  std::string("core.hooksPath=") +
						  disabled_hooks_path,
					  "diff", "--no-ext-diff",
					  "--no-textconv", "--" });
		add_fixed_command(result, "git.diff-staged", git,
				  std::vector<std::string>{
					  "--no-pager", "-c",
					  "core.quotepath=false", "-c",
					  "core.fsmonitor=false", "-c",
					  std::string("core.hooksPath=") +
						  disabled_hooks_path,
					  "diff", "--cached", "--no-ext-diff",
					  "--no-textconv", "--" });
		// The first command creates and switches to one deliberately fixed local
		// branch; it fails safely if that branch already exists. The checkpoint
		// command commits tracked changes only, so unknown/untracked files are
		// never swept into history unexpectedly.
		add_fixed_command(result, "git.branch-create", git,
				  std::vector<std::string>{
					  "--no-pager", "-c",
					  std::string("core.hooksPath=") +
						  disabled_hooks_path,
					  "checkout", "-b",
					  "webcool-agent-work" });
		add_fixed_command(
			result, "git.checkpoint-tracked", git,
			std::vector<std::string>{
				"--no-pager", "-c", "user.name=WebCool Agent",
				"-c", "user.email=webcool-agent@localhost",
				"-c",
				std::string("core.hooksPath=") +
					disabled_hooks_path,
				"commit", "-am", "WebCool agent checkpoint" });
	}
}

static bool discover_cmake_commands(agent_workspace_t &workspace,
				    const std::string &normalized,
				    const ai_admin_policy_t &policy,
				    project_toolchain_t &result,
				    const std::string &project_root,
				    bool http_service, bool c_project,
				    bool cmake, std::string &err)
{
	if (cmake) {
		// Search fixed system installation locations, never project-controlled PATH.
		const char *cmake_paths[] = { "/usr/bin/cmake",
					      "/opt/homebrew/bin/cmake",
					      "/usr/local/bin/cmake" };
		const char *ctest_paths[] = { "/usr/bin/ctest",
					      "/opt/homebrew/bin/ctest",
					      "/usr/local/bin/ctest" };
		const std::string native_prefix = c_project ? "c" : "cpp";
		std::vector<std::string> configure_args{
			"-S", ".", "-B", ".webcool-build",
			"-DCMAKE_BUILD_TYPE=Debug"
		};
#ifdef __APPLE__
		// Use installed compiler binaries directly: /usr/bin compiler/make shims
		// invoke xcrun and attempt to write outside the private build directory.
		const std::string xcode =
			"/Applications/Xcode.app/Contents/Developer";
		const std::string clt = "/Library/Developer/CommandLineTools";
		const bool full_xcode = executable_file(
			xcode +
			"/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++");
		const std::string developer = full_xcode ? xcode : clt;
		const std::string compiler_root =
			full_xcode ?
				developer +
					"/Toolchains/XcodeDefault.xctoolchain/usr/bin/" :
				developer + "/usr/bin/";
		if (executable_file(compiler_root + "clang++") &&
		    executable_file(developer + "/usr/bin/make")) {
			configure_args.push_back("-DCMAKE_C_COMPILER=" +
						 compiler_root + "clang");
			configure_args.push_back("-DCMAKE_CXX_COMPILER=" +
						 compiler_root + "clang++");
			configure_args.push_back("-DCMAKE_MAKE_PROGRAM=" +
						 developer + "/usr/bin/make");
			configure_args.push_back(
				"-DCMAKE_OSX_SYSROOT=" + developer +
				(full_xcode ?
					 "/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" :
					 "/SDKs/MacOSX.sdk"));
		}
#endif
		add_fixed_command(result, native_prefix + ".cmake.configure",
				  administrator_executable("WEBCOOL_AI_CMAKE",
							   cmake_paths, 3),
				  configure_args);
		add_fixed_command(
			result, native_prefix + ".cmake.build",
			administrator_executable("WEBCOOL_AI_CMAKE",
						 cmake_paths, 3),
			std::vector<std::string>{ "--build", ".webcool-build",
						  "--parallel", "2" });
		for (auto &command : result.commands) {
			if (command.id == native_prefix + ".cmake.configure" ||
			    command.id == native_prefix + ".cmake.build")
				command.allow_outbound_network =
					policy.allow_build_network;
		}
		add_fixed_command(result, native_prefix + ".ctest",
				  administrator_executable("WEBCOOL_AI_CTEST",
							   ctest_paths, 3),
				  std::vector<std::string>{
					  "--test-dir", ".webcool-build",
					  "--output-on-failure" });
		if (http_service) {
#ifdef _WIN32
			const std::string service_name = "webcool_app.exe";
#else
			const std::string service_name = "webcool_app";
#endif
			std::string service_relative =
				".webcool-build/" + service_name;
			std::string marker, marker_error;
			bool truncated = false;
			const std::string prefix =
				normalized.empty() ? "" : normalized + "/";
			if (workspace.read(prefix + ".webcool-http-service",
					   marker, truncated, marker_error) &&
			    !truncated) {
				const size_t first =
					marker.find_first_not_of(" \r\n\t");
				if (first != std::string::npos) {
					marker = marker.substr(
						first, marker.find_last_not_of(
							       " \r\n\t") -
							       first + 1);
					if (!agent_workspace_t::normalize_path(
						    marker, service_relative,
						    false, err))
						return false;
				} else {
					// Conservative compatibility for simple existing CMake projects.
					// Explicit marker paths handle variables, subdirectories and OUTPUT_NAME.
					std::string cmake_source;
					if (workspace.read(
						    prefix + "CMakeLists.txt",
						    cmake_source, truncated,
						    marker_error) &&
					    !truncated) {
						std::istringstream lines(
							cmake_source);
						std::string line, source;
						while (std::getline(lines,
								    line))
							source +=
								line.substr(
									0,
									line.find(
										'#')) +
								"\n";
						const std::regex target(
							"add_executable[ \t\r\n]*\\([ \t\r\n]*([A-Za-z0-9_.-]+)[ \t\r\n]",
							std::regex::icase);
						std::set<std::string>
							candidates;
						bool standard = false;
						for (std::sregex_iterator
							     it(source.begin(),
								source.end(),
								target),
						     end;
						     it != end; ++it) {
							const std::string name =
								(*it)[1];
							if (name ==
							    "webcool_app")
								standard = true;
							if (name == "server" ||
							    name == "http_server" ||
							    (name.size() > 4 &&
							     name.substr(
								     name.size() -
								     4) ==
								     "_web"))
								candidates.insert(
									name);
						}
						if (!standard &&
						    candidates.size() == 1) {
							service_relative =
								".webcool-build/" +
								*candidates
									 .begin();
#ifdef _WIN32
							service_relative +=
								".exe";
#endif
						}
					}
				}
			}
			const std::string service =
				project_root + "/" + service_relative;
			add_http_service_command(
				result, native_prefix + ".http-smoke", service,
				std::vector<std::string>());
		}
	}
	return true;
}

static void discover_make_commands(const ai_admin_policy_t &policy,
				   project_toolchain_t &result,
				   const std::string &project_root,
				   bool http_service, bool objective_c,
				   bool c_project, bool cmake, bool make)
{
	if (make && !objective_c) {
		add_fixed_command(result,
				  (c_project ? "c" : "cpp") +
					  std::string(".make"),
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
			add_http_service_command(
				result,
				(c_project ? "c" : "cpp") +
					std::string(".http-smoke"),
				service, std::vector<std::string>());
		}
	}
}

static void discover_acceptance_commands(agent_workspace_t &workspace,
					 const std::string &normalized,
					 const ai_admin_policy_t &policy,
					 project_toolchain_t &result)
{
	// A language-independent functional acceptance contract. Projects may use
	// Node's test runner to start their server and assert HTTP/browser behavior.
	// This remains an administrator-enabled fixed command inside the sandbox.
	if (has_relative_file(workspace, normalized,
			      "tests/acceptance.test.cjs")) {
		if (language_tool_enabled("javascript")) {
			add_fixed_command(
				result, "functional.acceptance",
				discover_language_executable(
					"node", policy.node_executable_path),
				std::vector<std::string>{
					"--test",
					"tests/acceptance.test.cjs" });
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

bool browser_debug_available(std::string &reason)
{
	if (!program_sandbox_t::browser_runtime_available(reason))
		return false;
	const ai_admin_policy_t policy = ai_runtime_policy_get();
	if (discover_language_executable("node", policy.node_executable_path)
		    .empty()) {
		reason = "Node.js is missing";
		return false;
	}
	return true;
}

bool browser_debug_enabled(std::string &reason)
{
	if (!ai_runtime_policy_get().allow_browser_debug) {
		reason = "browser debugging is disabled by administrator";
		return false;
	}
	return browser_debug_available(reason);
}

bool project_toolchain_catalog_t::discover(const std::string &project_path,
					   project_toolchain_t &result,
					   std::string &err) const
{
	result = project_toolchain_t();
	// Manifest detection is intentionally shallow. We inspect names in the
	// selected project root and never parse or execute build/package scripts.
	std::string normalized;
	if (!agent_workspace_t::normalize_path(project_path, normalized, true,
					       err)) {
		return ai_error("project.toolchain", "normalize-project", err);
	}
	const ai_admin_policy_t policy = ai_runtime_policy_get();
	result.functional_acceptance_enabled =
		language_tool_enabled("javascript");
	result.browser_acceptance_enabled =
		browser_debug_enabled(result.browser_unavailable_reason);
	std::string project_root;
	if (!agent_workspace_t::resolve_project_root(user_root_, normalized,
						     project_root, err)) {
		return ai_error("project.toolchain", "resolve-project-root",
				err);
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
		add_language(result.detected_languages,
			     c_project ? "c" : "cpp");
	discover_javascript_commands(workspace, normalized, entries, policy,
				     result, http_service);
	discover_python_commands(workspace, normalized, entries, policy, result,
				 http_service);
	discover_php_commands(workspace, normalized, entries, policy, result,
			      http_service);
	discover_java_commands(workspace, normalized, entries, policy, result,
			       http_service);
	discover_rust_commands(normalized, entries, policy, result,
			       http_service);
	discover_d_commands(workspace, normalized, entries, policy, result,
			    project_root, http_service);
	discover_go_commands(normalized, entries, policy, result, project_root,
			     http_service);
	discover_swift_commands(normalized, entries, policy, result,
				http_service);
	discover_csharp_commands(workspace, normalized, entries, policy, result,
				 http_service);
	discover_kotlin_commands(workspace, normalized, policy, result,
				 http_service);
	discover_objective_c_commands(policy, result, project_root,
				      http_service, objective_c);
	discover_git_commands(normalized, result, user_root_);
	if (!discover_cmake_commands(workspace, normalized, policy, result,
				     project_root, http_service, c_project,
				     cmake, err))
		return false;
	discover_make_commands(policy, result, project_root, http_service,
			       objective_c, c_project, cmake, make);
	// Standard installations retain HTTP readiness without requiring browser
	// engines. Never report an HTTP-only check as successful browser acceptance.
	if (result.browser_acceptance_enabled) {
		const std::string node = discover_language_executable(
			"node", policy.node_executable_path);
		for (auto &command : result.commands) {
			if (command.http_probe_port)
				command.browser_probe_node = node;
		}
	} else if (http_service) {
		result.unavailable_tools.push_back(
			policy.allow_browser_debug ?
				"browser.acceptance.disabled-runtime-unavailable" :
				"browser.acceptance.disabled-by-admin");
	}

	discover_acceptance_commands(workspace, normalized, policy, result);
	return true;
}

} // namespace ai
} // namespace webcool
