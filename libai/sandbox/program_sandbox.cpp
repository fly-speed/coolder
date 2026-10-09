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
namespace
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
	       (candidate[root.size()] == '/' ||
		candidate[root.size()] == '\\');
#else
	return candidate == root ||
	       (candidate.size() > root.size() &&
		candidate.compare(0, root.size(), root) == 0 &&
		candidate[root.size()] == '/');
#endif
}

bool canonical_existing_directory(const std::string &path,
				  std::string &canonical, std::string &err)
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
		err = "sandbox directory is missing, unsafe, or not a directory";
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
		if (ch == 0 || ch == '\r' || ch == '\n' || ch < 32 ||
		    ch == 127) {
			return false;
		}
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

bool ensure_sandbox_temp_directory(const std::string &workdir,
				   std::string &temp_directory,
				   std::string &err)
{
	temp_directory = workdir + "/.webcool-sandbox-tmp";
	struct stat st;
	if (lstat(temp_directory.c_str(), &st) != 0) {
		if (errno != ENOENT ||
		    mkdir(temp_directory.c_str(), 0700) != 0) {
			err = std::string(
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
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
		err = "sandbox temporary path is not a safe directory";
		return false;
	}
	return true;
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
			if (keep < count) {
				truncated = true;
				return;
			}
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
	return slash == std::string::npos ? "" :
					    executable.substr(0, slash + 1) +
						    "webcool-sandbox-helper";
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
		const size_t child_count = std::min<size_t>(
			static_cast<size_t>(count),
			sizeof(children) / sizeof(children[0]));
		for (size_t i = 0; i < child_count; ++i) {
			if (children[i] > 0)
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
	return slash == std::string::npos ? "" :
					    executable.substr(0, slash + 1) +
						    "webcool-sandbox-helper";
}

void cleanup_linux_cgroup(pid_t helper_pid)
{
	const std::string path =
		"/sys/fs/cgroup/webcool/run-" +
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
	const DWORD length = GetModuleFileNameW(
		NULL, &path[0], static_cast<DWORD>(path.size()));
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
			      static_cast<DWORD>(std::min<size_t>(
				      sizeof(buffer), available)),
			      &read, NULL) ||
		    read == 0)
			return;
		const size_t room = total < limit ? limit - total : 0;
		const size_t keep = std::min<size_t>(room, read);
		if (keep > 0)
			output.append(buffer, keep);
		total += keep;
		if (keep < read) {
			truncated = true;
			return;
		}
	}
}
#endif

} // namespace

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
		if (!webcool_utf8_path_to_wide(helper_executable.c_str(),
					       anchor)) {
			reason = "cannot encode browser installation path";
			return false;
		}
		const size_t slash = anchor.find_last_of(L"\\/");
		directory = slash == std::wstring::npos ?
				    L"" :
				    anchor.substr(0, slash);
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
		if (stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
		    access(file.c_str(), R_OK) != 0) {
			reason =
				"browser runner or Playwright dependencies are missing";
			return false;
		}
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
			for (size_t i = 0; i < matches.gl_pathc; ++i) {
				struct stat st;
				if (stat(matches.gl_pathv[i], &st) == 0 &&
				    S_ISREG(st.st_mode) &&
				    access(matches.gl_pathv[i], X_OK) == 0)
					executable = true;
			}
		}
		globfree(&matches);
		if (!executable) {
			reason =
				"bundled Chromium headless shell or Firefox is missing";
			return false;
		}
	}
	return true;
#else
	(void)helper_executable;
	reason = "browser validation is unsupported on this platform";
	return false;
#endif
}

