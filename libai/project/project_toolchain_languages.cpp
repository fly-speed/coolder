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
static void add_javascript_syntax_command(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result)
{
	const char *candidates[] = { "index.js", "main.js", "server.js",
		"src/main.js" };
	bool added = false;
	for (size_t i = 0; i < 4 && !added; ++i) {
		if (!has_relative_file(workspace, normalized, candidates[i]))
			continue;
		add_fixed_command(result, "javascript.syntax-check",
		    discover_language_executable(
		        "node", policy.node_executable_path),
		    std::vector<std::string>{ "--check", candidates[i] });
		added = true;
	}
	if (!added)
		result.unavailable_tools.push_back(
		    "javascript.no-fixed-entrypoint");
}

void discover_javascript_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	if (has_name(entries, normalized, "index.html")) {
		add_language(result.detected_languages, "html");
		if (has_relative_file(workspace, normalized, "script.js")
			&& !has_name(entries, normalized, "package.json")) {
			add_language(result.detected_languages, "javascript");
			if (language_tool_enabled("javascript")) {
				add_fixed_command(result, "javascript.syntax-check",
					discover_language_executable("node", policy.node_executable_path),
					std::vector<std::string>{"--check", "script.js"});
			}
		}
	}
	if (has_name(entries, normalized, "package.json")) {
		add_language(result.detected_languages, "javascript");
		result.unavailable_tools.push_back(
		    "javascript.package-manager-disabled");
		if (language_tool_enabled("javascript")) {
			add_javascript_syntax_command(
			    workspace, normalized, policy, result);
			if (has_relative_file(
			        workspace, normalized, "tests/main.test.js")) {
				add_fixed_command(result, "javascript.test",
				    discover_language_executable(
				        "node", policy.node_executable_path),
				    std::vector<std::string>{
				        "--test", "tests/main.test.js" });
			} else
				result.unavailable_tools.push_back(
				    "javascript.no-fixed-tests");
			if (http_service &&
			    has_relative_file(
			        workspace, normalized, "server.js")) {
				add_http_service_command(result,
				    "javascript.http-smoke",
				    discover_language_executable(
				        "node", policy.node_executable_path),
				    std::vector<std::string>{ "server.js" });
			}
		} else
			result.unavailable_tools.push_back(
			    "javascript.disabled-by-admin");
	}
}

void discover_python_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	if (has_name(entries, normalized, "pyproject.toml") ||
	    has_name(entries, normalized, "requirements.txt")) {
		add_language(result.detected_languages, "python");
		result.unavailable_tools.push_back(
		    "python.package-manager-disabled");
		if (language_tool_enabled("python")) {
			const std::string python = discover_language_executable(
			    "python", policy.python_executable_path);
			add_fixed_command(result, "python.syntax-check", python,
			    std::vector<std::string>{
			        "-m", "compileall", "-q", "." });
			if (has_relative_file(
			        workspace, normalized, "tests/test_main.py")) {
				add_fixed_command(result, "python.test", python,
				    std::vector<std::string>{ "-m", "unittest",
				        "-v", "tests/test_main.py" });
			} else
				result.unavailable_tools.push_back(
				    "python.no-fixed-tests");
			if (http_service &&
			    has_relative_file(
			        workspace, normalized, "server.py")) {
				add_http_service_command(result,
				    "python.http-smoke", python,
				    std::vector<std::string>{ "server.py" });
			}
		} else
			result.unavailable_tools.push_back(
			    "python.disabled-by-admin");
	}
}

static std::string find_php_entrypoint(
    agent_workspace_t &workspace, const std::string &normalized)
{
	const char *candidates[] = { "index.php", "public/index.php",
		"src/main.php" };
	std::string entrypoint;
	for (size_t i = 0; i < 3 && entrypoint.empty(); ++i) {
		if (!has_relative_file(workspace, normalized, candidates[i]))
			continue;
		entrypoint = candidates[i];
	}

	return entrypoint;
}

void discover_php_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
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
			const std::string entrypoint =
			    find_php_entrypoint(workspace, normalized);
			if (!entrypoint.empty()) {
				add_fixed_command(result, "php.syntax-check",
				    php,
				    std::vector<std::string>{
				        "-l", entrypoint });
			} else
				result.unavailable_tools.push_back(
				    "php.no-fixed-entrypoint");
			if (has_relative_file(
			        workspace, normalized, "tests/main_test.php")) {
				add_fixed_command(result, "php.test", php,
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
				add_http_service_command(result,
				    "php.http-smoke", php,
				    std::vector<std::string>{ "-S",
				        "127.0.0.1:18080", "-t",
				        document_root });
			}
		} else
			result.unavailable_tools.push_back(
			    "php.disabled-by-admin");
	}
}

