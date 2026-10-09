#include "sandbox_helper_main_internal.h"
namespace sandbox_helper
{
#ifdef _WIN32
bool utf8_to_wide(const std::string &input, std::wstring &output)
{
	if (input.empty()) {
		output.clear();
		return true;
	}
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
	    input.data(), static_cast<int>(input.size()), NULL, 0);
	if (size <= 0)
		return false;
	output.resize(static_cast<size_t>(size));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
	           static_cast<int>(input.size()), &output[0], size) == size;
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
	while (output.size() > 3 &&
	    (output[output.size() - 1] == L'\\' ||
	        output[output.size() - 1] == L'/'))
		output.resize(output.size() - 1);
	return true;
}

bool windows_path_within(const std::wstring &root, const std::wstring &path)
{
	if (!(_wcsicmp(root.c_str(), path.c_str()) == 0))
		return path.size() > root.size() &&
		    _wcsnicmp(root.c_str(), path.c_str(), root.size()) == 0 &&
		    (path[root.size()] == L'\\' || path[root.size()] == L'/');
	return true;
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
	        SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL,
	        NULL, &descriptor) != ERROR_SUCCESS)
		return false;
	HANDLE token = NULL;
	DWORD size = 0;
	bool same = false;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
		(void)GetTokenInformation(token, TokenUser, NULL, 0, &size);
		std::vector<unsigned char> buffer(size);
		if (size > 0 &&
		    GetTokenInformation(
		        token, TokenUser, &buffer[0], size, &size)) {
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
    const std::wstring &project_root, const std::wstring &workdir)
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
	        TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
	        &process_token) ||
	    !CreateRestrictedToken(process_token,
	        DISABLE_MAX_PRIVILEGE | LUA_TOKEN, 0, NULL, 0, NULL, 0, NULL,
	        &restricted_token)) {
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
	    JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
	    JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME;
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
	std::vector<wchar_t> mutable_command(
	    command_line.begin(), command_line.end());
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
	const BOOL created = CreateProcessAsUserW(restricted_token,
	    executable.c_str(), &mutable_command[0], NULL, NULL, TRUE,
	    CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
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
		fputs(
		    "webcool-sandbox-helper: restricted process launch failed\n",
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
		if (!(*cursor < 32 || *cursor == 127))
			continue;
		return false;
	}
	return true;
}

bool parse_number(
    const char *text, unsigned long long maximum, unsigned long long &value)
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

static bool parse_probe_port(const char *value, options_t &options)
{
	if (strcmp(value, "0") == 0)
		options.probe_port = 0;
	else {
		unsigned long long port = 0;
		if (!parse_number(value, 65535, port) || port < 1024)
			return false;
		options.probe_port = static_cast<unsigned short>(port);
	}

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
			if (!parse_probe_port(value, options))
				return false;
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
		if (safe_text(argv[i]))
			continue;
		return false;
	}
	return !options.user_root.empty() && !options.project_root.empty() &&
	    !options.workdir.empty() && options.cpu > 0 &&
	    options.memory >= 16ULL * 1024ULL * 1024ULL &&
	    options.processes > 0 && options.files >= 8 &&
	    (options.probe_port == 0 || options.loopback_network) &&
	    (options.browser_node.empty() == options.browser_runner.empty()) &&
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
	return path == root ||
	    (path.size() > root.size() &&
	        path.compare(0, root.size(), root) == 0 &&
	        path[root.size()] == '/');
}