program_sandbox_t::program_sandbox_t(
	const std::string &user_root, const std::string &project_path,
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
					std::string &project_root,
					std::string &workdir,
					const sandbox_command_t *&command,
					std::string &err) const
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
	if (!agent_workspace_t::resolve_project_root(user_root_, project_path_,
						     approved_project, err) ||
	    !canonical_existing_directory(approved_project, project_root,
					  err)) {
		if (err.empty())
			err = "sandbox project is not an approved directory";
		return false;
	}
	std::string relative_workdir;
	if (!agent_workspace_t::normalize_path(request.working_directory,
					       relative_workdir, true, err))
		return false;
	const std::string work_candidate =
		relative_workdir.empty() ?
			project_root :
			project_root + "/" + relative_workdir;
	if (!canonical_existing_directory(work_candidate, workdir, err) ||
	    !path_is_within(project_root, workdir)) {
		if (err.empty())
			err = "sandbox working directory leaves the project";
		return false;
	}
	for (size_t i = 0; i < commands_.size(); ++i) {
		if (commands_[i].id == request.command_id) {
			command = &commands_[i];
			break;
		}
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
	if (!webcool_utf8_path_to_wide(command->executable.c_str(),
				       executable_wide)) {
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
		err = "sandbox command accepts only its server-defined arguments";
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
			group == 0 ? command->fixed_arguments :
				     request.arguments;
		for (size_t i = 0; i < arguments.size(); ++i) {
			if (!safe_argument(arguments[i])) {
				err = "sandbox arguments cannot contain control characters";
				return false;
			}
			argument_bytes += arguments[i].size() + 1;
		}
	}
	if (argument_bytes > kMaxArgumentBytes) {
		err = "sandbox arguments exceed the policy size limit";
		return false;
	}
	if (limits_.timeout_ms < 100 || limits_.timeout_ms > 10 * 60 * 1000 ||
	    limits_.cpu_seconds < 1 || limits_.cpu_seconds > 10 * 60 ||
	    limits_.memory_bytes < 16ULL * 1024ULL * 1024ULL ||
	    limits_.memory_bytes > 16ULL * 1024ULL * 1024ULL * 1024ULL ||
	    limits_.process_count < 1 || limits_.process_count > 256 ||
	    limits_.open_files < 8 || limits_.open_files > 1024 ||
	    limits_.file_size_bytes > 1024ULL * 1024ULL * 1024ULL ||
	    limits_.output_bytes < 1024 ||
	    limits_.output_bytes > 16UL * 1024UL * 1024UL) {
		err = "sandbox resource limits are outside the safe range";
		return false;
	}
	return true;
}

