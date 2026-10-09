#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct sandbox_limits_t {
	// Independent wall-clock limit enforced by the broker.
	unsigned long timeout_ms = 30000;
	// Kernel-enforced limits passed to the isolated helper.
	unsigned long cpu_seconds = 20;
	unsigned long long memory_bytes = 1024ULL * 1024ULL * 1024ULL;
	// Real browser validation uses a multi-process engine. Keep enough room for
	// Chromium/Firefox while the delegated cgroup still provides a hard ceiling.
	unsigned long process_count = 256;
	// Browser engines keep IPC pipes, shared libraries and page resources open
	// concurrently; 64 descriptors causes them to exit immediately after launch.
	unsigned long open_files = 512;
	unsigned long long file_size_bytes = 64ULL * 1024ULL * 1024ULL;
	unsigned long output_bytes = 1024UL * 1024UL;
};

// Trusted server policy entry. The browser/model selects only `id`; executable
// and fixed arguments always come from the C++ project toolchain catalog.
struct sandbox_command_t {
	std::string id;
	std::string executable;
	std::vector<std::string> fixed_arguments;
	bool allow_dynamic_arguments = false;
	// Long-lived preview services may bind and connect only through the isolated
	// loopback interface. Ordinary build/test commands keep all networking off.
	bool allow_loopback_network = false;
	bool allow_outbound_network = false;
	// Non-zero for a trusted long-running service smoke test. The isolated helper
	// waits for an HTTP success response and then reaps the complete service tree.
	unsigned short http_probe_port = 0;
	std::string http_probe_path = "/";
	// Trusted host runner, not a project supplied executable.
	std::string browser_probe_node;
	std::string browser_evidence_root;
};

// Untrusted request. Every production language policy rejects arguments and
// uses the project root as working_directory; fields remain for future
// explicitly reviewed policies.
struct sandbox_request_t {
	std::string command_id;
	std::vector<std::string> arguments;
	std::string working_directory;
};

// Bounded in-memory result. stdout/stderr are returned to the confirming HTTP
// request but are deliberately omitted from persistent sandbox audit records.
struct sandbox_result_t {
	bool started = false;
	// User cancellation is distinct from resource-policy termination.
	bool cancelled = false;
	bool timed_out = false;
	bool memory_limit_exceeded = false;
	bool process_limit_exceeded = false;
	bool output_truncated = false;
	int exit_code = -1;
	int signal = 0;
	unsigned long long elapsed_ms = 0;
	std::string standard_output;
	std::string standard_error;
	std::string error;
};

// A policy-first process broker. Callers select a trusted command ID and pass
// an argv vector; command strings are never interpreted by a shell.
// The service launches a separate helper with posix_spawn so the multithreaded
// server never performs complex sandbox setup between fork and exec.
class program_sandbox_t {
public:
	program_sandbox_t(const std::string &user_root,
			  const std::string &project_path,
			  const std::vector<sandbox_command_t> &commands,
			  const sandbox_limits_t &limits = sandbox_limits_t(),
			  const std::string &helper_executable = "",
			  const std::vector<std::string> &readonly_roots = {});

	bool validate(const sandbox_request_t &request, std::string &err) const;
	bool execute(const sandbox_request_t &request, sandbox_result_t &result,
		     const std::atomic<bool> *cancel_requested = NULL,
		     const std::function<bool()> &should_cancel = {}) const;

	// Fail-closed capability probe. A false result must never fall back to an
	// ordinary child process.
	static bool backend_available(std::string &reason);

	// Read-only check of the trusted browser distribution beside the helper
	// on POSIX, or beside the application on Windows.
	// An empty engine directory is not an installed runtime. No browser starts.
	static bool
	browser_runtime_available(std::string &reason,
				  const std::string &helper_executable = "");

private:
	bool resolve_request(const sandbox_request_t &request,
			     std::string &project_root, std::string &workdir,
			     const sandbox_command_t *&command,
			     std::string &err) const;

	std::string user_root_;
	std::string project_path_;
	std::vector<sandbox_command_t> commands_;
	sandbox_limits_t limits_;
	std::string helper_executable_;
	std::vector<std::string> readonly_roots_;
};

} // namespace ai
} // namespace webcool
