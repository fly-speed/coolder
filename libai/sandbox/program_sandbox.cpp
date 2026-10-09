#include "stdafx.h"
#include "program_sandbox_internal.h"
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
const size_t kMaxArguments = 64;
const size_t kMaxArgumentBytes = 32 * 1024;

bool path_is_within(const std::string &root, const std::string &candidate)
{
#ifdef _WIN32
	if (_stricmp(candidate.c_str(), root.c_str()) == 0)
		return true;
	return candidate.size() > root.size() &&
	    _strnicmp(candidate.c_str(), root.c_str(), root.size()) == 0 &&
	    (candidate[root.size()] == '/' || candidate[root.size()] == '\\');
#else
	return candidate == root ||
	    (candidate.size() > root.size() &&
	        candidate.compare(0, root.size(), root) == 0 &&
	        candidate[root.size()] == '/');
#endif
}

bool canonical_existing_directory(
    const std::string &path, std::string &canonical, std::string &err)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot encode sandbox directory path";
		return false;
	}
	const DWORD needed = GetFullPathNameW(wide.c_str(), 0, NULL, NULL);
	if (needed == 0 || needed > 32768) {
		err = "cannot resolve sandbox directory";
		return false;
	}
	std::vector<wchar_t> full(needed + 1, L'\0');
	if (GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()),
	        &full[0], NULL) == 0) {
		err = "cannot resolve sandbox directory";
		return false;
	}
	const DWORD attributes = GetFileAttributesW(&full[0]);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
	    !webcool_wide_to_utf8(&full[0], canonical)) {
		err =
		    "sandbox directory is missing, unsafe, or not a directory";
		return false;
	}
	while (canonical.size() > 3 &&
	    (canonical[canonical.size() - 1] == '/' ||
	        canonical[canonical.size() - 1] == '\\'))
		canonical.resize(canonical.size() - 1);
	return true;
#else
	char resolved[PATH_MAX];
	if (realpath(path.c_str(), resolved) == NULL) {
		err = std::string("cannot resolve sandbox directory: ") +
		    strerror(errno);
		return false;
	}
	struct stat st;
	if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
		err = "sandbox working directory is not a directory";
		return false;
	}
	canonical = resolved;
	return true;
#endif
}

bool safe_argument(const std::string &value)
{
	if (value.empty())
		return true;
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(value[i]);
		if (!(ch == 0 || ch == '\r' || ch == '\n' || ch < 32 ||
		        ch == 127))
			continue;
		return false;
	}
	return true;
}

#ifndef _WIN32
void close_fd(int &fd)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