void discover_java_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	if (has_name(entries, normalized, "pom.xml") ||
	    has_name(entries, normalized, "build.gradle") ||
	    has_name(entries, normalized, "build.gradle.kts") ||
	    has_relative_file(workspace, normalized, "src/Main.java")) {
		add_language(result.detected_languages, "java");
		if (language_tool_enabled("java") &&
		    (has_relative_file(
		         workspace, normalized, "src/Main.java") ||
		        has_name(entries, normalized, "Main.java"))) {
			const std::string source =
			    has_relative_file(
			        workspace, normalized, "src/Main.java") ?
			    "src/Main.java" :
			    "Main.java";
			add_fixed_command(result, "java.compile-main",
			    discover_language_executable(
			        "javac", policy.javac_executable_path),
			    std::vector<std::string>{ source });
			if (has_relative_file(
			        workspace, normalized, "tests/MainTest.java")) {
				// Compile both classes into the existing src directory.  Keeping a
				// single class-path root avoids the ':' versus ';' separator
				// difference between Unix and Windows Java runtimes.
				add_fixed_command(result, "java.compile-tests",
				    discover_language_executable(
				        "javac", policy.javac_executable_path),
				    std::vector<std::string>{ "-d", "src",
				        source, "tests/MainTest.java" });
				add_fixed_command(result, "java.test",
				    discover_language_executable(
				        "java", policy.java_executable_path),
				    std::vector<std::string>{
				        "-ea", "-cp", "src", "MainTest" });
			} else
				result.unavailable_tools.push_back(
				    "java.no-fixed-tests");
			if (http_service) {
				add_http_service_command(result,
				    "java.http-smoke",
				    discover_language_executable(
				        "java", policy.java_executable_path),
				    std::vector<std::string>{
				        "-cp", "src", "Main" });
			}
		} else
			result.unavailable_tools.push_back(
			    language_tool_enabled("java") ?
			        "java.no-fixed-entrypoint" :
			        "java.disabled-by-admin");
	}
}

void discover_rust_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	if (has_name(entries, normalized, "Cargo.toml")) {
		add_language(result.detected_languages, "rust");
		if (language_tool_enabled("rust")) {
			const std::string cargo = discover_language_executable(
			    "cargo", policy.cargo_executable_path);
			add_fixed_command(result, "rust.build", cargo,
			    std::vector<std::string>{ "build", "--offline",
			        "--target-dir", ".webcool-build/rust" });
			add_fixed_command(result, "rust.test", cargo,
			    std::vector<std::string>{ "test", "--offline",
			        "--target-dir", ".webcool-build/rust" });
			if (http_service) {
				add_http_service_command(result,
				    "rust.http-smoke", cargo,
				    std::vector<std::string>{ "run",
				        "--offline", "--target-dir",
				        ".webcool-build/rust" });
			}
		} else
			result.unavailable_tools.push_back(
			    "rust.disabled-by-admin");
	}
}

void discover_d_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service)
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
			    has_relative_file(
			        workspace, normalized, "source/app.d") ?
			    "source/app.d" :
			    (has_relative_file(
			         workspace, normalized, "src/main.d") ?
			            "src/main.d" :
			            "");
			if (!entrypoint.empty()) {
				add_fixed_command(result, "d.build", dmd,
				    std::vector<std::string>{
				        "-of=.webcool-d-app", entrypoint });
			} else
				result.unavailable_tools.push_back(
				    "d.no-fixed-entrypoint");
			if (has_relative_file(
			        workspace, normalized, "tests/main.d")) {
				add_fixed_command(result, "d.test", dmd,
				    std::vector<std::string>{
				        "-unittest", "-run", "tests/main.d" });
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
				add_http_service_command(result, "d.http-smoke",
				    service, std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
			    "d.disabled-by-admin");
	}
}

