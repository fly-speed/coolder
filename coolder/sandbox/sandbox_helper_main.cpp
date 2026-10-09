#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/audit.h>
#include <linux/filter.h>
#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define WEBCOOL_HAVE_LANDLOCK_HEADERS 1
#endif
#else
#include <linux/landlock.h>
#define WEBCOOL_HAVE_LANDLOCK_HEADERS 1
#endif
#include <linux/seccomp.h>
#include <sched.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif

namespace
{

// This executable intentionally does not link the ACL server runtime. Failures
// are written to its captured stderr; program_sandbox_t converts them to a
// bounded summary and records that summary with logger_error in the main
// service. Keeping the helper small reduces its trusted computing base.
struct options_t {
	std::string user_root;
	std::string project_root;
	std::string workdir;
	std::vector<std::string> readonly_roots;
	unsigned long long cpu = 0;
	unsigned long long memory = 0;
	unsigned long long processes = 0;
	unsigned long long files = 0;
	unsigned long long file_size = 0;
	bool loopback_network = false;
	bool outbound_network = false;
	unsigned short probe_port = 0;
	std::string probe_path = "/";
	std::string browser_node, browser_runner, browser_evidence_root;
	int command_index = -1;
};

#ifdef _WIN32
bool utf8_to_wide(const std::string &input, std::wstring &output)
{
	if (input.empty()) {
		output.clear();
		return true;
	}
	const int size =
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
				    static_cast<int>(input.size()), NULL, 0);
	if (size <= 0)
		return false;
	output.resize(static_cast<size_t>(size));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
				   static_cast<int>(input.size()), &output[0],
				   size) == size;
}

bool canonical_windows_directory(const std::string &input, std::wstring &output)
{
	std::wstring wide;
	if (!utf8_to_wide(input, wide))
		return false;
	const DWORD needed = GetFullPathNameW(wide.c_str(), 0, NULL, NULL);
	if (needed == 0 || needed > 32768)
		return false;
	std::vector<wchar_t> full(needed + 1, L'\0');
	if (GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()),
			     &full[0], NULL) == 0)
		return false;
	const DWORD attributes = GetFileAttributesW(&full[0]);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		return false;
	output.assign(&full[0]);
	while (output.size() > 3 && (output[output.size() - 1] == L'\\' ||
				     output[output.size() - 1] == L'/'))
		output.resize(output.size() - 1);
	return true;
}

bool windows_path_within(const std::wstring &root, const std::wstring &path)
{
	if (_wcsicmp(root.c_str(), path.c_str()) == 0)
		return true;
	return path.size() > root.size() &&
	       _wcsnicmp(root.c_str(), path.c_str(), root.size()) == 0 &&
	       (path[root.size()] == L'\\' || path[root.size()] == L'/');
}

void append_quoted(std::wstring &command, const std::wstring &argument)
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
			continue;
		}
		command.append(slashes, L'\\');
		slashes = 0;
		command += argument[i];
	}
	command.append(slashes * 2, L'\\');
	command += L'"';
}

bool directory_owned_by_current_user(const std::wstring &directory)
{
	PSID owner = NULL;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	if (GetNamedSecurityInfoW(const_cast<wchar_t *>(directory.c_str()),
				  SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
				  &owner, NULL, NULL, NULL,
				  &descriptor) != ERROR_SUCCESS)
		return false;
	HANDLE token = NULL;
	DWORD size = 0;
	bool same = false;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		(void)GetTokenInformation(token, TokenUser, NULL, 0, &size);
		std::vector<unsigned char> buffer(size);
		if (size > 0 && GetTokenInformation(token, TokenUser,
						    &buffer[0], size, &size)) {
			TOKEN_USER *user =
				reinterpret_cast<TOKEN_USER *>(&buffer[0]);
			same = EqualSid(owner, user->User.Sid) != FALSE;
		}
		CloseHandle(token);
	}
	LocalFree(descriptor);
	return same;
}

