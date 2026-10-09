#pragma once
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

namespace sandbox_helper
{

// This executable intentionally does not link the ACL server runtime. Failures
// are written to its captured stderr; program_sandbox_t converts them to a
// bounded summary and records that summary with logger_error in the main
// service. Keeping the helper small reduces its trusted computing base.
struct options_t {
	// Filesystem root belonging to the authenticated user.
	std::string user_root;
	// Resolved filesystem root of the selected project.
	std::string project_root;
	// Working directory passed to the sandbox helper.
	std::string workdir;
	// Additional filesystem roots granted read-only access.
	std::vector<std::string> readonly_roots;
	// CPU time limit in seconds passed to the isolated command.
	unsigned long long cpu = 0;
	// Memory allowance in bytes passed to the sandbox helper.
	unsigned long long memory = 0;
	// Maximum process count passed to the sandbox helper.
	unsigned long long processes = 0;
	// Maximum number of open descriptors allowed in the sandboxed
	// process.
	unsigned long long files = 0;
	// Maximum file size in bytes passed to the sandbox helper.
	unsigned long long file_size = 0;
	// Whether sandboxed commands may use loopback networking.
	bool loopback_network = false;
	// Whether sandboxed commands may use outbound networking.
	bool outbound_network = false;
	// Local port used by the HTTP service readiness probe.
	unsigned short probe_port = 0;
	// HTTP path requested by the readiness probe.
	std::string probe_path = "/";
	// browser_node: Node executable used by the trusted browser probe.
	// browser_runner: Trusted browser probe runner path.
	// browser_evidence_root: Directory used to exchange browser probe
	// evidence.
	std::string browser_node, browser_runner, browser_evidence_root;
	// Index of the command executable in the helper's argv array.
	int command_index = -1;
};

#ifndef _WIN32
// Resolve an existing directory to its canonical filesystem path.
bool canonical_directory(const std::string &input, std::string &output);
// Check whether a path lies inside the specified filesystem root.
bool path_is_within(const std::string &root, const std::string &path);
// Apply the named kernel resource limit before launching untrusted code.
bool set_limit(int resource, rlim_t value, const char *name);
// Close inherited descriptors that must not reach the sandboxed command.
void close_inherited_descriptors();
// Translate a wait status into the helper's process exit code.
int child_exit_status(int status);
// Stop and reap the probe service while preserving the requested result code.
int terminate_service(pid_t child, int result);
// Choose an available loopback port for the service readiness probe.
bool allocate_probe_port(unsigned short &port);
// Run the trusted browser acceptance probe with the supplied capabilities.
int run_browser_probe(const options_t &options);
// Wait for HTTP readiness, optionally run browser acceptance, then stop the
// service.
int probe_http_service(pid_t child, unsigned short port,
    const std::string &path, const options_t *options = NULL);
#ifdef __APPLE__
// Check whether the requested local service probe port can be bound.
bool probe_port_available(unsigned short port);
// Return the parent component of a filesystem path.
std::string parent_directory(const std::string &path);
// Build the macOS sandbox profile for the granted paths and network policy.
std::string macos_profile(const std::string &user_root,
    const std::string &project_root, const std::string &toolchain_root,
    const std::string &toolchain_alias_root, bool loopback_network,
    bool outbound_network, const std::vector<std::string> &readonly_roots);
#endif
#ifdef __linux__
// Set up Linux isolation and supervise the requested command.
int run_linux_sandbox(const options_t &options, char *argv[],
    const std::string &project_root, const std::string &workdir);
#endif
#endif
}