void discover_go_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service)
{
	if (has_name(entries, normalized, "go.mod")) {
		add_language(result.detected_languages, "go");
		if (language_tool_enabled("go")) {
			const std::string go_program =
			    discover_go_executable(policy.go_executable_path);
			// An explicit output name is required for the conventional ./src
			// main package: plain `go build ./...` tries to create a root-level
			// executable named "src", which collides with the source directory.
			add_fixed_command(result, "go.build", go_program,
			    std::vector<std::string>{
			        "build", "-o", ".webcool-go-app", "./src" });
			add_fixed_command(result, "go.test", go_program,
			    std::vector<std::string>{
			        "test", "-count=1", "./..." });
			if (http_service) {
				const std::string service =
				    project_root + "/.webcool-go-app";
				add_http_service_command(result,
				    "go.http-smoke", service,
				    std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
			    "go.disabled-by-admin");
	}
}

void discover_swift_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	if (has_name(entries, normalized, "Package.swift")) {
		add_language(result.detected_languages, "swift");
		if (language_tool_enabled("swift")) {
			const std::string swift = discover_language_executable(
			    "swift", policy.swift_executable_path);
			add_fixed_command(result, "swift.build", swift,
			    std::vector<std::string>{ "build", "--scratch-path",
			        ".webcool-build/swift" });
			add_fixed_command(result, "swift.test", swift,
			    std::vector<std::string>{ "test", "--scratch-path",
			        ".webcool-build/swift" });
			if (http_service) {
				add_http_service_command(result,
				    "swift.http-smoke", swift,
				    std::vector<std::string>{ "run",
				        "--skip-build", "--scratch-path",
				        ".webcool-build/swift" });
			}
		} else
			result.unavailable_tools.push_back(
			    "swift.disabled-by-admin");
	}
}

void discover_csharp_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service)
{
	std::string csharp_project;
	for (size_t i = 0; i < entries.size(); ++i) {
		const std::string &path = entries[i].path;
		if (!(csharp_project.empty() && !entries[i].directory &&
		        path.size() >= 7 &&
		        path.substr(path.size() - 7) == ".csproj"))
			continue;
		// list() returns paths relative to the user root; commands execute with
		// the project itself as cwd and therefore need a project-relative path.
		csharp_project = normalized.empty() ?
		    path :
		    path.substr(normalized.size() + 1);
	}
	if (!csharp_project.empty()) {
		add_language(result.detected_languages, "csharp");
		if (language_tool_enabled("csharp")) {
			const std::string dotnet = discover_language_executable(
			    "dotnet", policy.dotnet_executable_path);
			add_fixed_command(result, "csharp.build", dotnet,
			    std::vector<std::string>{ "build", csharp_project,
			        "-o", ".webcool-build/dotnet",
			        "-p:AssemblyName=webcool_app" });
			if (has_relative_file(
			        workspace, normalized, "tests/Tests.csproj")) {
				add_fixed_command(result, "csharp.test", dotnet,
				    std::vector<std::string>{ "run",
				        "--project", "tests/Tests.csproj",
				        "--configuration", "Debug" });
			} else
				result.unavailable_tools.push_back(
				    "csharp.no-fixed-tests");
			if (http_service) {
				add_http_service_command(result,
				    "csharp.http-smoke", dotnet,
				    std::vector<std::string>{
				        ".webcool-build/dotnet/webcool_app.dll" });
			}
		} else
			result.unavailable_tools.push_back(
			    "csharp.disabled-by-admin");
	}
}

void discover_kotlin_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, bool http_service)
{
	if (has_relative_file(workspace, normalized, "src/Main.kt")) {
		add_language(result.detected_languages, "kotlin");
		if (language_tool_enabled("kotlin")) {
			add_fixed_command(result, "kotlin.compile-main",
			    discover_language_executable(
			        "kotlinc", policy.kotlinc_executable_path),
			    std::vector<std::string>{ "src/Main.kt",
			        "-include-runtime", "-d",
			        ".webcool-kotlin-main.jar" });
			if (has_relative_file(
			        workspace, normalized, "tests/MainTest.kt")) {
				add_fixed_command(result,
				    "kotlin.compile-tests",
				    discover_language_executable("kotlinc",
				        policy.kotlinc_executable_path),
				    std::vector<std::string>{ "src/Main.kt",
				        "tests/MainTest.kt", "-include-runtime",
				        "-d", ".webcool-kotlin-tests.jar" });
				add_fixed_command(result, "kotlin.test",
				    discover_language_executable(
				        "java", policy.java_executable_path),
				    std::vector<std::string>{ "-cp",
				        ".webcool-kotlin-tests.jar",
				        "MainTest" });
			} else
				result.unavailable_tools.push_back(
				    "kotlin.no-fixed-tests");
			if (http_service) {
				add_http_service_command(result,
				    "kotlin.http-smoke",
				    discover_language_executable(
				        "java", policy.java_executable_path),
				    std::vector<std::string>{
				        "-jar", ".webcool-kotlin-main.jar" });
			}
		} else
			result.unavailable_tools.push_back(
			    "kotlin.disabled-by-admin");
	}
}

void discover_objective_c_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c)
{
	if (objective_c) {
		if (language_tool_enabled("objective-c")) {
			const std::string make_tool =
			    discover_language_executable(
			        "make", policy.make_executable_path);
			add_fixed_command(result, "objective-c.build",
			    make_tool, std::vector<std::string>{ "-j2" });
			add_fixed_command(result, "objective-c.test", make_tool,
			    std::vector<std::string>{ "test" });
			if (http_service) {
				const std::string service =
				    project_root + "/.webcool-objc-app";
				add_http_service_command(result,
				    "objective-c.http-smoke", service,
				    std::vector<std::string>());
			}
		} else
			result.unavailable_tools.push_back(
			    "objective-c.disabled-by-admin");
	}
}
}
}
}