int run_windows_sandbox(const options_t &options, int argc, char *argv[],
			const std::wstring &project_root,
			const std::wstring &workdir)
{
	if (!directory_owned_by_current_user(project_root)) {
		fputs("webcool-sandbox-helper: project ACL owner mismatch\n",
		      stderr);
		return 125;
	}
	std::wstring executable;
	if (!utf8_to_wide(argv[options.command_index], executable))
		return 126;
	const DWORD executable_attributes =
		GetFileAttributesW(executable.c_str());
	if (executable_attributes == INVALID_FILE_ATTRIBUTES ||
	    (executable_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	    (executable_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		return 126;
	std::wstring command_line;
	for (int i = options.command_index; i < argc; ++i) {
		std::wstring argument;
		if (!utf8_to_wide(argv[i], argument))
			return 126;
		append_quoted(command_line, argument);
	}
	HANDLE process_token = NULL;
	HANDLE restricted_token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(),
			      TOKEN_DUPLICATE | TOKEN_QUERY |
				      TOKEN_ASSIGN_PRIMARY,
			      &process_token) ||
	    !CreateRestrictedToken(process_token,
				   DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 0, NULL,
				   0, NULL, 0, NULL, &restricted_token)) {
		if (process_token)
			CloseHandle(process_token);
		fputs("webcool-sandbox-helper: restricted token setup failed\n",
		      stderr);
		return 125;
	}
	CloseHandle(process_token);
	HANDLE job = CreateJobObjectW(NULL, NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
	memset(&limits, 0, sizeof(limits));
	limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
		JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
		JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
		JOB_OBJECT_LIMIT_PROCESS_TIME;
	limits.BasicLimitInformation.ActiveProcessLimit =
		static_cast<DWORD>(options.processes);
	limits.ProcessMemoryLimit = static_cast<SIZE_T>(options.memory);
	limits.JobMemoryLimit = static_cast<SIZE_T>(options.memory);
	limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
		static_cast<LONGLONG>(options.cpu) * 10000000LL;
	if (job == NULL ||
	    !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
				     &limits, sizeof(limits))) {
		if (job)
			CloseHandle(job);
		CloseHandle(restricted_token);
		fputs("webcool-sandbox-helper: Job Object setup failed\n",
		      stderr);
		return 125;
	}
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	memset(&startup, 0, sizeof(startup));
	memset(&process, 0, sizeof(process));
	startup.cb = sizeof(startup);
	std::vector<wchar_t> mutable_command(command_line.begin(),
					     command_line.end());
	mutable_command.push_back(L'\0');
	// Do not forward WebCool service environment variables to project code.
	// The double-null-terminated block contains only fixed system search paths
	// and project-local home/temp locations.
	std::wstring environment;
	environment += L"PATH=C:\\Windows\\System32;C:\\Windows";
	environment.push_back(L'\0');
	environment += L"USERPROFILE=" + workdir;
	environment.push_back(L'\0');
	environment += L"TEMP=" + workdir;
	environment.push_back(L'\0');
	environment += L"TMP=" + workdir;
	environment.push_back(L'\0');
	environment += L"LANG=C";
	environment.push_back(L'\0');
	environment.push_back(L'\0');
	const BOOL created = CreateProcessAsUserW(
		restricted_token, executable.c_str(), &mutable_command[0], NULL,
		NULL, TRUE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW |
			CREATE_UNICODE_ENVIRONMENT,
		&environment[0], workdir.c_str(), &startup, &process);
	CloseHandle(restricted_token);
	if (!created || !AssignProcessToJobObject(job, process.hProcess) ||
	    ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
		if (created) {
			TerminateProcess(process.hProcess, 126);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
		CloseHandle(job);
		fputs("webcool-sandbox-helper: restricted process launch failed\n",
		      stderr);
		return 125;
	}
	CloseHandle(process.hThread);
	(void)WaitForSingleObject(process.hProcess, INFINITE);
	DWORD exit_code = 126;
	(void)GetExitCodeProcess(process.hProcess, &exit_code);
	CloseHandle(process.hProcess);
	CloseHandle(
		job); // KILL_ON_JOB_CLOSE removes any surviving descendants.
	return static_cast<int>(exit_code);
}
#endif

bool safe_text(const char *text)
{
	if (text == NULL)
		return false;
	for (const unsigned char *cursor =
		     reinterpret_cast<const unsigned char *>(text);
	     *cursor != 0; ++cursor) {
		if (*cursor < 32 || *cursor == 127)
			return false;
	}
	return true;
}

bool parse_number(const char *text, unsigned long long maximum,
		  unsigned long long &value)
{
	if (text == NULL || *text == '\0')
		return false;
	char *end = NULL;
	errno = 0;
	const unsigned long long parsed = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
	    parsed > maximum)
		return false;
	value = parsed;
	return true;
}

bool parse_options(int argc, char *argv[], options_t &options)
{
	if (argc < 2 || argc > 80)
		return false;
	for (int i = 1; i < argc; ++i) {
		const std::string name = argv[i];
		if (name == "--") {
			options.command_index = i + 1;
			break;
		}
		if (i + 1 >= argc)
			return false;
		const char *value = argv[++i];
		if (name == "--user-root")
			options.user_root = value;
		else if (name == "--project-root")
			options.project_root = value;
		else if (name == "--workdir")
			options.workdir = value;
		else if (name == "--readonly-root") {
			if (!safe_text(value) || value[0] != '/' ||
			    options.readonly_roots.size() >= 8)
				return false;
			options.readonly_roots.push_back(value);
		} else if (name == "--cpu") {
			if (!parse_number(value, 600, options.cpu))
				return false;
		} else if (name == "--memory") {
			if (!parse_number(value,
					  16ULL * 1024ULL * 1024ULL * 1024ULL,
					  options.memory))
				return false;
		} else if (name == "--processes") {
			if (!parse_number(value, 256, options.processes))
				return false;
		} else if (name == "--files") {
			if (!parse_number(value, 1024, options.files))
				return false;
		} else if (name == "--file-size") {
			if (!parse_number(value, 1024ULL * 1024ULL * 1024ULL,
					  options.file_size))
				return false;
		} else if (name == "--network") {
			options.loopback_network = false;
			options.outbound_network = false;
			if (strcmp(value, "none") == 0)
				options.loopback_network = false;
			else if (strcmp(value, "outbound") == 0)
				options.outbound_network = true;
			else if (strcmp(value, "loopback") == 0) {
				options.loopback_network = true;
			} else
				return false;
		} else if (name == "--probe-port") {
			if (strcmp(value, "0") == 0)
				options.probe_port = 0;
			else {
				unsigned long long port = 0;
				if (!parse_number(value, 65535, port) ||
				    port < 1024)
					return false;
				options.probe_port =
					static_cast<unsigned short>(port);
			}
		} else if (name == "--browser-evidence-root") {
			if (!safe_text(value) || value[0] != '/')
				return false;
			options.browser_evidence_root = value;
		} else if (name == "--browser-node" ||
			   name == "--browser-runner") {
			if (!safe_text(value) || value[0] != '/')
				return false;
			if (name == "--browser-node")
				options.browser_node = value;
			else
				options.browser_runner = value;
		} else if (name == "--probe-path") {
			if (!safe_text(value) || value[0] != '/' ||
			    strlen(value) > 256) {
				return false;
			}
			options.probe_path = value;
		} else
			return false;
	}
	if (!safe_text(options.user_root.c_str()) ||
	    !safe_text(options.project_root.c_str()) ||
	    !safe_text(options.workdir.c_str()))
		return false;
	for (int i = options.command_index; i > 0 && i < argc; ++i) {
		if (!safe_text(argv[i]))
			return false;
	}
	return !options.user_root.empty() && !options.project_root.empty() &&
	       !options.workdir.empty() && options.cpu > 0 &&
	       options.memory >= 16ULL * 1024ULL * 1024ULL &&
	       options.processes > 0 && options.files >= 8 &&
	       (options.probe_port == 0 || options.loopback_network) &&
	       (options.browser_node.empty() ==
		options.browser_runner.empty()) &&
	       (options.browser_node.empty() || options.probe_port != 0) &&
	       options.command_index > 0 && options.command_index < argc;
}

#ifndef _WIN32
bool canonical_directory(const std::string &input, std::string &output)
{
	char path[PATH_MAX];
	if (realpath(input.c_str(), path) == NULL)
		return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return false;
	output = path;
	return true;
}

bool path_is_within(const std::string &root, const std::string &path)
{
	return path == root || (path.size() > root.size() &&
				path.compare(0, root.size(), root) == 0 &&
				path[root.size()] == '/');
}

bool set_limit(int resource, rlim_t value, const char *name)
{
	struct rlimit limit;
	if (getrlimit(resource, &limit) != 0) {
		fprintf(stderr,
			"webcool-sandbox-helper: cannot read %s limit: %s\n",
			name, strerror(errno));
		return false;
	}
	// A host/container may already impose a stricter hard limit. Keep that
	// stricter value instead of trying to raise it, and lower both soft and hard
	// limits so the sandboxed process cannot restore a broader allowance.
	const rlim_t selected =
		limit.rlim_max != RLIM_INFINITY && limit.rlim_max < value ?
			limit.rlim_max :
			value;
	limit.rlim_cur = selected;
	limit.rlim_max = selected;
	if (setrlimit(resource, &limit) == 0)
		return true;
	fprintf(stderr, "webcool-sandbox-helper: cannot set %s limit: %s\n",
		name, strerror(errno));
	return false;
}

void close_inherited_descriptors()
{
#ifdef __linux__
#ifdef __NR_close_range
	if (syscall(__NR_close_range, 3U, ~0U, 0U) == 0)
		return;
#endif
#endif
	long maximum = sysconf(_SC_OPEN_MAX);
	if (maximum < 0 || maximum > 65536)
		maximum = 65536;
	for (int fd = 3; fd < maximum; ++fd)
		close(fd);
}

int child_exit_status(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 126;
}

int terminate_service(pid_t child, int result)
{
	(void)kill(-child, SIGTERM);
	(void)kill(child, SIGTERM);
	for (int attempt = 0; attempt < 20; ++attempt) {
		int status = 0;
		if (waitpid(child, &status, WNOHANG) == child)
			return result;
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
	}
	(void)kill(-child, SIGKILL);
	(void)kill(child, SIGKILL);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	return result;
}

// The probe runs inside the Linux network namespace and beside the macOS
// sandbox process. Project code can therefore expose only its isolated
// loopback listener; no host/LAN network access is required.
bool allocate_probe_port(unsigned short &port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t size = sizeof(address);
	const bool ok =
		bind(fd, reinterpret_cast<struct sockaddr *>(&address), size) ==
			0 &&
		getsockname(fd, reinterpret_cast<struct sockaddr *>(&address),
			    &size) == 0;
	if (ok)
		port = ntohs(address.sin_port);
	close(fd);
	return ok && port >= 1024;
}

#ifdef __APPLE__
bool probe_port_available(unsigned short port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	struct sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const bool available =
		bind(fd, reinterpret_cast<struct sockaddr *>(&address),
		     sizeof(address)) == 0;
	close(fd);
	if (!available)
		fputs("webcool-sandbox-helper: validation blocked: service port already occupied; refusing to test an existing server\n",
		      stderr);
	return available;
}

#endif

int run_browser_probe(const options_t &options)
{
#if defined(__APPLE__) || defined(__linux__)
	if (access(options.browser_node.c_str(), X_OK) != 0 ||
	    access(options.browser_runner.c_str(), R_OK) != 0) {
		fputs("webcool-sandbox-helper: browser runtime unavailable; check Node and deployed browser runner\n",
		      stderr);
		return 78;
	}
	const std::string url =
		"http://127.0.0.1:" + std::to_string(options.probe_port) +
		options.probe_path;
#ifdef __linux__
	// Chromium's own namespace sandbox cannot create nested namespaces after
	// WebCool applies seccomp. The browser still runs inside WebCool's private
	// user, mount, PID, IPC, UTS and network namespaces, Landlock rules and
	// delegated cgroup, so tell the trusted runner to disable only that nested
	// Chromium layer.
	if (setenv("WEBCOOL_BROWSER_OUTER_SANDBOX", "linux", 1) != 0)
		return 126;
	if (setenv("MOZ_DISABLE_CONTENT_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_GMP_SANDBOX", "1", 1) != 0 ||
	    setenv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1", 1) != 0)
		return 126;
#endif
	const pid_t browser = fork();
	if (browser < 0)
		return 126;
	if (browser == 0) {
		execl(options.browser_node.c_str(),
		      options.browser_node.c_str(),
		      options.browser_runner.c_str(), url.c_str(),
		      options.project_root.c_str(),
		      (options.browser_evidence_root.empty() ?
			       options.project_root :
			       options.browser_evidence_root)
			      .c_str(),
		      static_cast<char *>(NULL));
		fputs("webcool-sandbox-helper: browser runtime unavailable; install browser dependencies and Node\n",
		      stderr);
		_exit(78);
	}
	int status = 0;
	while (waitpid(browser, &status, 0) < 0) {
		if (errno != EINTR)
			return 126;
	}
	return child_exit_status(status);
#else
	(void)options;
	return 78;
#endif
}

int probe_http_service(pid_t child, unsigned short port,
		       const std::string &path,
		       const options_t *browser_options = NULL)
{
	const std::chrono::steady_clock::time_point deadline =
		std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		int status = 0;
		const pid_t waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			return child_exit_status(status);
		const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
		if (descriptor >= 0) {
			struct timeval receive_timeout;
			receive_timeout.tv_sec = 0;
			receive_timeout.tv_usec = 250000;
			(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
					 &receive_timeout,
					 sizeof(receive_timeout));
			struct sockaddr_in address;
			memset(&address, 0, sizeof(address));
			address.sin_family = AF_INET;
			address.sin_port = htons(port);
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(descriptor,
				    reinterpret_cast<struct sockaddr *>(
					    &address),
				    sizeof(address)) == 0) {
				const std::string request =
					"GET " + path +
					" HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
				(void)send(descriptor, request.data(),
					   request.size(), 0);
				char response[64];
				const ssize_t received =
					recv(descriptor, response,
					     sizeof(response) - 1, 0);
				if (received > 12) {
					response[received] = '\0';
					const char *space =
						strchr(response, ' ');
					const int code =
						space == NULL ? 0 :
								atoi(space + 1);
					close(descriptor);
					if (strncmp(response, "HTTP/", 5) ==
						    0 &&
					    code >= 200 && code < 400) {
						return terminate_service(
							child,
							browser_options ?
								run_browser_probe(
									*browser_options) :
								0);
					}
				} else
					close(descriptor);
			} else
				close(descriptor);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	fputs("webcool-sandbox-helper: HTTP service readiness probe timed out\n",
	      stderr);
	return terminate_service(child, 70);
}

#ifdef __APPLE__
std::string escape_literal(const std::string &value)
{
	std::string escaped;
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '\\' || value[i] == '"')
			escaped += '\\';
		escaped += value[i];
	}
	return escaped;
}

