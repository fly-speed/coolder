#include "sandbox_helper_main_internal.h"
namespace sandbox_helper
{
#ifdef __linux__
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
		request.ifr_flags = static_cast<short>(
		    request.ifr_flags | IFF_UP | IFF_RUNNING);
		ok = ioctl(descriptor, SIOCSIFFLAGS, &request) == 0;
	}
	close(descriptor);
	return ok;
}

bool setup_user_and_system_namespaces(
    bool loopback_network, bool outbound_network)
{
	const uid_t uid = getuid();
	const gid_t gid = getgid();
	if (unshare(CLONE_NEWUSER) != 0)
		return false;
	(void)write_text_file("/proc/self/setgroups", "deny\n");
	if (!write_text_file("/proc/self/uid_map",
	        "0 " + std::to_string(static_cast<unsigned long long>(uid)) +
	            " 1\n") ||
	    !write_text_file("/proc/self/gid_map",
	        "0 " + std::to_string(static_cast<unsigned long long>(gid)) +
	            " 1\n") ||
	    setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0) {
		return false;
	}
	if (!(unshare(CLONE_NEWNS | (outbound_network ? 0 : CLONE_NEWNET) |
	          CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID) != 0))
		return mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0 &&
		    (!loopback_network || enable_loopback_interface());
	return false;
}

#ifdef WEBCOOL_HAVE_LANDLOCK_HEADERS
unsigned long long landlock_access_mask()
{
	unsigned long long mask = LANDLOCK_ACCESS_FS_EXECUTE |
	    LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
	    LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
	    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
	    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
	    LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
	    LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
	mask |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
	mask |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
	return mask;
}

bool add_landlock_path(
    int ruleset_fd, const char *path, unsigned long long access, bool required)
{
	const int parent_fd = open(path, O_PATH | O_CLOEXEC);
	if (parent_fd < 0)
		return !required && errno == ENOENT;
	struct landlock_path_beneath_attr rule;
	memset(&rule, 0, sizeof(rule));
	rule.allowed_access = access;
	rule.parent_fd = parent_fd;
	const int result = static_cast<int>(syscall(__NR_landlock_add_rule,
	    ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
	close(parent_fd);
	return result == 0;
}

bool apply_landlock(const std::string &project_root, bool outbound_network,
    const std::vector<std::string> &readonly_roots,
    const std::string &browser_runner, const std::string &browser_evidence_root)
{
#if !defined(__NR_landlock_create_ruleset) || \
    !defined(__NR_landlock_add_rule) || !defined(__NR_landlock_restrict_self)
	(void)project_root;
	(void)outbound_network;
	(void)readonly_roots;
	(void)browser_runner;
	(void)browser_evidence_root;
	return false;
#else
	const int abi = static_cast<int>(syscall(__NR_landlock_create_ruleset,
	    NULL, 0, LANDLOCK_CREATE_RULESET_VERSION));
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
	    LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
	const char *system_paths[] = { "/usr", "/bin", "/sbin", "/lib",
		"/lib64" };
	bool ok = true;
	for (size_t i = 0; i < sizeof(system_paths) / sizeof(system_paths[0]);
	     ++i) {
		if (add_landlock_path(
		        ruleset_fd, system_paths[i], read_only, false))
			continue;
		ok = false;
		break;
	}
	const char *system_files[] = { "/etc/ld.so.cache", "/etc/localtime",
		"/dev/null", "/dev/urandom" };
	for (size_t i = 0;
	     ok && i < sizeof(system_files) / sizeof(system_files[0]); ++i) {
		ok = add_landlock_path(ruleset_fd, system_files[i],
		    LANDLOCK_ACCESS_FS_READ_FILE, false);
	}
	if (outbound_network) {
		for (const char *path : { "/etc/resolv.conf", "/etc/hosts",
		         "/etc/nsswitch.conf", "/etc/ssl", "/etc/pki" }) {
			if (!ok)
				continue;
			ok = add_landlock_path(
			    ruleset_fd, path, read_only, false);
		}
	}
	for (const auto &path : readonly_roots) {
		if (!ok)
			continue;
		ok = add_landlock_path(
		    ruleset_fd, path.c_str(), read_only, true);
	}
	if (ok && !browser_runner.empty()) {
		const size_t slash = browser_runner.rfind('/');
		const std::string browser_root = slash == std::string::npos ?
		    "" :
		    browser_runner.substr(0, slash);
		ok = !browser_root.empty() &&
		    add_landlock_path(
		        ruleset_fd, browser_root.c_str(), read_only, true);
		const char *browser_system_paths[] = { "/opt", "/proc",
			"/etc/fonts", "/etc/ssl", "/sys/devices",
			"/sys/bus/pci" };
		for (size_t i = 0; ok &&
		     i < sizeof(browser_system_paths) /
		             sizeof(browser_system_paths[0]);
		     ++i)
			ok = add_landlock_path(ruleset_fd,
			    browser_system_paths[i], read_only, false);
		const char *browser_system_files[] = { "/etc/passwd",
			"/etc/group", "/etc/machine-id" };
		for (size_t i = 0; ok &&
		     i < sizeof(browser_system_files) /
		             sizeof(browser_system_files[0]);
		     ++i)
			ok = add_landlock_path(ruleset_fd,
			    browser_system_files[i],
			    LANDLOCK_ACCESS_FS_READ_FILE, false);
	}
	if (ok)
		ok = add_landlock_path(ruleset_fd, project_root.c_str(),
		    landlock_access_mask(), true);
	if (ok && !browser_evidence_root.empty() &&
	    !path_is_within(project_root, browser_evidence_root)) {
		ok =
		    add_landlock_path(ruleset_fd, browser_evidence_root.c_str(),
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

bool apply_seccomp(
    bool loopback_network, bool outbound_network, bool browser_runtime)
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
	    BPF_STMT(BPF_RET | BPF_K,                           \
	        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))
	struct sock_filter filter[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
		                            static_cast<unsigned int>(offsetof(
		                                struct seccomp_data, arch))),
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
		    browser_runtime ?
		        SECCOMP_RET_ALLOW :
		        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
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
		    outbound_network ?
		        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA) :
		        SECCOMP_RET_ALLOW),
#endif
#ifdef __NR_listen
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_listen, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
		    outbound_network ?
		        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA) :
		        SECCOMP_RET_ALLOW),
