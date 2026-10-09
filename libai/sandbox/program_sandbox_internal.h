#pragma once
#include "stdafx.h"
#include "program_sandbox.h"
#include "../agent/ai_admin_policy.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits.h>
#include <sstream>

#ifdef _WIN32
#include "../common/platform_compat.h"
#include "../common/windows_browser_runtime.h"
#include <windows.h>
#else
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace webcool
{
namespace ai
{
namespace sandbox_detail
{

// These are broker protocol limits, separate from kernel resource limits.
// Arguments remain distinct argv elements and are never concatenated into a
// command string or interpreted by a shell.
extern const size_t kMaxArguments;
extern const size_t kMaxArgumentBytes;

bool path_is_within(const std::string &root, const std::string &candidate);

bool canonical_existing_directory(
    const std::string &path, std::string &canonical, std::string &err);

bool safe_argument(const std::string &value);

#ifndef _WIN32
void close_fd(int &fd);

bool ensure_sandbox_temp_directory(
    const std::string &workdir, std::string &temp_directory, std::string &err);

void append_pipe(int fd, std::string &output, size_t &total, size_t limit,
    bool &truncated, bool &open);

#ifdef __APPLE__
std::string adjacent_helper_path();

bool mac_process_tree_exceeds(pid_t root, unsigned long limit);
#elif defined(__linux__)
std::string adjacent_helper_path();

void cleanup_linux_cgroup(pid_t helper_pid);
#endif
#endif

#ifdef _WIN32
std::string adjacent_helper_path();

bool windows_absolute_path(const std::string &path);

void append_windows_quoted(std::wstring &command, const std::wstring &argument);

void append_windows_pipe(HANDLE pipe, std::string &output, size_t &total,
    size_t limit, bool &truncated);
#endif

}
}
}

namespace webcool
{
namespace ai
{
#if defined(__APPLE__) || defined(__linux__)
bool execute_posix_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_,
    const std::vector<std::string> &readonly_roots_,
    const sandbox_limits_t &limits_, const std::string &project_root,
    const std::string &workdir, const sandbox_command_t *command);
#endif
#if defined(_WIN32)
bool execute_windows_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_, const sandbox_limits_t &limits_,
    const std::string &project_root, const std::string &workdir,
    const sandbox_command_t *command);
#endif

}
}