bool ensure_sandbox_temp_directory(
    const std::string &workdir, std::string &temp_directory, std::string &err)
{
	temp_directory = workdir + "/.webcool-sandbox-tmp";
	struct stat st;
	if (lstat(temp_directory.c_str(), &st) != 0) {
		if (errno != ENOENT ||
		    mkdir(temp_directory.c_str(), 0700) != 0) {
			err =
			    std::string(
			        "cannot create sandbox temporary directory: ") +
			    strerror(errno);
			return false;
		}
		if (lstat(temp_directory.c_str(), &st) != 0) {
			err = "cannot inspect sandbox temporary directory";
			return false;
		}
	}
	// A fixed hidden child avoids treating the Go module root itself as TMPDIR.
	// Reject links and non-directories so an untrusted project cannot redirect
	// compiler scratch writes outside its own workspace.
	if (!(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
		return true;
	err = "sandbox temporary path is not a safe directory";
	return false;
}

void append_pipe(int fd, std::string &output, size_t &total, size_t limit,
    bool &truncated, bool &open)
{
	char buffer[8192];
	// Read one ready chunk only. A second read can suspend an ACL fiber
	// after the pipe is drained, preventing deadline/cancellation checks.
	{
		const ssize_t got = read(fd, buffer, sizeof(buffer));
		if (got > 0) {
			const size_t count = static_cast<size_t>(got);
			const size_t room = total < limit ? limit - total : 0;
			const size_t keep = std::min(room, count);
			if (keep > 0)
				output.append(buffer, keep);
			total += keep;
			if (!(keep < count))
				return;
			truncated = true;
			return;
		}
		if (got == 0)
			open = false;
		else if (errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR) {
			open = false;
		}
		return;
	}
}

#ifdef __APPLE__
std::string adjacent_helper_path()
{
	uint32_t size = 0;
	(void)_NSGetExecutablePath(NULL, &size);
	if (size == 0 || size > 64 * 1024)
		return "";
	std::vector<char> path(size + 1, '\0');
	if (_NSGetExecutablePath(&path[0], &size) != 0)
		return "";
	std::string executable(&path[0]);
	const size_t slash = executable.rfind('/');
	return slash == std::string::npos ?
	    "" :
	    executable.substr(0, slash + 1) + "webcool-sandbox-helper";
}

bool mac_process_tree_exceeds(pid_t root, unsigned long limit)
{
	std::vector<pid_t> pending(1, root);
	size_t visited = 0;
	while (!pending.empty()) {
		const pid_t parent = pending.back();
		pending.pop_back();
		if (++visited > limit)
			return true;
		pid_t children[256];
		const int count = proc_listchildpids(
		    parent, children, static_cast<int>(sizeof(children)));
		if (count <= 0)
			continue;
		const size_t child_count =
		    std::min<size_t>(static_cast<size_t>(count),
		        sizeof(children) / sizeof(children[0]));
		for (size_t i = 0; i < child_count; ++i) {
			if (!(children[i] > 0))
				continue;
			pending.push_back(children[i]);
		}
	}
	return false;
}
#elif defined(__linux__)
std::string adjacent_helper_path()
{
	char path[PATH_MAX + 1];
	const ssize_t length = readlink("/proc/self/exe", path, PATH_MAX);
	if (length <= 0 || length > PATH_MAX)
		return "";
	path[length] = '\0';
	const std::string executable(path);
	const size_t slash = executable.rfind('/');
	return slash == std::string::npos ?
	    "" :
	    executable.substr(0, slash + 1) + "webcool-sandbox-helper";
}

void cleanup_linux_cgroup(pid_t helper_pid)
{
	const std::string path = "/sys/fs/cgroup/webcool/run-" +
	    std::to_string(static_cast<unsigned long long>(helper_pid));
	const int kill_fd =
	    open((path + "/cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC);
	if (kill_fd >= 0) {
		const ssize_t written = write(kill_fd, "1\n", 2);
		(void)written;
		close(kill_fd);
	}
	(void)rmdir(path.c_str());
}
#endif
#endif

#ifdef _WIN32
std::string adjacent_helper_path()
{
	std::vector<wchar_t> path(32768, L'\0');
	const DWORD length =
	    GetModuleFileNameW(NULL, &path[0], static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size())
		return "";
	std::wstring executable(&path[0], length);
	const size_t slash = executable.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return "";
	const std::wstring helper =
	    executable.substr(0, slash + 1) + L"webcool-sandbox-helper.exe";
	std::string utf8;
	return webcool_wide_to_utf8(helper.c_str(), utf8) ? utf8 : "";
}

bool windows_absolute_path(const std::string &path)
{
	return path.size() >= 3 &&
	    ((path[0] >= 'A' && path[0] <= 'Z') ||
	        (path[0] >= 'a' && path[0] <= 'z')) &&
	    path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

void append_windows_quoted(std::wstring &command, const std::wstring &argument)
{
	if (!command.empty())
		command += L' ';
	command += L'"';
	size_t slashes = 0;
	for (size_t i = 0; i < argument.size(); ++i) {
		if (argument[i] == L'\\') {
			++slashes;
			continue;
		}
		if (argument[i] == L'"') {
			command.append(slashes * 2 + 1, L'\\');
			command += L'"';
			slashes = 0;
		} else {
			command.append(slashes, L'\\');
			slashes = 0;
			command += argument[i];
		}
	}
	command.append(slashes * 2, L'\\');
	command += L'"';
}

void append_windows_pipe(HANDLE pipe, std::string &output, size_t &total,
    size_t limit, bool &truncated)
{
	for (;;) {
		DWORD available = 0;
		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) ||
		    available == 0)
			return;
		char buffer[8192];
		DWORD read = 0;
		if (!ReadFile(pipe, buffer,
		        static_cast<DWORD>(
		            std::min<size_t>(sizeof(buffer), available)),
		        &read, NULL) ||
		    read == 0)
			return;
		const size_t room = total < limit ? limit - total : 0;
		const size_t keep = std::min<size_t>(room, read);
		if (keep > 0)
			output.append(buffer, keep);
		total += keep;
		if (!(keep < read))
			continue;
		truncated = true;
		return;
	}
}
#endif

}
using namespace sandbox_detail;
// namespace

#if defined(__APPLE__) || defined(__linux__)
static bool has_executable_browser(const glob_t &matches)
{
	bool executable = false;
	for (size_t i = 0; i < matches.gl_pathc; ++i) {
		struct stat st;
		if (!(stat(matches.gl_pathv[i], &st) == 0 &&
		        S_ISREG(st.st_mode) &&
		        access(matches.gl_pathv[i], X_OK) == 0))
			continue;
		executable = true;
	}

	return executable;
}

#endif

bool program_sandbox_t::browser_runtime_available(
    std::string &reason, const std::string &helper_executable)
{
	reason.clear();
#if defined(_WIN32)
	// Windows packages place browser assets beside webcool.exe. The browser
	// distribution does not depend on the separate project sandbox helper.
	std::wstring directory =
	    windows_browser_runtime::application_directory();
	if (!helper_executable.empty()) {
		std::wstring anchor;
		if (!webcool_utf8_path_to_wide(
		        helper_executable.c_str(), anchor)) {
			reason = "cannot encode browser installation path";
			return false;
		}
		const size_t slash = anchor.find_last_of(L"\\/");
		directory =
		    slash == std::wstring::npos ? L"" : anchor.substr(0, slash);
	}
	return windows_browser_runtime::available(directory, reason);
#elif defined(__APPLE__) || defined(__linux__)
	const std::string helper = helper_executable.empty() ?
	    adjacent_helper_path() :
	    helper_executable;
	char resolved[PATH_MAX];
	struct stat helper_st;
	if (helper.empty() || !realpath(helper.c_str(), resolved) ||
	    stat(resolved, &helper_st) != 0 || !S_ISREG(helper_st.st_mode) ||
	    access(resolved, X_OK) != 0) {
		reason = "sandbox helper is missing or not executable";
		return false;
	}
	const std::string host(resolved);
	const std::string browser =
	    host.substr(0, host.find_last_of('/')) + "/browser";
	for (const char *relative :
	    { "/browser_probe.cjs", "/node_modules/playwright/index.js",
	        "/node_modules/playwright-core/index.js" }) {
		struct stat st;
		const std::string file = browser + relative;
		if (!(stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
		        access(file.c_str(), R_OK) != 0))
			continue;
		reason =
		    "browser runner or Playwright dependencies are missing";
		return false;
	}
	// The default matrix runs Chromium headless shell and Firefox. WebKit is
	// optional and is checked by Playwright when a contract requests it.
#ifdef __APPLE__
	const char *engines[] = {
		"/chromium_headless_shell-*/chrome-headless-shell-mac-*/chrome-headless-shell",
		"/firefox-*/firefox/Nightly.app/Contents/MacOS/firefox"
	};
#else
	const char *engines[] = {
		"/chromium_headless_shell-*/chrome-linux*/headless_shell",
		"/firefox-*/firefox/firefox"
	};
#endif
	for (const char *engine : engines) {
		glob_t matches = {};
		const std::string pattern = browser + "/.browsers" + engine;
		bool executable = false;
		if (glob(pattern.c_str(), 0, NULL, &matches) == 0) {
			executable = has_executable_browser(matches);
		}
		globfree(&matches);
		if (executable)
			continue;
		reason =
		    "bundled Chromium headless shell or Firefox is missing";
		return false;
	}
	return true;
#else
	(void)helper_executable;
	reason = "browser validation is unsupported on this platform";
	return false;
#endif
}

program_sandbox_t::program_sandbox_t(const std::string &user_root,
    const std::string &project_path,
    const std::vector<sandbox_command_t> &commands,
    const sandbox_limits_t &limits, const std::string &helper_executable,
    const std::vector<std::string> &readonly_roots)
        : user_root_(user_root)
        , project_path_(project_path)
        , commands_(commands)
        , limits_(limits)
        , helper_executable_(helper_executable)
        , readonly_roots_(readonly_roots)
{
}

bool program_sandbox_t::resolve_request(const sandbox_request_t &request,
    std::string &project_root, std::string &workdir,
    const sandbox_command_t *&command, std::string &err) const
{
	command = NULL;
	std::string user_root;
	if (!canonical_existing_directory(user_root_, user_root, err))
		return false;
	if (user_root == "/") {
		err = "sandbox user root cannot be the filesystem root";
		return false;
	}
	std::string approved_project;
	if (!agent_workspace_t::resolve_project_root(
	        user_root_, project_path_, approved_project, err) ||
	    !canonical_existing_directory(
	        approved_project, project_root, err)) {
		if (!err.empty())
			return false;
		err = "sandbox project is not an approved directory";
		return false;
	}
	std::string relative_workdir;
	if (!agent_workspace_t::normalize_path(
	        request.working_directory, relative_workdir, true, err))
		return false;
	const std::string work_candidate = relative_workdir.empty() ?
	    project_root :
	    project_root + "/" + relative_workdir;
	if (!canonical_existing_directory(work_candidate, workdir, err) ||
	    !path_is_within(project_root, workdir)) {
		if (!err.empty())
			return false;
		err = "sandbox working directory leaves the project";
		return false;
	}
	for (size_t i = 0; i < commands_.size(); ++i) {
		if (!(commands_[i].id == request.command_id))
			continue;
		command = &commands_[i];
		break;
	}
	if (command == NULL) {
		err = "sandbox command is not enabled by policy";
		return false;
	}
	// Recheck at dispatch as well: a queued command may predate permission revocation.
	if (!command->browser_probe_node.empty() &&
	    !ai_runtime_policy_get().allow_browser_debug) {
		err = "browser debugging is disabled by administrator";
		return false;
	}
	if (command->id.empty() || command->executable.empty()
#ifdef _WIN32
	    || !windows_absolute_path(command->executable)
#else
	    || command->executable[0] != '/'
#endif
	) {
		err = "sandbox command policy requires an absolute executable";
		return false;
	}
#ifndef _WIN32
	struct stat executable_st;
	if (stat(command->executable.c_str(), &executable_st) != 0 ||
	    !S_ISREG(executable_st.st_mode) ||
	    access(command->executable.c_str(), X_OK) != 0) {
		err = "sandbox executable is missing or not executable";
		return false;
	}
#else
	std::wstring executable_wide;
	if (!webcool_utf8_path_to_wide(
	        command->executable.c_str(), executable_wide)) {
		err = "cannot encode sandbox executable path";
		return false;
	}
	const DWORD executable_attributes =
	    GetFileAttributesW(executable_wide.c_str());
	if (executable_attributes == INVALID_FILE_ATTRIBUTES ||
	    (executable_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	    (executable_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "sandbox executable is missing or unsafe";
		return false;
	}
#endif
	if (request.arguments.size() + command->fixed_arguments.size() >
	    kMaxArguments) {
		err = "sandbox argument count exceeds the policy limit";
		return false;
	}
	if (!command->allow_dynamic_arguments && !request.arguments.empty()) {
		err =
		    "sandbox command accepts only its server-defined arguments";
		return false;
	}
	if ((!command->browser_probe_node.empty() &&
	        (command->http_probe_port == 0 ||
	            command->browser_probe_node[0] != '/' ||
	            !safe_argument(command->browser_probe_node))) ||
	    (command->http_probe_port != 0 &&
	        !command->allow_loopback_network) ||
	    command->http_probe_path.empty() ||
	    command->http_probe_path[0] != '/' ||
	    command->http_probe_path.size() > 256 ||
	    !safe_argument(command->http_probe_path)) {
		err = "sandbox HTTP probe policy is invalid";
		return false;
	}
	size_t argument_bytes = 0;
	for (size_t group = 0; group < 2; ++group) {
		const std::vector<std::string> &arguments =
		    group == 0 ? command->fixed_arguments : request.arguments;
		for (size_t i = 0; i < arguments.size(); ++i) {
			if (!safe_argument(arguments[i])) {
				err =
				    "sandbox arguments cannot contain control characters";
				return false;
			}
			argument_bytes += arguments[i].size() + 1;
		}
	}
	if (argument_bytes > kMaxArgumentBytes) {
		err = "sandbox arguments exceed the policy size limit";
		return false;
	}
	if (!(limits_.timeout_ms < 100 || limits_.timeout_ms > 10 * 60 * 1000 ||
	        limits_.cpu_seconds < 1 || limits_.cpu_seconds > 10 * 60 ||
	        limits_.memory_bytes < 16ULL * 1024ULL * 1024ULL ||
	        limits_.memory_bytes > 16ULL * 1024ULL * 1024ULL * 1024ULL ||
	        limits_.process_count < 1 || limits_.process_count > 256 ||
	        limits_.open_files < 8 || limits_.open_files > 1024 ||
	        limits_.file_size_bytes > 1024ULL * 1024ULL * 1024ULL ||
	        limits_.output_bytes < 1024 ||
	        limits_.output_bytes > 16UL * 1024UL * 1024UL))
		return true;
	err = "sandbox resource limits are outside the safe range";
	return false;
}

bool program_sandbox_t::validate(
    const sandbox_request_t &request, std::string &err) const
{
	std::string project_root;
	std::string workdir;
	const sandbox_command_t *command = NULL;
	if (resolve_request(request, project_root, workdir, command, err))
		return true;
	return ai_error("program.sandbox", "validate-request", err);
}

bool program_sandbox_t::backend_available(std::string &reason)
{
#ifdef __APPLE__
	if (access("/usr/bin/sandbox-exec", X_OK) == 0)
		return true;
	reason = "macOS sandbox-exec is unavailable";
#elif defined(__linux__)
	if (access("/proc/self/ns/user", R_OK) == 0 &&
	    access("/proc/self/ns/mnt", R_OK) == 0 &&
	    access("/sys/fs/cgroup/cgroup.controllers", R_OK) == 0 &&
	    access("/sys/fs/cgroup/webcool", W_OK) == 0)
		return true;
	reason = "Linux sandbox requires namespaces and a delegated writable "
	         "/sys/fs/cgroup/webcool cgroup v2 directory";
#elif defined(_WIN32)
	// Restricted Token + Job Object containment is implemented in the helper,
	// but a shared WebCool service account still needs a per-WebCool-user
	// AppContainer/ACL capability before project code can be allowed to run.
	reason =
	    "Windows sandbox is fail-closed until per-user AppContainer ACL "
	    "confinement passes Windows integration tests";
#else
	reason = "this operating system has no WebCool sandbox backend";
#endif
	return false;
}

bool program_sandbox_t::execute(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel) const
{
	result = sandbox_result_t();
	std::string project_root;
	std::string workdir;
	const sandbox_command_t *command = NULL;
	if (!resolve_request(
	        request, project_root, workdir, command, result.error)) {
		return ai_error(
		    "program.sandbox", "resolve-request", result.error);
	}
	std::string unavailable;
	if (!backend_available(unavailable)) {
		result.error = unavailable;
		return ai_error(
		    "program.sandbox", "probe-backend", result.error);
	}
#if defined(_WIN32)
	return execute_windows_sandbox(request, result, cancel_requested,
	    should_cancel, user_root_, helper_executable_, limits_,
	    project_root, workdir, command);
#elif !defined(__APPLE__) && !defined(__linux__)
	result.error = "sandbox backend dispatch is unavailable";
	return ai_error("program.sandbox", "dispatch-backend", result.error);
#else
	return execute_posix_sandbox(request, result, cancel_requested,
	    should_cancel, user_root_, helper_executable_, readonly_roots_,
	    limits_, project_root, workdir, command);
#endif
}

} // namespace ai
} // namespace webcool
