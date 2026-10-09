#pragma once
#include "project_toolchain.h"
#include "../workspace/agent_workspace.h"
#include "../agent/ai_admin_policy.h"
namespace webcool
{
namespace ai
{
namespace toolchain_detail
{
// Check whether the directory listing contains the named project entry.
bool has_name(const std::vector<workspace_entry_t> &entries,
    const std::string &project_path, const std::string &name);
// Validate the Git marker without following an unsafe workspace link.
bool has_safe_git_marker(
    const std::string &user_root, const std::string &project_path);
// Select the first available executable from the configured candidates.
std::string first_executable(const char *const *candidates, size_t count);
// Check whether a path names an executable regular file.
bool executable_file(const std::string &path);
// Return the storage path used for executable from search.
std::string executable_from_search_path(const std::string &name);
// Resolve an administrator override or an allowed executable candidate.
std::string administrator_executable(
    const char *variable, const char *const *unix_candidates, size_t count);
// Resolve the Go compiler from configuration and supported installation
// paths.
std::string go_executable(const std::string &configured_path);
// Check for a readable project-relative file through workspace access rules.
bool has_relative_file(agent_workspace_t &workspace,
    const std::string &project_path, const std::string &relative);
// Add a detected language without duplicating an earlier entry.
void add_language(
    std::vector<std::string> &languages, const std::string &language);
// Register a command with server-selected executable and arguments.
void add_fixed_command(project_toolchain_t &result, const std::string &id,
    const std::string &executable, const std::vector<std::string> &arguments);
// Register a service command with its readiness probe settings.
void add_http_service_command(project_toolchain_t &result,
    const std::string &id, const std::string &executable,
    const std::vector<std::string> &arguments);
// Check administrator policy before exposing a language toolchain.
bool language_tool_enabled(const std::string &language);
// Discover permitted fixed javascript commands for the selected project.
void discover_javascript_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed python commands for the selected project.
void discover_python_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed php commands for the selected project.
void discover_php_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed java commands for the selected project.
void discover_java_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed rust commands for the selected project.
void discover_rust_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed d commands for the selected project.
void discover_d_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service);
// Discover permitted fixed go commands for the selected project.
void discover_go_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service);
// Discover permitted fixed swift commands for the selected project.
void discover_swift_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed csharp commands for the selected project.
void discover_csharp_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
// Discover permitted fixed kotlin commands for the selected project.
void discover_kotlin_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, bool http_service);
// Discover permitted fixed objective c commands for the selected project.
void discover_objective_c_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c);
// Discover permitted fixed git commands for the selected project.
void discover_git_commands(const std::string &normalized,
    project_toolchain_t &result, const std::string &user_root_);
// Discover permitted fixed cmake commands for the selected project.
bool discover_cmake_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool c_project, bool cmake, std::string &err);
// Discover permitted fixed make commands for the selected project.
void discover_make_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c, bool c_project, bool cmake, bool make);
// Discover permitted fixed acceptance commands for the selected project.
void discover_acceptance_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result);
}
}
}