#endif
#ifdef __NR_socket
		// Loopback services retain a private network namespace. Explicit outbound
		// builds use host routing; bind/listen remain denied above.
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
		    (loopback_network || outbound_network) ?
		        SECCOMP_RET_ALLOW :
		        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_socketpair
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1),
		BPF_STMT(BPF_RET | BPF_K,
		    (loopback_network || outbound_network) ?
		        SECCOMP_RET_ALLOW :
		        SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
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
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
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
	if (!WIFSIGNALED(status))
		return 126;
	return 128 + WTERMSIG(status);
}

static void run_service_probe(const std::string &project_root,
    const options_t &runtime_options, pid_t service)
{
	if (!apply_landlock(project_root, runtime_options.outbound_network,
	        runtime_options.readonly_roots, runtime_options.browser_runner,
	        runtime_options.browser_evidence_root) ||
	    !apply_seccomp(runtime_options.loopback_network,
	        runtime_options.outbound_network,
	        !runtime_options.browser_node.empty())) {
		_exit(125);
	}
	_exit(probe_http_service(service, runtime_options.probe_port,
	    runtime_options.probe_path,
	    runtime_options.browser_node.empty() ? NULL : &runtime_options));
}

int run_linux_sandbox(const options_t &options, char *argv[],
    const std::string &project_root, const std::string &workdir)
{
	const std::string cgroup = "/sys/fs/cgroup/webcool/run-" +
	    std::to_string(static_cast<unsigned long long>(getpid()));
	if (mkdir(cgroup.c_str(), 0700) != 0 ||
	    !write_text_file(cgroup + "/memory.max",
	        std::to_string(options.memory) + "\n") ||
	    !write_text_file(cgroup + "/pids.max",
	        std::to_string(options.processes) + "\n") ||
	    !write_text_file(cgroup + "/cpu.max", "100000 100000\n")) {
		(void)rmdir(cgroup.c_str());
		fputs(
		    "webcool-sandbox-helper: cgroup v2 delegation unavailable\n",
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
		    !setup_user_and_system_namespaces(
		        options.loopback_network, options.outbound_network))
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
			            std::to_string(runtime_options.probe_port) :
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
		    || !set_limit(RLIMIT_NPROC,
		           static_cast<rlim_t>(runtime_options.processes),
		           "process-count")
#endif
		) {
			fputs(
			    "webcool-sandbox-helper: Linux isolation setup failed\n",
			    stderr);
			_exit(125);
		}
		if (runtime_options.probe_port != 0) {
			const pid_t service = fork();
			if (service < 0)
				_exit(126);
			if (service > 0) {
				run_service_probe(
				    project_root, runtime_options, service);
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
	const bool assigned = write_text_file(cgroup + "/cgroup.procs",
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
	if (synchronized)
		return mirrored_exit_status(status);
	fputs(
	    "webcool-sandbox-helper: cannot enter delegated cgroup\n", stderr);
	return 125;
}

#endif
}