bool program_sandbox_t::validate(const sandbox_request_t &request,
				 std::string &err) const
{
	std::string project_root;
	std::string workdir;
	const sandbox_command_t *command = NULL;
	if (!resolve_request(request, project_root, workdir, command, err)) {
		return ai_error("program.sandbox", "validate-request", err);
	}
	return true;
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

#if defined(__APPLE__) || defined(__linux__)
#ifdef __APPLE__
static void kill_sandbox_descendants(pid_t root, unsigned depth = 0)
{
	if (depth > 32)
		return;
	pid_t children[512];
	const int count = proc_listchildpids(root, children, sizeof(children));
	for (int i = 0; i < count && i < 512; ++i) {
		if (children[i] <= 0)
			continue;
		(void)kill(children[i], SIGSTOP);
		kill_sandbox_descendants(children[i], depth + 1);
		(void)kill(children[i], SIGKILL);
	}
}
#endif
static bool monitor_posix_sandbox(
	sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
	const std::function<bool()> &should_cancel,
	const sandbox_limits_t &limits_, pid_t child, int *stdout_pipe,
	int *stderr_pipe, const std::chrono::steady_clock::time_point &started,
	const std::string &temp_directory)
{
	// From this point onward execute() returns a result even for a non-zero child
	// exit. A build/test failure is command output, not a broker launch failure.
	close_fd(stdout_pipe[1]);
	close_fd(stderr_pipe[1]);
	(void)fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
	bool stdout_open = true;
	bool stderr_open = true;
	bool exited = false;
	bool killed = false;
	bool memory_limit_exceeded = false;
	bool process_limit_exceeded = false;
	int status = 0;
	size_t output_bytes = 0;
	while (!exited || stdout_open || stderr_open) {
		// Native poll(2) ignores entries whose fd is negative, but ACL's hooked
		// fiber poll treats a negative descriptor as a fatal programming error.
		// stdout and stderr can close at different times (Go builds commonly do
		// this), so only submit descriptors which are still open.
		struct pollfd descriptors[2];
		nfds_t descriptor_count = 0;
		if (stdout_open) {
			descriptors[descriptor_count].fd = stdout_pipe[0];
			descriptors[descriptor_count].events = POLLIN | POLLHUP;
			descriptors[descriptor_count].revents = 0;
			++descriptor_count;
		}
		if (stderr_open) {
			descriptors[descriptor_count].fd = stderr_pipe[0];
			descriptors[descriptor_count].events = POLLIN | POLLHUP;
			descriptors[descriptor_count].revents = 0;
			++descriptor_count;
		}
		const int ready = poll(descriptors, descriptor_count, 50);
		if (ready > 0) {
			for (nfds_t i = 0; i < descriptor_count; ++i) {
				if (!(descriptors[i].revents &
				      (POLLIN | POLLHUP | POLLERR)))
					continue;
				if (descriptors[i].fd == stdout_pipe[0]) {
					append_pipe(stdout_pipe[0],
						    result.standard_output,
						    output_bytes,
						    limits_.output_bytes,
						    result.output_truncated,
						    stdout_open);
				} else {
					append_pipe(stderr_pipe[0],
						    result.standard_error,
						    output_bytes,
						    limits_.output_bytes,
						    result.output_truncated,
						    stderr_open);
				}
			}
		}
		if (!exited) {
			const pid_t waited = waitpid(child, &status, WNOHANG);
			if (waited == child) {
				exited = true;
				// Commands may not leave background descendants behind. Killing the
				// process group also ensures inherited output pipes are released.
				(void)kill(-child, SIGKILL);
			}
		}
		const unsigned long long elapsed = static_cast<
			unsigned long long>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count());
		const bool cancelled =
			(cancel_requested != NULL &&
			 cancel_requested->load(std::memory_order_relaxed)) ||
			(should_cancel && should_cancel());
#ifdef __APPLE__
		if (!exited && !killed) {
			struct rusage_info_v2 usage;
			memset(&usage, 0, sizeof(usage));
			if (proc_pid_rusage(child, RUSAGE_INFO_V2,
					    reinterpret_cast<rusage_info_t *>(
						    &usage)) == 0 &&
			    usage.ri_phys_footprint > limits_.memory_bytes) {
				memory_limit_exceeded = true;
			}
			process_limit_exceeded = mac_process_tree_exceeds(
				child, limits_.process_count);
		}
#endif
		if (!exited && !killed &&
		    (cancelled || elapsed >= limits_.timeout_ms ||
		     result.output_truncated || memory_limit_exceeded ||
		     process_limit_exceeded)) {
#ifdef __APPLE__
			(void)kill(child, SIGSTOP);
			kill_sandbox_descendants(child);
#endif
			result.cancelled = cancelled;
			result.timed_out = elapsed >= limits_.timeout_ms;
			result.memory_limit_exceeded = memory_limit_exceeded;
			result.process_limit_exceeded = process_limit_exceeded;
			(void)kill(-child, SIGKILL);
			(void)kill(child, SIGKILL);
			killed = true;
		}
	}
	close_fd(stdout_pipe[0]);
	close_fd(stderr_pipe[0]);
	if (!exited) {
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
	}
#ifdef __linux__
	cleanup_linux_cgroup(child);
#endif
	result.elapsed_ms = static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started)
			.count());
	if (WIFEXITED(status))
		result.exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		result.signal = WTERMSIG(status);
	if (result.cancelled)
		result.error = "sandbox command cancelled";
	else if (result.timed_out)
		result.error = "sandbox command timed out";
	else if (result.memory_limit_exceeded) {
		result.error = "sandbox memory limit exceeded";
	} else if (result.process_limit_exceeded) {
		result.error = "sandbox process limit exceeded";
	} else if (result.output_truncated)
		result.error = "sandbox output limit exceeded";
	else if (result.exit_code == 70) {
		result.error = "sandbox HTTP service readiness probe failed";
	} else if (result.exit_code == 125 &&
		   result.standard_error.find("webcool-sandbox-helper:") !=
			   std::string::npos) {
		result.error = "sandbox platform backend is unavailable";
	} else if (result.exit_code == 126 &&
		   result.standard_error.find("webcool-sandbox-helper:") !=
			   std::string::npos) {
		result.error = "sandbox helper rejected the execution policy";
	} else if (result.exit_code == 127 &&
		   result.standard_error.find("webcool-sandbox-helper:") !=
			   std::string::npos) {
		result.error = "cannot start sandbox backend";
	} else if (result.exit_code == 71 &&
		   result.standard_error.find("sandbox-exec:") !=
			   std::string::npos) {
		result.error = "macOS sandbox profile could not be applied";
	}
	// Successful compilers normally empty TMPDIR themselves. Remove the private
	// directory when possible; interrupted tools may leave bounded cache entries,
	// in which case rmdir safely leaves it for later reuse.
	(void)rmdir(temp_directory.c_str());
	if (!result.error.empty() && !result.cancelled) {
		// Log only the broker's fixed summary. Child stdout/stderr may contain
		// source code or credentials and must never enter the service log.
		ai_log_error("program.sandbox", "child-result", result.error);
	}
	return true;
}