std::string parent_directory(const std::string &path)
{
	const size_t slash = path.rfind('/');
	if (slash == std::string::npos)
		return "";
	return slash == 0 ? "/" : path.substr(0, slash);
}

std::string macos_metadata_ancestor_rules(const std::string &path)
{
	std::string rules;
	// Seatbelt requires metadata permission on every ancestor used while
	// resolving a non-system compiler installation. Contents of those ancestors
	// remain unreadable because only the exact directory nodes are listed.
	for (size_t slash = path.find('/', 1); slash != std::string::npos;
	     slash = path.find('/', slash + 1)) {
		rules += " (literal \"" +
			 escape_literal(path.substr(0, slash)) + "\")";
	}
	if (!path.empty() && path != "/") {
		rules += " (literal \"" + escape_literal(path) + "\")";
	}
	return rules.empty() ? "" : "(allow file-read-metadata" + rules + ")";
}

std::string macos_profile(const std::string &user_root,
			  const std::string &project_root,
			  const std::string &toolchain_root,
			  const std::string &toolchain_alias_root,
			  bool loopback_network, bool outbound_network,
			  const std::vector<std::string> &readonly_roots)
{
	std::string dependency_rules;
	for (const auto &path : readonly_roots) {
		dependency_rules += macos_metadata_ancestor_rules(path) +
				    "(allow file-read* (subpath \"" +
				    escape_literal(path) + "\"))" +
				    "(deny file-write* (subpath \"" +
				    escape_literal(path) + "\"))";
	}
	const std::string user = escape_literal(user_root);
	const std::string root = escape_literal(project_root);
	const std::string toolchain = escape_literal(toolchain_root);
	const std::string toolchain_alias =
		escape_literal(toolchain_alias_root);
	const std::string toolchain_ancestors =
		macos_metadata_ancestor_rules(toolchain_root);
	const std::string toolchain_alias_ancestors =
		macos_metadata_ancestor_rules(toolchain_alias_root);
	return "(version 1)(deny default)"
	       "(allow process-exec process-fork)"
	       "(allow sysctl-read)(allow mach-lookup)"
	       // Enumerate runtime/toolchain roots instead of allowing every file and
	       // trying to subtract user directories. Seatbelt deny rules cannot be safely
	       // re-opened for a nested project, which previously prevented freshly built
	       // project executables from starting under /Users or /tmp.
	       // dyld reads the root directory object while locating the shared cache on
	       // current macOS. `literal` deliberately does not expose descendants.
	       "(allow file-read* (literal \"/\"))"
	       "(allow file-read-metadata (literal \"/Applications\"))"
	       "(allow file-read* (subpath \"/System\") (subpath \"/usr\")"
	       " (subpath \"/bin\") (subpath \"/sbin\") (subpath \"/dev\")"
	       " (subpath \"/Library\")"
	       " (subpath \"/Applications/Xcode.app\")"
	       " (subpath \"/opt/homebrew\") (subpath \"/etc\")"
	       " (subpath \"/private/etc\")"
	       " (subpath \"/private/var/db\") (subpath \"/private/var/select\")"
	       // xcode-select reads the /var alias before resolving developer_dir.
	       " (literal \"/var\") (subpath \"/var/select\")"
	       " (subpath \"/private/var/run\") (literal \"/dev/null\"))"
	       // Administrators may configure a compiler outside the standard system
	       // roots (for example /Users/name/golang/go/bin/go). Its installation
	       // tree is read-only; project output remains confined to project_root.
	       + (toolchain.empty() ?
			  "" :
			  "(allow file-read* (literal \"" + toolchain +
				  "\")"
				  " (subpath \"" +
				  toolchain + "\"))") +
	       toolchain_ancestors +
	       (toolchain_alias.empty() || toolchain_alias == toolchain ?
			"" :
			"(allow file-read* (literal \"" + toolchain_alias +
				"\")"
				" (subpath \"" +
				toolchain_alias + "\"))") +
	       toolchain_alias_ancestors + dependency_rules +
	       macos_metadata_ancestor_rules(project_root) +
	       "(allow file-read* (subpath \"" + root + "\"))" +
	       "(deny file-read* (subpath \"" + user + "/.webcool_agent\"))" +
	       "(allow file-write* (literal \"/dev/null\"))" +
	       "(allow file-write* (subpath \"" + root + "\"))" +
	       "(deny file-write* (subpath \"" + user + "/.webcool_agent\"))" +
	       (outbound_network ? "(allow network-outbound)" : "") +
	       (loopback_network ?
			"(allow network-inbound (local ip \"localhost:*\"))"
			"(allow network-outbound (remote ip \"localhost:*\"))" :
			"");
}
#elif defined(__linux__)

