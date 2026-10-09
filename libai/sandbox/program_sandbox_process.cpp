#include "stdafx.h"
#include "program_sandbox_internal.h"
namespace webcool
{
namespace ai
{
using namespace sandbox_detail;
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
// Both streams consume the same byte budget, preventing stderr from bypassing
// the output cap while preserving separate buffers for diagnostics.
static void drain_sandbox_pipes(struct pollfd *descriptors,
    nfds_t descriptor_count, int *stdout_pipe, int *stderr_pipe,
    sandbox_result_t &result, size_t &output_bytes,
    const sandbox_limits_t &limits_, bool &stdout_open, bool &stderr_open)
{
	for (nfds_t i = 0; i < descriptor_count; ++i) {
		if (!(descriptors[i].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		if (descriptors[i].fd == stdout_pipe[0]) {
			append_pipe(stdout_pipe[0], result.standard_output,
			    output_bytes, limits_.output_bytes,
			    result.output_truncated, stdout_open);
		} else {
			append_pipe(stderr_pipe[0], result.standard_error,
			    output_bytes, limits_.output_bytes,
			    result.output_truncated, stderr_open);
		}
	}
}

static bool monitor_posix_sandbox(sandbox_result_t &result,
    const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const sandbox_limits_t &limits_,
    pid_t child, int *stdout_pipe, int *stderr_pipe,
    const std::chrono::steady_clock::time_point &started,
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
			drain_sandbox_pipes(descriptors, descriptor_count,
			    stdout_pipe, stderr_pipe, result, output_bytes,
			    limits_, stdout_open, stderr_open);
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
		const unsigned long long elapsed =
		    static_cast<unsigned long long>(
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
			        reinterpret_cast<rusage_info_t *>(&usage)) ==
			        0 &&
			    usage.ri_phys_footprint > limits_.memory_bytes) {
				memory_limit_exceeded = true;
			}
			process_limit_exceeded = mac_process_tree_exceeds(
			    child, limits_.process_count);
		}
#endif
		if (!(!exited && !killed &&
		        (cancelled || elapsed >= limits_.timeout_ms ||
		            result.output_truncated || memory_limit_exceeded ||
		            process_limit_exceeded)))
			continue;
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
	    result.standard_error.find("sandbox-exec:") != std::string::npos) {
		result.error = "macOS sandbox profile could not be applied";
	}
	// Successful compilers normally empty TMPDIR themselves. Remove the private
	// directory when possible; interrupted tools may leave bounded cache entries,
	// in which case rmdir safely leaves it for later reuse.
	(void)rmdir(temp_directory.c_str());
	if (!(!result.error.empty() && !result.cancelled))
		return true;
	// Log only the broker's fixed summary. Child stdout/stderr may contain
	// source code or credentials and must never enter the service log.
	ai_log_error("program.sandbox", "child-result", result.error);

	return true;
}

#endif

#if defined(__APPLE__) || defined(__linux__)
static std::vector<std::string> posix_helper_arguments(
    const sandbox_request_t &request, const std::string &helper,
    const std::string &user_root_, const std::string &project_root,
    const std::string &workdir, const sandbox_limits_t &limits_,
    const sandbox_command_t *command,
    const std::vector<std::string> &readonly_roots_)
{
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
	        (command->allow_loopback_network ? "loopback" : "none"));
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
	values.insert(
	    values.end(), request.arguments.begin(), request.arguments.end());
	return values;
}