#endif

#if defined(__APPLE__) || defined(__linux__)
static bool execute_posix_sandbox(
	const sandbox_request_t &request, sandbox_result_t &result,
	const std::atomic<bool> *cancel_requested,
	const std::function<bool()> &should_cancel,
	const std::string &user_root_, const std::string &helper_executable_,
	const std::vector<std::string> &readonly_roots_,
	const sandbox_limits_t &limits_, const std::string &project_root,
	const std::string &workdir, const sandbox_command_t *command)
{
	const std::string helper = helper_executable_.empty() ?
					   adjacent_helper_path() :
					   helper_executable_;
	struct stat helper_st;
	if (helper.empty() || stat(helper.c_str(), &helper_st) != 0 ||
	    !S_ISREG(helper_st.st_mode) || access(helper.c_str(), X_OK) != 0) {
		result.error = "sandbox helper is missing or not executable";
		return ai_error("program.sandbox", "validate-helper",
				result.error);
	}
	int stdout_pipe[2] = { -1, -1 };
	int stderr_pipe[2] = { -1, -1 };
	if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		result.error =
			std::string("cannot create sandbox output pipes: ") +
			strerror(errno);
		return ai_error("program.sandbox", "create-output-pipes",
				result.error);
	}
	std::vector<std::string> values;
	values.push_back(helper);
	values.push_back("--user-root");
	values.push_back(user_root_);
	values.push_back("--project-root");
	values.push_back(project_root);
	values.push_back("--workdir");
	values.push_back(workdir);
	values.push_back("--cpu");
	values.push_back(std::to_string(limits_.cpu_seconds));
	values.push_back("--memory");
	values.push_back(std::to_string(limits_.memory_bytes));
	values.push_back("--processes");
	values.push_back(std::to_string(limits_.process_count));
	values.push_back("--files");
	values.push_back(std::to_string(limits_.open_files));
	values.push_back("--file-size");
	values.push_back(std::to_string(limits_.file_size_bytes));
	values.push_back("--network");
	values.push_back(command->allow_outbound_network ?
				 "outbound" :
				 (command->allow_loopback_network ? "loopback" :
								    "none"));
	values.push_back("--probe-port");
	values.push_back(std::to_string(command->http_probe_port));
	values.push_back("--probe-path");
	values.push_back(command->http_probe_path);
#if defined(__APPLE__) || defined(__linux__)
	if (!command->browser_probe_node.empty()) {
		values.push_back("--browser-node");
		values.push_back(command->browser_probe_node);
		if (!command->browser_evidence_root.empty()) {
			values.push_back("--browser-evidence-root");
			values.push_back(command->browser_evidence_root);
		}
		values.push_back("--browser-runner");
		char helper_path[PATH_MAX];
		const std::string browser_host =
			realpath(helper.c_str(), helper_path) ? helper_path :
								helper;
		values.push_back(
			browser_host.substr(0, browser_host.find_last_of('/')) +
			"/browser/browser_probe.cjs");
	}
