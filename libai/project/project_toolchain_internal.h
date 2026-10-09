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
bool has_name(const std::vector<workspace_entry_t> &entries,
    const std::string &project_path, const std::string &name);
bool has_safe_git_marker(
    const std::string &user_root, const std::string &project_path);
std::string first_executable(const char *const *candidates, size_t count);
bool executable_file(const std::string &path);
std::string executable_from_search_path(const std::string &name);
std::string administrator_executable(
    const char *variable, const char *const *unix_candidates, size_t count);
std::string go_executable(const std::string &configured_path);
bool has_relative_file(agent_workspace_t &workspace,
    const std::string &project_path, const std::string &relative);
void add_language(
    std::vector<std::string> &languages, const std::string &language);
void add_fixed_command(project_toolchain_t &result, const std::string &id,
    const std::string &executable, const std::vector<std::string> &arguments);
void add_http_service_command(project_toolchain_t &result,
    const std::string &id, const std::string &executable,
    const std::vector<std::string> &arguments);
bool language_tool_enabled(const std::string &language);
void discover_javascript_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_python_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_php_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_java_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_rust_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_d_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service);
void discover_go_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    const std::string &project_root, bool http_service);
void discover_swift_commands(const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_csharp_commands(agent_workspace_t &workspace,
    const std::string &normalized,
    const std::vector<workspace_entry_t> &entries,
    const ai_admin_policy_t &policy, project_toolchain_t &result,
    bool http_service);
void discover_kotlin_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, bool http_service);
void discover_objective_c_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c);
void discover_git_commands(const std::string &normalized,
    project_toolchain_t &result, const std::string &user_root_);
bool discover_cmake_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool c_project, bool cmake, std::string &err);
void discover_make_commands(const ai_admin_policy_t &policy,
    project_toolchain_t &result, const std::string &project_root,
    bool http_service, bool objective_c, bool c_project, bool cmake, bool make);
void discover_acceptance_commands(agent_workspace_t &workspace,
    const std::string &normalized, const ai_admin_policy_t &policy,
    project_toolchain_t &result);
}
}
}
