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

#ifndef _WIN32
bool canonical_directory(const std::string &input, std::string &output);
bool path_is_within(const std::string &root, const std::string &path);
bool set_limit(int resource, rlim_t value, const char *name);
void close_inherited_descriptors();
int child_exit_status(int status);
int terminate_service(pid_t child, int result);
bool allocate_probe_port(unsigned short &port);
int run_browser_probe(const options_t &options);
int probe_http_service(pid_t child, unsigned short port,
    const std::string &path, const options_t *options = NULL);
#ifdef __APPLE__
bool probe_port_available(unsigned short port);
std::string parent_directory(const std::string &path);
std::string macos_profile(const std::string &user_root,
    const std::string &project_root, const std::string &toolchain_root,
    const std::string &toolchain_alias_root, bool loopback_network,
    bool outbound_network, const std::vector<std::string> &readonly_roots);
#endif
#ifdef __linux__
int run_linux_sandbox(const options_t &options, char *argv[],
    const std::string &project_root, const std::string &workdir);
#endif
#endif
}