#endif
	for (const auto &root : readonly_roots_) {
		values.push_back("--readonly-root");
		values.push_back(root);
	}
	values.push_back("--");
	values.push_back(command->executable);
	values.insert(values.end(), command->fixed_arguments.begin(),
		      command->fixed_arguments.end());
	values.insert(values.end(), request.arguments.begin(),
		      request.arguments.end());
	std::vector<std::vector<char>> argument_storage;
	std::vector<char *> argv;
	argument_storage.reserve(values.size());
	argv.reserve(values.size() + 1);
	for (size_t i = 0; i < values.size(); ++i) {
		argument_storage.push_back(
			std::vector<char>(values[i].begin(), values[i].end()));
		argument_storage.back().push_back('\0');
		argv.push_back(&argument_storage.back()[0]);
	}
	argv.push_back(NULL);
	std::vector<std::string> environment_values;
	std::string temp_directory;
	if (!ensure_sandbox_temp_directory(workdir, temp_directory,
					   result.error)) {
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		return ai_error("program.sandbox",
				"prepare-temporary-directory", result.error);
	}
	environment_values.push_back("PATH=/usr/bin:/bin:/usr/sbin:/sbin");
	environment_values.push_back("HOME=" + workdir);
	environment_values.push_back("TMPDIR=" + temp_directory);
	environment_values.push_back("LANG=C");
	environment_values.push_back("GIT_TERMINAL_PROMPT=0");
	environment_values.push_back("GIT_CONFIG_NOSYSTEM=1");
	environment_values.push_back("GIT_CONFIG_GLOBAL=/dev/null");
	std::vector<std::vector<char>> environment_storage;
	std::vector<char *> envp;
	environment_storage.reserve(environment_values.size());
	envp.reserve(environment_values.size() + 1);
	for (size_t i = 0; i < environment_values.size(); ++i) {
		environment_storage.push_back(
			std::vector<char>(environment_values[i].begin(),
					  environment_values[i].end()));
		environment_storage.back().push_back('\0');
		envp.push_back(&environment_storage.back()[0]);
	}
	envp.push_back(NULL);
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	posix_spawn_file_actions_t file_actions;
	posix_spawnattr_t attributes;
	const int file_actions_error =
		posix_spawn_file_actions_init(&file_actions);
	if (file_actions_error != 0) {
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		result.error = "cannot initialize sandbox process attributes";
		return ai_error("program.sandbox", "initialize-spawn-actions",
				result.error);
	}
	const int attributes_error = posix_spawnattr_init(&attributes);
	int setup_error = attributes_error;
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addopen(
			&file_actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_adddup2(
			&file_actions, stdout_pipe[1], STDOUT_FILENO);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_adddup2(
			&file_actions, stderr_pipe[1], STDERR_FILENO);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(&file_actions,
								stdout_pipe[0]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(&file_actions,
								stderr_pipe[0]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(&file_actions,
								stdout_pipe[1]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(&file_actions,
								stderr_pipe[1]);
	if (setup_error == 0)
		setup_error = posix_spawnattr_setflags(&attributes,
						       POSIX_SPAWN_SETPGROUP);
	if (setup_error == 0)
		setup_error = posix_spawnattr_setpgroup(&attributes, 0);
	if (setup_error != 0) {
		(void)posix_spawn_file_actions_destroy(&file_actions);
		if (attributes_error == 0)
			(void)posix_spawnattr_destroy(&attributes);
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		result.error =
			std::string("cannot configure sandbox process: ") +
			strerror(setup_error);
		return ai_error("program.sandbox", "configure-spawn",
				result.error);
	}
	pid_t child = -1;
	const int spawn_error =
		posix_spawn(&child, helper.c_str(), &file_actions, &attributes,
			    &argv[0], &envp[0]);
	(void)posix_spawn_file_actions_destroy(&file_actions);
	(void)posix_spawnattr_destroy(&attributes);
	if (spawn_error != 0) {
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		result.error = std::string("cannot start sandbox helper: ") +
			       strerror(spawn_error);
		return ai_error("program.sandbox", "spawn-helper",
				result.error);
	}
	result.started = true;
	return monitor_posix_sandbox(result, cancel_requested, should_cancel,
				     limits_, child, stdout_pipe, stderr_pipe,
				     started, temp_directory);
}

#endif

#if defined(_WIN32)
static bool execute_windows_sandbox(
	const sandbox_request_t &request, sandbox_result_t &result,
	const std::atomic<bool> *cancel_requested,
	const std::function<bool()> &should_cancel,
	const std::string &user_root_, const std::string &helper_executable_,
	const sandbox_limits_t &limits_, const std::string &project_root,
	const std::string &workdir, const sandbox_command_t *command)
{
	const std::string helper = helper_executable_.empty() ?
					   adjacent_helper_path() :
					   helper_executable_;
	std::wstring helper_wide;
	if (helper.empty() ||
	    !webcool_utf8_path_to_wide(helper.c_str(), helper_wide)) {
		result.error =
			"sandbox helper is missing or has an invalid path";
		return ai_error("program.sandbox",
				"validate-windows-helper-path", result.error);
	}
	const DWORD helper_attributes = GetFileAttributesW(helper_wide.c_str());
	if (helper_attributes == INVALID_FILE_ATTRIBUTES ||
	    (helper_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	    (helper_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		result.error = "sandbox helper is missing or unsafe";
		return ai_error("program.sandbox", "validate-windows-helper",
				result.error);
	}
	std::vector<std::string> values;
	values.push_back(helper);
	values.push_back("--user-root");
	values.push_back(user_root_);
	values.push_back("--project-root");
	values.push_back(project_root);
	values.push_back("--workdir");
	values.push_back(workdir);
	values.push_back("--cpu");
	values.push_back(std::to_string(limits_.cpu_seconds));
	values.push_back("--memory");
	values.push_back(std::to_string(limits_.memory_bytes));
	values.push_back("--processes");
	values.push_back(std::to_string(limits_.process_count));
	values.push_back("--files");
	values.push_back(std::to_string(limits_.open_files));
	values.push_back("--file-size");
	values.push_back(std::to_string(limits_.file_size_bytes));
	values.push_back("--network");
	values.push_back(command->allow_outbound_network ?
				 "outbound" :
				 (command->allow_loopback_network ? "loopback" :
								    "none"));
	values.push_back("--probe-port");
	values.push_back(std::to_string(command->http_probe_port));
	values.push_back("--probe-path");
	values.push_back(command->http_probe_path);
	values.push_back("--");
	values.push_back(command->executable);
	values.insert(values.end(), command->fixed_arguments.begin(),
		      command->fixed_arguments.end());
	values.insert(values.end(), request.arguments.begin(),
		      request.arguments.end());
	std::wstring command_line;
	for (size_t i = 0; i < values.size(); ++i) {
		std::wstring value;
		if (!webcool_utf8_to_wide(values[i].c_str(), value)) {
			result.error = "cannot encode sandbox helper arguments";
			return ai_error("program.sandbox",
					"encode-windows-arguments",
					result.error);
		}
		append_windows_quoted(command_line, value);
	}
	SECURITY_ATTRIBUTES security;
	memset(&security, 0, sizeof(security));
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	HANDLE stdout_read = NULL, stdout_write = NULL;
	HANDLE stderr_read = NULL, stderr_write = NULL;
	if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
	    !CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
		if (stdout_read)
			CloseHandle(stdout_read);
		if (stdout_write)
			CloseHandle(stdout_write);
		if (stderr_read)
			CloseHandle(stderr_read);
		if (stderr_write)
			CloseHandle(stderr_write);
		result.error = "cannot create sandbox output pipes";
		return ai_error("program.sandbox",
				"create-windows-output-pipes", result.error);
	}
	(void)SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
	(void)SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
	HANDLE input = CreateFileW(
		L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	memset(&startup, 0, sizeof(startup));
	memset(&process, 0, sizeof(process));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input;
	startup.hStdOutput = stdout_write;
	startup.hStdError = stderr_write;
	std::vector<wchar_t> mutable_command(command_line.begin(),
					     command_line.end());
	mutable_command.push_back(L'\0');
	HANDLE job = CreateJobObjectW(NULL, NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
	memset(&job_limits, 0, sizeof(job_limits));
	job_limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (job == NULL ||
	    !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
				     &job_limits, sizeof(job_limits)) ||
	    !CreateProcessW(helper_wide.c_str(), &mutable_command[0], NULL,
			    NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
			    NULL, NULL, &startup, &process) ||
	    !AssignProcessToJobObject(job, process.hProcess) ||
	    ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
		if (process.hProcess)
			TerminateProcess(process.hProcess, 126);
		if (process.hThread)
			CloseHandle(process.hThread);
		if (process.hProcess)
			CloseHandle(process.hProcess);
		if (job)
			CloseHandle(job);
		if (input != INVALID_HANDLE_VALUE)
			CloseHandle(input);
		CloseHandle(stdout_read);
		CloseHandle(stdout_write);
		CloseHandle(stderr_read);
		CloseHandle(stderr_write);
		result.error =
			"cannot start the Windows sandbox helper or Job Object";
		return ai_error("program.sandbox", "start-windows-helper",
				result.error);
	}
	CloseHandle(process.hThread);
	if (input != INVALID_HANDLE_VALUE)
		CloseHandle(input);
	CloseHandle(stdout_write);
	CloseHandle(stderr_write);
	result.started = true;
	const std::chrono::steady_clock::time_point started =
		std::chrono::steady_clock::now();
	size_t output_bytes = 0;
	bool killed = false;
	for (;;) {
		append_windows_pipe(stdout_read, result.standard_output,
				    output_bytes, limits_.output_bytes,
				    result.output_truncated);
		append_windows_pipe(stderr_read, result.standard_error,
				    output_bytes, limits_.output_bytes,
				    result.output_truncated);
		const DWORD waited = WaitForSingleObject(process.hProcess, 50);
		const unsigned long long elapsed = static_cast<
			unsigned long long>(
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
				.count());
		const bool cancelled =
			(cancel_requested != NULL &&
			 cancel_requested->load(std::memory_order_relaxed)) ||
			(should_cancel && should_cancel());
		if (!killed && (cancelled || elapsed >= limits_.timeout_ms ||
				result.output_truncated)) {
			result.cancelled = cancelled;
			result.timed_out =
				!cancelled && elapsed >= limits_.timeout_ms;
			(void)TerminateJobObject(job, cancelled ? 130 : 124);
			killed = true;
		}
		if (waited == WAIT_OBJECT_0)
			break;
		if (waited == WAIT_FAILED) {
			(void)TerminateJobObject(job, 126);
			break;
		}
	}
	(void)WaitForSingleObject(process.hProcess, INFINITE);
	append_windows_pipe(stdout_read, result.standard_output, output_bytes,
			    limits_.output_bytes, result.output_truncated);
	append_windows_pipe(stderr_read, result.standard_error, output_bytes,
			    limits_.output_bytes, result.output_truncated);
	DWORD exit_code = 126;
	(void)GetExitCodeProcess(process.hProcess, &exit_code);
	result.exit_code = static_cast<int>(exit_code);
	CloseHandle(stdout_read);
	CloseHandle(stderr_read);
	CloseHandle(process.hProcess);
	CloseHandle(job);
	result.elapsed_ms = static_cast<unsigned long long>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started)
			.count());
	if (result.cancelled)
		result.error = "sandbox command cancelled";
	else if (result.timed_out)
		result.error = "sandbox command timed out";
	else if (result.output_truncated)
		result.error = "sandbox output limit exceeded";
	else if (result.exit_code == 70) {
		result.error = "sandbox HTTP service readiness probe failed";
	} else if (result.exit_code == 125)
		result.error = "Windows sandbox isolation setup failed";
	else if (result.exit_code == 126)
		result.error =
			"Windows sandbox helper rejected the execution policy";
	else if (result.exit_code == 127)
		result.error = "cannot start Windows sandbox command";
	if (!result.error.empty() && !result.cancelled) {
		ai_log_error("program.sandbox", "windows-child-result",
			     result.error);
	}
	return true;
}

#endif

bool program_sandbox_t::execute(
	const sandbox_request_t &request, sandbox_result_t &result,
	const std::atomic<bool> *cancel_requested,
	const std::function<bool()> &should_cancel) const
{
	result = sandbox_result_t();
	std::string project_root;
	std::string workdir;
	const sandbox_command_t *command = NULL;
	if (!resolve_request(request, project_root, workdir, command,
			     result.error)) {
		return ai_error("program.sandbox", "resolve-request",
				result.error);
	}
	std::string unavailable;
	if (!backend_available(unavailable)) {
		result.error = unavailable;
		return ai_error("program.sandbox", "probe-backend",
				result.error);
	}
#if defined(_WIN32)
	return execute_windows_sandbox(
		request, result, cancel_requested, should_cancel, user_root_,
		helper_executable_, limits_, project_root, workdir, command);
#elif !defined(__APPLE__) && !defined(__linux__)
	result.error = "sandbox backend dispatch is unavailable";
	return ai_error("program.sandbox", "dispatch-backend", result.error);
#else
	return execute_posix_sandbox(request, result, cancel_requested,
				     should_cancel, user_root_,
				     helper_executable_, readonly_roots_,
				     limits_, project_root, workdir, command);
#endif
}

} // namespace ai
} // namespace webcool
