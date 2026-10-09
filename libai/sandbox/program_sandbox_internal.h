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
// Upper bound for argument bytes.
extern const size_t kMaxArgumentBytes;

// Check whether a path lies inside the specified filesystem root.
bool path_is_within(const std::string &root, const std::string &candidate);

// Canonicalize a directory and report invalid or inaccessible paths.
bool canonical_existing_directory(
    const std::string &path, std::string &canonical, std::string &err);

// Reject arguments that violate the broker's size or text constraints.
bool safe_argument(const std::string &value);

#ifndef _WIN32
// Close an open descriptor and reset it to the invalid sentinel.
void close_fd(int &fd);

// Create or validate the private temporary directory for this sandbox run.
bool ensure_sandbox_temp_directory(
    const std::string &workdir, std::string &temp_directory, std::string &err);

// Drain available POSIX pipe bytes while enforcing the shared output limit.
void append_pipe(int fd, std::string &output, size_t &total, size_t limit,
    bool &truncated, bool &open);

#ifdef __APPLE__
// Return the storage path used for adjacent helper.
std::string adjacent_helper_path();

// Check whether the macOS process tree exceeds the configured count.
bool mac_process_tree_exceeds(pid_t root, unsigned long limit);
#elif defined(__linux__)
// Return the storage path used for adjacent helper.
std::string adjacent_helper_path();

// Remove the completed helper's Linux process-accounting group.
void cleanup_linux_cgroup(pid_t helper_pid);
#endif
#endif

#ifdef _WIN32
// Return the storage path used for adjacent helper.
std::string adjacent_helper_path();

// Return the storage path used for windows absolute.
bool windows_absolute_path(const std::string &path);

// Append an argv element using Windows command-line escaping rules.
void append_windows_quoted(std::wstring &command, const std::wstring &argument);

// Drain available Windows pipe bytes within the shared output budget.
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
// Launch and supervise the isolated POSIX helper with cancellation support.
bool execute_posix_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_,
    const std::vector<std::string> &readonly_roots_,
    const sandbox_limits_t &limits_, const std::string &project_root,
    const std::string &workdir, const sandbox_command_t *command);
#endif
#if defined(_WIN32)
// Launch and supervise the restricted Windows process and capture its output.
bool execute_windows_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_, const sandbox_limits_t &limits_,
    const std::string &project_root, const std::string &workdir,
    const sandbox_command_t *command);
#endif

}
}