bool write_text_file(const std::string &path, const std::string &value)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	const ssize_t written = write(fd, value.data(), value.size());
	const int saved = errno;
	close(fd);
	errno = saved;
	return written == static_cast<ssize_t>(value.size());
}

bool enable_loopback_interface()
{
	const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return false;
	struct ifreq request;
	memset(&request, 0, sizeof(request));
	strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
	bool ok = ioctl(descriptor, SIOCGIFFLAGS, &request) == 0;
	if (ok) {
		request.ifr_flags = static_cast<short>(request.ifr_flags |
						       IFF_UP | IFF_RUNNING);
		ok = ioctl(descriptor, SIOCSIFFLAGS, &request) == 0;
	}
	close(descriptor);
	return ok;
}

bool setup_user_and_system_namespaces(bool loopback_network,
				      bool outbound_network)
{
	const uid_t uid = getuid();
	const gid_t gid = getgid();
	if (unshare(CLONE_NEWUSER) != 0)
		return false;
	(void)write_text_file("/proc/self/setgroups", "deny\n");
	if (!write_text_file(
		    "/proc/self/uid_map",
		    "0 " +
			    std::to_string(
				    static_cast<unsigned long long>(uid)) +
			    " 1\n") ||
	    !write_text_file(
		    "/proc/self/gid_map",
		    "0 " +
			    std::to_string(
				    static_cast<unsigned long long>(gid)) +
			    " 1\n") ||
	    setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0) {
		return false;
	}
	if (unshare(CLONE_NEWNS | (outbound_network ? 0 : CLONE_NEWNET) |
		    CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID) != 0)
		return false;
	return mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0 &&
	       (!loopback_network || enable_loopback_interface());
}