bool set_limit(int resource, rlim_t value, const char *name)
{
	struct rlimit limit;
	if (getrlimit(resource, &limit) != 0) {
		fprintf(stderr,
		    "webcool-sandbox-helper: cannot read %s limit: %s\n", name,
		    strerror(errno));
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
	if (!WIFSIGNALED(status))
		return 126;
	return 128 + WTERMSIG(status);
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
	if (!(slash == std::string::npos))
		return slash == 0 ? "/" : path.substr(0, slash);
	return "";
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
	if (!(!path.empty() && path != "/"))
		return rules.empty() ?
		    "" :
		    "(allow file-read-metadata" + rules + ")";
	rules += " (literal \"" + escape_literal(path) + "\")";

	return rules.empty() ? "" : "(allow file-read-metadata" + rules + ")";
}

std::string macos_profile(const std::string &user_root,
    const std::string &project_root, const std::string &toolchain_root,
    const std::string &toolchain_alias_root, bool loopback_network,
    bool outbound_network, const std::vector<std::string> &readonly_roots)
{
	std::string dependency_rules;
	for (const auto &path : readonly_roots) {
		dependency_rules += macos_metadata_ancestor_rules(path) +
		    "(allow file-read* (subpath \"" + escape_literal(path) +
		    "\"))" + "(deny file-write* (subpath \"" +
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
	    + (toolchain.empty() ? "" :
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

#endif
#endif
}
using namespace sandbox_helper;
#ifdef __APPLE__
static int run_macos_sandbox(options_t &options, int argc, char **argv,
    const std::string &user_root, const std::string &project_root,
    const std::string &workdir, const std::string &requested_executable,
    const std::string &canonical_executable)
{
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
	    toolchain_alias_root.compare(
	        toolchain_alias_root.size() - 4, 4, "/bin") == 0) {
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
	            !set_limit(RLIMIT_FSIZE,
	                static_cast<rlim_t>(options.file_size), "file-size") ||
	            !set_limit(RLIMIT_NOFILE,
	                static_cast<rlim_t>(options.files), "open-files")))) {
		fputs("webcool-sandbox-helper: resource limit setup failed\n",
		    stderr);
		return 126;
	}

	// Explicit project contract: the service reads WEBCOOL_HTTP_PORT.
	// Preserve compatibility with legacy fixed-port projects.
	const bool dynamic_browser_port = !options.browser_node.empty() &&
	    access((project_root + "/.webcool-http-port-env").c_str(), F_OK) ==
	        0;
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
	values.push_back(macos_profile(user_root, project_root, toolchain_root,
	    toolchain_alias_root, options.loopback_network,
	    options.outbound_network, options.readonly_roots));
	for (int i = options.command_index; i < argc; ++i) {
		// Built-in launchers such as php -S carry the listen address in argv.
		const std::string argument = argv[i];
		values.push_back(
		    dynamic_browser_port && argument == "127.0.0.1:18080" ?
		        "127.0.0.1:" + std::to_string(options.probe_port) :
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
			return probe_http_service(service, options.probe_port,
			    options.probe_path,
			    options.browser_node.empty() ? NULL : &options);
		}
		if (!options.browser_node.empty() &&
		    (!set_limit(RLIMIT_CPU, options.cpu, "CPU") ||
		        !set_limit(
		            RLIMIT_FSIZE, options.file_size, "file-size") ||
		        !set_limit(RLIMIT_NOFILE, options.files, "open-files")))
			return 126;
		(void)setpgid(0, 0);
	}
	execv("/usr/bin/sandbox-exec", &arguments[0]);
	fputs("webcool-sandbox-helper: cannot start macOS sandbox\n", stderr);
	return 127;
}
#endif

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
			fputs(
			    "webcool-sandbox-helper: invalid read-only dependency root\n",
			    stderr);
			return 126;
		}
		path = canonical;
	}
#ifdef __linux__
	if (!options.browser_node.empty()) {
		if (access(options.browser_node.c_str(), X_OK) != 0) {
			fputs(
			    "webcool-sandbox-helper: browser runtime unavailable; check Node installation\n",
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
			fputs(
			    "webcool-sandbox-helper: browser runtime path rejected\n",
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
			fputs(
			    "webcool-sandbox-helper: browser runner is not deployment-owned\n",
			    stderr);
			return 126;
		}
		options.browser_node = node_path;
		options.browser_runner = runner_path;
		if (!options.browser_evidence_root.empty()) {
			std::string evidence;
			if (!canonical_directory(
			        options.browser_evidence_root, evidence) ||
			    !path_is_within(user_root, evidence)) {
				fputs(
				    "webcool-sandbox-helper: browser evidence path rejected\n",
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
	return run_macos_sandbox(options, argc, argv, user_root, project_root,
	    workdir, requested_executable, canonical_executable);

#endif
#endif
}