bool execute_posix_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_,
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
		return ai_error(
		    "program.sandbox", "validate-helper", result.error);
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
		return ai_error(
		    "program.sandbox", "create-output-pipes", result.error);
	}
	const auto values = posix_helper_arguments(request, helper, user_root_,
	    project_root, workdir, limits_, command, readonly_roots_);
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
	if (!ensure_sandbox_temp_directory(
	        workdir, temp_directory, result.error)) {
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
		setup_error = posix_spawn_file_actions_addclose(
		    &file_actions, stdout_pipe[0]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(
		    &file_actions, stderr_pipe[0]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(
		    &file_actions, stdout_pipe[1]);
	if (setup_error == 0)
		setup_error = posix_spawn_file_actions_addclose(
		    &file_actions, stderr_pipe[1]);
	if (setup_error == 0)
		setup_error = posix_spawnattr_setflags(
		    &attributes, POSIX_SPAWN_SETPGROUP);
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
		return ai_error(
		    "program.sandbox", "configure-spawn", result.error);
	}
	pid_t child = -1;
	const int spawn_error = posix_spawn(&child, helper.c_str(),
	    &file_actions, &attributes, &argv[0], &envp[0]);
	(void)posix_spawn_file_actions_destroy(&file_actions);
	(void)posix_spawnattr_destroy(&attributes);
	if (spawn_error != 0) {
		close_fd(stdout_pipe[0]);
		close_fd(stdout_pipe[1]);
		close_fd(stderr_pipe[0]);
		close_fd(stderr_pipe[1]);
		result.error = std::string("cannot start sandbox helper: ") +
		    strerror(spawn_error);
		return ai_error(
		    "program.sandbox", "spawn-helper", result.error);
	}
	result.started = true;
	return monitor_posix_sandbox(result, cancel_requested, should_cancel,
	    limits_, child, stdout_pipe, stderr_pipe, started, temp_directory);
}

#endif

#if defined(_WIN32)
static bool monitor_windows_sandbox(sandbox_result_t &result,
    const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const sandbox_limits_t &limits_,
    PROCESS_INFORMATION &process, HANDLE stdout_read, HANDLE stderr_read,
    HANDLE job)
{
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
		const unsigned long long elapsed =
		    static_cast<unsigned long long>(
		        std::chrono::duration_cast<std::chrono::milliseconds>(
		            std::chrono::steady_clock::now() - started)
		            .count());
		const bool cancelled =
		    (cancel_requested != NULL &&
		        cancel_requested->load(std::memory_order_relaxed)) ||
		    (should_cancel && should_cancel());
		if (!killed &&
		    (cancelled || elapsed >= limits_.timeout_ms ||
		        result.output_truncated)) {
			result.cancelled = cancelled;
			result.timed_out =
			    !cancelled && elapsed >= limits_.timeout_ms;
			(void)TerminateJobObject(job, cancelled ? 130 : 124);
			killed = true;
		}
		if (waited == WAIT_OBJECT_0)
			break;
		if (!(waited == WAIT_FAILED))
			continue;
		(void)TerminateJobObject(job, 126);
		break;
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
	if (!(!result.error.empty() && !result.cancelled))
		return true;
	ai_log_error("program.sandbox", "windows-child-result", result.error);

	return true;
}

bool execute_windows_sandbox(const sandbox_request_t &request,
    sandbox_result_t &result, const std::atomic<bool> *cancel_requested,
    const std::function<bool()> &should_cancel, const std::string &user_root_,
    const std::string &helper_executable_, const sandbox_limits_t &limits_,
    const std::string &project_root, const std::string &workdir,
    const sandbox_command_t *command)
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
		return ai_error(
		    "program.sandbox", "validate-windows-helper", result.error);
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
	        (command->allow_loopback_network ? "loopback" : "none"));
	values.push_back("--probe-port");
	values.push_back(std::to_string(command->http_probe_port));
	values.push_back("--probe-path");
	values.push_back(command->http_probe_path);
	values.push_back("--");
	values.push_back(command->executable);
	values.insert(values.end(), command->fixed_arguments.begin(),
	    command->fixed_arguments.end());
	values.insert(
	    values.end(), request.arguments.begin(), request.arguments.end());
	std::wstring command_line;
	for (size_t i = 0; i < values.size(); ++i) {
		std::wstring value;
		if (!webcool_utf8_to_wide(values[i].c_str(), value)) {
			result.error = "cannot encode sandbox helper arguments";
			return ai_error("program.sandbox",
			    "encode-windows-arguments", result.error);
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
	HANDLE input = CreateFileW(L"NUL", GENERIC_READ,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
	    FILE_ATTRIBUTE_NORMAL, NULL);
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	memset(&startup, 0, sizeof(startup));
	memset(&process, 0, sizeof(process));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input;
	startup.hStdOutput = stdout_write;
	startup.hStdError = stderr_write;
	std::vector<wchar_t> mutable_command(
	    command_line.begin(), command_line.end());
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
	        NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL,
	        &startup, &process) ||
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
		return ai_error(
		    "program.sandbox", "start-windows-helper", result.error);
	}
	CloseHandle(process.hThread);
	if (input != INVALID_HANDLE_VALUE)
		CloseHandle(input);
	CloseHandle(stdout_write);
	CloseHandle(stderr_write);
	return monitor_windows_sandbox(result, cancel_requested, should_cancel,
	    limits_, process, stdout_read, stderr_read, job);
}

#endif

}
}