#ifdef WEBCOOL_HAVE_LANDLOCK_HEADERS
unsigned long long landlock_access_mask()
{
	unsigned long long mask =
		LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
		LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
		LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
		LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
		LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
	mask |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
	mask |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
	return mask;
}

bool add_landlock_path(int ruleset_fd, const char *path,
		       unsigned long long access, bool required)
{
	const int parent_fd = open(path, O_PATH | O_CLOEXEC);
	if (parent_fd < 0)
		return !required && errno == ENOENT;
	struct landlock_path_beneath_attr rule;
	memset(&rule, 0, sizeof(rule));
	rule.allowed_access = access;
	rule.parent_fd = parent_fd;
	const int result =
		static_cast<int>(syscall(__NR_landlock_add_rule, ruleset_fd,
					 LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
	close(parent_fd);
	return result == 0;
}

bool apply_landlock(const std::string &project_root, bool outbound_network,
		    const std::vector<std::string> &readonly_roots,
		    const std::string &browser_runner,
		    const std::string &browser_evidence_root)
{
#if !defined(__NR_landlock_create_ruleset) || \
	!defined(__NR_landlock_add_rule) ||   \
	!defined(__NR_landlock_restrict_self)
	(void)project_root;
	(void)outbound_network;
	(void)readonly_roots;
	(void)browser_runner;
	(void)browser_evidence_root;
	return false;
#else
	const int abi =
		static_cast<int>(syscall(__NR_landlock_create_ruleset, NULL, 0,
					 LANDLOCK_CREATE_RULESET_VERSION));
	if (abi < 1)
		return false;
	struct landlock_ruleset_attr ruleset;
	memset(&ruleset, 0, sizeof(ruleset));
	ruleset.handled_access_fs = landlock_access_mask();
	const int ruleset_fd = static_cast<int>(syscall(
		__NR_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0));
	if (ruleset_fd < 0)
		return false;
	const unsigned long long read_only = LANDLOCK_ACCESS_FS_EXECUTE |
					     LANDLOCK_ACCESS_FS_READ_FILE |
					     LANDLOCK_ACCESS_FS_READ_DIR;
	const char *system_paths[] = { "/usr", "/bin", "/sbin", "/lib",
				       "/lib64" };
	bool ok = true;
	for (size_t i = 0; i < sizeof(system_paths) / sizeof(system_paths[0]);
	     ++i) {
		if (!add_landlock_path(ruleset_fd, system_paths[i], read_only,
				       false)) {
			ok = false;
			break;
		}
	}
	const char *system_files[] = { "/etc/ld.so.cache", "/etc/localtime",
				       "/dev/null", "/dev/urandom" };
	for (size_t i = 0;
	     ok && i < sizeof(system_files) / sizeof(system_files[0]); ++i) {
		ok = add_landlock_path(ruleset_fd, system_files[i],
				       LANDLOCK_ACCESS_FS_READ_FILE, false);
	}
	if (outbound_network) {
		for (const char *path :
		     { "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
		       "/etc/ssl", "/etc/pki" })
			if (ok)
				ok = add_landlock_path(ruleset_fd, path,
						       read_only, false);
	}
	for (const auto &path : readonly_roots)
		if (ok)
			ok = add_landlock_path(ruleset_fd, path.c_str(),
					       read_only, true);
	if (ok && !browser_runner.empty()) {
		const size_t slash = browser_runner.rfind('/');
		const std::string browser_root =
			slash == std::string::npos ?
				"" :
				browser_runner.substr(0, slash);
		ok = !browser_root.empty() &&
		     add_landlock_path(ruleset_fd, browser_root.c_str(),
				       read_only, true);
		const char *browser_system_paths[] = {
			"/opt",	    "/proc",	    "/etc/fonts",
			"/etc/ssl", "/sys/devices", "/sys/bus/pci"
		};
		for (size_t i = 0;
		     ok && i < sizeof(browser_system_paths) /
					   sizeof(browser_system_paths[0]);
		     ++i)
			ok = add_landlock_path(ruleset_fd,
					       browser_system_paths[i],
					       read_only, false);
		const char *browser_system_files[] = { "/etc/passwd",
						       "/etc/group",
						       "/etc/machine-id" };
		for (size_t i = 0;
		     ok && i < sizeof(browser_system_files) /
					   sizeof(browser_system_files[0]);
		     ++i)
			ok = add_landlock_path(
				ruleset_fd, browser_system_files[i],
				LANDLOCK_ACCESS_FS_READ_FILE, false);
	}
	if (ok)
		ok = add_landlock_path(ruleset_fd, project_root.c_str(),
				       landlock_access_mask(), true);
	if (ok && !browser_evidence_root.empty() &&
	    !path_is_within(project_root, browser_evidence_root)) {
		ok = add_landlock_path(ruleset_fd,
				       browser_evidence_root.c_str(),
				       landlock_access_mask(), true);
	}
	if (ok && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0) {
		ok = syscall(__NR_landlock_restrict_self, ruleset_fd, 0) == 0;
	} else
		ok = false;
	close(ruleset_fd);
	return ok;
#endif
}
#else
bool apply_landlock(const std::string &, bool, const std::vector<std::string> &,
		    const std::string &, const std::string &)
{
	return false;
}
#endif

bool apply_seccomp(bool loopback_network, bool outbound_network,
		   bool browser_runtime)
{
#if defined(__x86_64__)
	const unsigned int architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
	const unsigned int architecture = AUDIT_ARCH_AARCH64;
#else
	return false;
#endif
#define WEBCOOL_DENY_SYSCALL(name)                              \
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), \
		BPF_STMT(BPF_RET | BPF_K,                       \
			 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 static_cast<unsigned int>(
				 offsetof(struct seccomp_data, arch))),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
#ifdef SECCOMP_RET_KILL_PROCESS
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#else
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
#endif
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 static_cast<unsigned int>(
				 offsetof(struct seccomp_data, nr))),
#if defined(__x86_64__)
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000U, 0, 1),
#ifdef SECCOMP_RET_KILL_PROCESS
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#else
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
#endif
#endif
#ifdef __NR_ptrace
		WEBCOOL_DENY_SYSCALL(ptrace),
#endif
#ifdef __NR_bpf
		WEBCOOL_DENY_SYSCALL(bpf),
#endif
#ifdef __NR_mount
		WEBCOOL_DENY_SYSCALL(mount),
#endif
#ifdef __NR_umount2
		WEBCOOL_DENY_SYSCALL(umount2),
#endif
#ifdef __NR_unshare
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_unshare, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 browser_runtime ? SECCOMP_RET_ALLOW :
					   SECCOMP_RET_ERRNO |
						   (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_setns
		WEBCOOL_DENY_SYSCALL(setns),
#endif
#ifdef __NR_open_by_handle_at
		WEBCOOL_DENY_SYSCALL(open_by_handle_at),
#endif
#ifdef __NR_perf_event_open
		WEBCOOL_DENY_SYSCALL(perf_event_open),
#endif
#ifdef __NR_userfaultfd
		WEBCOOL_DENY_SYSCALL(userfaultfd),
#endif
#ifdef __NR_process_vm_readv
		WEBCOOL_DENY_SYSCALL(process_vm_readv),
#endif
#ifdef __NR_process_vm_writev
		WEBCOOL_DENY_SYSCALL(process_vm_writev),
#endif
#ifdef __NR_bind
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 outbound_network ? SECCOMP_RET_ERRNO |
						    (EPERM & SECCOMP_RET_DATA) :
					    SECCOMP_RET_ALLOW),
#endif
#ifdef __NR_listen
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_listen, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 outbound_network ? SECCOMP_RET_ERRNO |
						    (EPERM & SECCOMP_RET_DATA) :
					    SECCOMP_RET_ALLOW),
#endif
#ifdef __NR_socket
		// Loopback services retain a private network namespace. Explicit outbound
		// builds use host routing; bind/listen remain denied above.
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 (loopback_network || outbound_network) ?
				 SECCOMP_RET_ALLOW :
				 SECCOMP_RET_ERRNO |
					 (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_socketpair
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
			 (loopback_network || outbound_network) ?
				 SECCOMP_RET_ALLOW :
				 SECCOMP_RET_ERRNO |
					 (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_keyctl
		WEBCOOL_DENY_SYSCALL(keyctl),
#endif
#ifdef __NR_add_key
		WEBCOOL_DENY_SYSCALL(add_key),
#endif
#ifdef __NR_request_key
		WEBCOOL_DENY_SYSCALL(request_key),
#endif
#ifdef __NR_io_uring_setup
		WEBCOOL_DENY_SYSCALL(io_uring_setup),
#endif
#ifdef __NR_init_module
		WEBCOOL_DENY_SYSCALL(init_module),
#endif
#ifdef __NR_finit_module
		WEBCOOL_DENY_SYSCALL(finit_module),
#endif
#ifdef __NR_delete_module
		WEBCOOL_DENY_SYSCALL(delete_module),
#endif
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
	};
#undef WEBCOOL_DENY_SYSCALL
	struct sock_fprog program;
	program.len =
		static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0]));
	program.filter = filter;
	return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
	       prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

int mirrored_exit_status(int status)
{
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 126;
}

int run_linux_sandbox(const options_t &options, char *argv[],
		      const std::string &project_root,
		      const std::string &workdir)
{
	const std::string cgroup =
		"/sys/fs/cgroup/webcool/run-" +
		std::to_string(static_cast<unsigned long long>(getpid()));
	if (mkdir(cgroup.c_str(), 0700) != 0 ||
	    !write_text_file(cgroup + "/memory.max",
			     std::to_string(options.memory) + "\n") ||
	    !write_text_file(cgroup + "/pids.max",
			     std::to_string(options.processes) + "\n") ||
	    !write_text_file(cgroup + "/cpu.max", "100000 100000\n")) {
		(void)rmdir(cgroup.c_str());
		fputs("webcool-sandbox-helper: cgroup v2 delegation unavailable\n",
		      stderr);
		return 125;
	}
	int sync_pipe[2];
	if (pipe(sync_pipe) != 0) {
		(void)rmdir(cgroup.c_str());
		return 126;
	}
	const pid_t child = fork();
	if (child < 0) {
		close(sync_pipe[0]);
		close(sync_pipe[1]);
		(void)rmdir(cgroup.c_str());
		return 126;
	}
	if (child == 0) {
		close(sync_pipe[1]);
		char ready = 0;
		const bool synchronized =
			read(sync_pipe[0], &ready, 1) == 1 && ready == 1;
		close(sync_pipe[0]);
		if (!synchronized ||
		    !setup_user_and_system_namespaces(options.loopback_network,
						      options.outbound_network))
			_exit(125);
		const pid_t namespaced = fork();
		if (namespaced < 0)
			_exit(126);
		if (namespaced > 0) {
			int status = 0;
			while (waitpid(namespaced, &status, 0) < 0 &&
			       errno == EINTR) {
			}
			_exit(mirrored_exit_status(status));
		}
		// CLONE_NEWPID affects children only. Mount proc after the second fork so
		// Chromium sees only this validation tree rather than host processes.
		if (!options.browser_node.empty() &&
		    mount("proc", "/proc", "proc",
			  MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
			_exit(125);
		options_t runtime_options = options;
		std::vector<std::string> runtime_values;
		std::vector<char *> runtime_arguments;
		if (!runtime_options.browser_node.empty() &&
		    access((project_root + "/.webcool-http-port-env").c_str(),
			   F_OK) == 0) {
			if (!allocate_probe_port(runtime_options.probe_port))
				_exit(73);
			const std::string port =
				std::to_string(runtime_options.probe_port);
			if (setenv("WEBCOOL_HTTP_PORT", port.c_str(), 1) != 0)
				_exit(126);
		}
		for (int i = runtime_options.command_index; argv[i] != NULL;
		     ++i) {
			const std::string argument = argv[i];
			runtime_values.push_back(
				!runtime_options.browser_node.empty() &&
						runtime_options.probe_port !=
							options.probe_port &&
						argument == "127.0.0.1:18080" ?
					"127.0.0.1:" +
						std::to_string(
							runtime_options
								.probe_port) :
					argument);
		}
		for (size_t i = 0; i < runtime_values.size(); ++i)
			runtime_arguments.push_back(
				const_cast<char *>(runtime_values[i].c_str()));
		runtime_arguments.push_back(NULL);
		if (chdir(workdir.c_str()) != 0 ||
		    !set_limit(RLIMIT_CPU,
			       static_cast<rlim_t>(runtime_options.cpu), "CPU")
		    // Browsers reserve multi-gigabyte virtual address ranges. Their actual
		    // resident memory remains bounded by memory.max in the delegated cgroup.
		    || (runtime_options.browser_node.empty() &&
			!set_limit(RLIMIT_AS,
				   static_cast<rlim_t>(runtime_options.memory),
				   "address-space")) ||
		    !set_limit(RLIMIT_FSIZE,
			       static_cast<rlim_t>(runtime_options.file_size),
			       "file-size") ||
		    !set_limit(RLIMIT_NOFILE,
			       static_cast<rlim_t>(runtime_options.files),
			       "open-files")
#ifdef RLIMIT_NPROC
		    ||
		    !set_limit(RLIMIT_NPROC,
			       static_cast<rlim_t>(runtime_options.processes),
			       "process-count")
#endif
		) {
			fputs("webcool-sandbox-helper: Linux isolation setup failed\n",
			      stderr);
			_exit(125);
		}
		if (runtime_options.probe_port != 0) {
			const pid_t service = fork();
			if (service < 0)
				_exit(126);
			if (service > 0) {
				if (!apply_landlock(
					    project_root,
					    runtime_options.outbound_network,
					    runtime_options.readonly_roots,
					    runtime_options.browser_runner,
					    runtime_options
						    .browser_evidence_root) ||
				    !apply_seccomp(
					    runtime_options.loopback_network,
					    runtime_options.outbound_network,
					    !runtime_options.browser_node
						     .empty())) {
					_exit(125);
				}
				_exit(probe_http_service(
					service, runtime_options.probe_port,
					runtime_options.probe_path,
					runtime_options.browser_node.empty() ?
						NULL :
						&runtime_options));
			}
			(void)setpgid(0, 0);
		}
		// The untrusted project service gets the strict base filesystem and syscall
		// policy. Only the trusted probe parent receives browser runtime paths and
		// nested unshare support required by Firefox.
		if (!apply_landlock(project_root,
				    runtime_options.outbound_network,
				    runtime_options.readonly_roots, "", "") ||
		    !apply_seccomp(runtime_options.loopback_network,
				   runtime_options.outbound_network, false)) {
			_exit(125);
		}
		execv(runtime_arguments[0], &runtime_arguments[0]);
		fputs("webcool-sandbox-helper: cannot start sandbox command\n",
		      stderr);
		_exit(127);
	}
	close(sync_pipe[0]);
	const bool assigned = write_text_file(
		cgroup + "/cgroup.procs",
		std::to_string(static_cast<unsigned long long>(child)) + "\n");
	const char ready = assigned ? 1 : 0;
	ssize_t written;
	do {
		written = write(sync_pipe[1], &ready, 1);
	} while (written < 0 && errno == EINTR);
	close(sync_pipe[1]);
	const bool synchronized = assigned && written == 1;
	if (!synchronized)
		(void)kill(child, SIGKILL);
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	(void)write_text_file(cgroup + "/cgroup.kill", "1\n");
	(void)rmdir(cgroup.c_str());
	if (!synchronized) {
		fputs("webcool-sandbox-helper: cannot enter delegated cgroup\n",
		      stderr);
		return 125;
	}
	return mirrored_exit_status(status);
}
#endif
#endif

} // namespace

int main(int argc, char *argv[])
{
#ifndef _WIN32
	close_inherited_descriptors();
#endif
	options_t options;
	if (!parse_options(argc, argv, options)) {
		fputs("webcool-sandbox-helper: invalid invocation\n", stderr);
		return 126;
	}
#ifdef _WIN32
	std::wstring user_root;
	std::wstring project_root;
	std::wstring workdir;
	if (!canonical_windows_directory(options.user_root, user_root) ||
	    !canonical_windows_directory(options.project_root, project_root) ||
	    !canonical_windows_directory(options.workdir, workdir) ||
	    !windows_path_within(user_root, project_root) ||
	    !windows_path_within(project_root, workdir)) {
		fputs("webcool-sandbox-helper: directory policy rejected\n",
		      stderr);
		return 126;
	}
	return run_windows_sandbox(options, argc, argv, project_root, workdir);
#else
	std::string user_root;
	std::string project_root;
	std::string workdir;
	if (!canonical_directory(options.user_root, user_root) ||
	    !canonical_directory(options.project_root, project_root) ||
	    project_root == "/" ||
	    !canonical_directory(options.workdir, workdir) ||
	    !path_is_within(user_root, project_root) ||
	    !path_is_within(project_root, workdir)) {
		fputs("webcool-sandbox-helper: directory policy rejected\n",
		      stderr);
		return 126;
	}
	options.user_root = user_root;
	options.project_root = project_root;
	options.workdir = workdir;
	for (auto &path : options.readonly_roots) {
		std::string canonical;
		if (!canonical_directory(path, canonical) ||
		    path_is_within(project_root, canonical) ||
		    path_is_within(canonical, project_root)) {
			fputs("webcool-sandbox-helper: invalid read-only dependency root\n",
			      stderr);
			return 126;
		}
		path = canonical;
	}
#ifdef __linux__
	if (!options.browser_node.empty()) {
		if (access(options.browser_node.c_str(), X_OK) != 0) {
			fputs("webcool-sandbox-helper: browser runtime unavailable; check Node installation\n",
			      stderr);
			return 78;
		}
		char node_path[PATH_MAX], runner_path[PATH_MAX],
			helper_path[PATH_MAX];
		struct stat node_st, runner_st;
		const ssize_t helper_size = readlink(
			"/proc/self/exe", helper_path, sizeof(helper_path) - 1);
		if (realpath(options.browser_node.c_str(), node_path) == NULL ||
		    stat(node_path, &node_st) != 0 ||
		    !S_ISREG(node_st.st_mode) || access(node_path, X_OK) != 0 ||
		    realpath(options.browser_runner.c_str(), runner_path) ==
			    NULL ||
		    stat(runner_path, &runner_st) != 0 ||
		    !S_ISREG(runner_st.st_mode) || helper_size <= 0) {
			fputs("webcool-sandbox-helper: browser runtime path rejected\n",
			      stderr);
			return 126;
		}
		helper_path[helper_size] = '\0';
		std::string helper_directory(helper_path);
		const size_t helper_slash = helper_directory.rfind('/');
		if (helper_slash == std::string::npos)
			return 126;
		helper_directory.resize(helper_slash);
		char expected_runner[PATH_MAX];
		const std::string expected =
			helper_directory + "/browser/browser_probe.cjs";
		if (realpath(expected.c_str(), expected_runner) == NULL ||
		    strcmp(expected_runner, runner_path) != 0) {
			fputs("webcool-sandbox-helper: browser runner is not deployment-owned\n",
			      stderr);
			return 126;
		}
		options.browser_node = node_path;
		options.browser_runner = runner_path;
		if (!options.browser_evidence_root.empty()) {
			std::string evidence;
			if (!canonical_directory(options.browser_evidence_root,
						 evidence) ||
			    !path_is_within(user_root, evidence)) {
				fputs("webcool-sandbox-helper: browser evidence path rejected\n",
				      stderr);
				return 126;
			}
			options.browser_evidence_root = evidence;
		}
	}
#endif
	const char *executable = argv[options.command_index];
	const std::string requested_executable =
		executable == NULL ? "" : executable;
	char canonical_executable[PATH_MAX];
	struct stat executable_st;
	if (executable == NULL || executable[0] != '/' ||
	    realpath(executable, canonical_executable) == NULL ||
	    stat(canonical_executable, &executable_st) != 0 ||
	    !S_ISREG(executable_st.st_mode) ||
	    access(canonical_executable, X_OK) != 0) {
		fputs("webcool-sandbox-helper: executable policy rejected\n",
		      stderr);
		return 126;
	}
	// macOS treats /tmp and /private/tmp as distinct Seatbelt path literals even
	// though the former resolves to the latter. Execute the same canonical path
	// used by the project/workdir policy so project-built binaries cannot be
	// rejected merely because the caller used a filesystem alias. Linux gains
	// the same deterministic no-symlink execution behavior.
	argv[options.command_index] = canonical_executable;
#if defined(__linux__)
	return run_linux_sandbox(options, argv, project_root, workdir);
#elif !defined(__APPLE__)
	fputs("webcool-sandbox-helper: platform backend unavailable\n", stderr);
	return 125;
#else
	// Most compiler installations place the executable in a bin directory and
	// keep libraries, source and data beside it. Grant that installation root
	// read-only access. Never broaden a shallow /bin-style path to filesystem /.
	std::string toolchain_root = parent_directory(canonical_executable);
	if (toolchain_root.size() > 4 &&
	    toolchain_root.compare(toolchain_root.size() - 4, 4, "/bin") == 0) {
		const std::string installation_root =
			parent_directory(toolchain_root);
		if (installation_root != "/")
			toolchain_root = installation_root;
	}
	if (toolchain_root == "/" ||
	    path_is_within(project_root, canonical_executable))
		toolchain_root.clear();
	std::string toolchain_alias_root =
		parent_directory(requested_executable);
	if (toolchain_alias_root.size() > 4 &&
	    toolchain_alias_root.compare(toolchain_alias_root.size() - 4, 4,
					 "/bin") == 0) {
		const std::string installation_root =
			parent_directory(toolchain_alias_root);
		if (installation_root != "/")
			toolchain_alias_root = installation_root;
	}
	if (toolchain_alias_root == "/" ||
	    path_is_within(project_root, requested_executable))
		toolchain_alias_root.clear();
	if (chdir(workdir.c_str()) != 0 ||
	    (options.browser_node.empty() &&
	     (!set_limit(RLIMIT_CPU, static_cast<rlim_t>(options.cpu), "CPU")
	      // macOS rejects finite RLIMIT_AS/RLIMIT_DATA values with EINVAL. The
	      // parent broker enforces this limit from proc_pid_rusage and kills the
	      // complete process group; the helper still applies all supported limits.
	      ||
	      !set_limit(RLIMIT_FSIZE, static_cast<rlim_t>(options.file_size),
			 "file-size") ||
	      !set_limit(RLIMIT_NOFILE, static_cast<rlim_t>(options.files),
			 "open-files")))) {
		fputs("webcool-sandbox-helper: resource limit setup failed\n",
		      stderr);
		return 126;
	}

	// Explicit project contract: the service reads WEBCOOL_HTTP_PORT.
	// Preserve compatibility with legacy fixed-port projects.
	const bool dynamic_browser_port =
		!options.browser_node.empty() &&
		access((project_root + "/.webcool-http-port-env").c_str(),
		       F_OK) == 0;
	if (dynamic_browser_port) {
		if (!allocate_probe_port(options.probe_port))
			return 73;
		const std::string port = std::to_string(options.probe_port);
		if (setenv("WEBCOOL_HTTP_PORT", port.c_str(), 1) != 0)
			return 126;
	}
	std::vector<std::string> values;
	values.push_back("/usr/bin/sandbox-exec");
	values.push_back("-p");
	values.push_back(macos_profile(
		user_root, project_root, toolchain_root, toolchain_alias_root,
		options.loopback_network, options.outbound_network,
		options.readonly_roots));
	for (int i = options.command_index; i < argc; ++i) {
		// Built-in launchers such as php -S carry the listen address in argv.
		const std::string argument = argv[i];
		values.push_back(
			dynamic_browser_port && argument == "127.0.0.1:18080" ?
				"127.0.0.1:" +
					std::to_string(options.probe_port) :
				argument);
	}
	std::vector<std::vector<char>> storage;
	std::vector<char *> arguments;
	storage.reserve(values.size());
	arguments.reserve(values.size() + 1);
	for (size_t i = 0; i < values.size(); ++i) {
		storage.push_back(
			std::vector<char>(values[i].begin(), values[i].end()));
		storage.back().push_back('\0');
		arguments.push_back(&storage.back()[0]);
	}
	arguments.push_back(NULL);
	if (options.probe_port != 0) {
		if (!probe_port_available(options.probe_port))
			return 73;
		const pid_t service = fork();
		if (service < 0)
			return 126;
		if (service > 0) {
			return probe_http_service(
				service, options.probe_port, options.probe_path,
				options.browser_node.empty() ? NULL : &options);
		}
		if (!options.browser_node.empty() &&
		    (!set_limit(RLIMIT_CPU, options.cpu, "CPU") ||
		     !set_limit(RLIMIT_FSIZE, options.file_size, "file-size") ||
		     !set_limit(RLIMIT_NOFILE, options.files, "open-files")))
			return 126;
		(void)setpgid(0, 0);
	}
	execv("/usr/bin/sandbox-exec", &arguments[0]);
	fputs("webcool-sandbox-helper: cannot start macOS sandbox\n", stderr);
	return 127;
#endif
#endif
}
