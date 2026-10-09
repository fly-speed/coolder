#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Resource ceilings enforced when executing an external command.
struct sandbox_limits_t {
	// Independent wall-clock limit enforced by the broker.
	unsigned long timeout_ms = 30000;
	// Kernel-enforced limits passed to the isolated helper.
	unsigned long cpu_seconds = 20;
	// Maximum memory allowance in bytes.
	unsigned long long memory_bytes = 1024ULL * 1024ULL * 1024ULL;
	// Real browser validation uses a multi-process engine. Keep enough room for
	// Chromium/Firefox while the delegated cgroup still provides a hard ceiling.
	unsigned long process_count = 256;
	// Browser engines keep IPC pipes, shared libraries and page resources open
	// concurrently; 64 descriptors causes them to exit immediately after launch.
	unsigned long open_files = 512;
	// Maximum size in bytes of an individual created file.
	unsigned long long file_size_bytes = 64ULL * 1024ULL * 1024ULL;
	// Maximum number of captured command-output bytes.
	unsigned long output_bytes = 1024UL * 1024UL;
};

// Trusted server policy entry. The browser/model selects only `id`; executable
// and fixed arguments always come from the C++ project toolchain catalog.
struct sandbox_command_t {
	// Identifier used to look up this record.
	std::string id;
	// Resolved program path to execute.
	std::string executable;
	// Server-defined arguments for this command.
	std::vector<std::string> fixed_arguments;
	// Whether callers may append permitted dynamic arguments.
	bool allow_dynamic_arguments = false;
	// Long-lived preview services may bind and connect only through the isolated
	// loopback interface. Ordinary build/test commands keep all networking off.
	bool allow_loopback_network = false;
	// Whether this command requests outbound network access.
	bool allow_outbound_network = false;
	// Non-zero for a trusted long-running service smoke test. The isolated helper
	// waits for an HTTP success response and then reaps the complete service tree.
	unsigned short http_probe_port = 0;
	// HTTP route used to verify a launched service.
	std::string http_probe_path = "/";
	// Trusted host runner, not a project supplied executable.
	std::string browser_probe_node;
	// Directory used to exchange browser probe evidence.
	std::string browser_evidence_root;
};

// Untrusted request. Every production language policy rejects arguments and
// uses the project root as working_directory; fields remain for future
// explicitly reviewed policies.
struct sandbox_request_t {
	// Identifier of the selected fixed sandbox command.
	std::string command_id;
	// Arguments passed to the selected command or tool.
	std::vector<std::string> arguments;
	// Workspace-relative directory in which to run the command.
	std::string working_directory;
};

// Bounded in-memory result. stdout/stderr are returned to the confirming HTTP
// request but are deliberately omitted from persistent sandbox audit records.
struct sandbox_result_t {
	// Whether the operation has entered its running state.
	bool started = false;
	// User cancellation is distinct from resource-policy termination.
	bool cancelled = false;
	// Whether the command exceeded its time limit.
	bool timed_out = false;
	// Whether execution exceeded the memory allowance.
	bool memory_limit_exceeded = false;
	// Whether execution exceeded the process-count allowance.
	bool process_limit_exceeded = false;
	// Whether captured output exceeded its configured byte limit.
	bool output_truncated = false;
	// Process exit code reported by the sandbox helper.
	int exit_code = -1;
	// Signal responsible for terminating the process, when available.
	int signal = 0;
	// Elapsed stream or operation time in milliseconds.
	unsigned long long elapsed_ms = 0;
	// Captured standard output, bounded by the output limit.
	std::string standard_output;
	// Captured standard error, bounded by the output limit.
	std::string standard_error;
	// Diagnostic explaining why the operation failed.
	std::string error;
};

// A policy-first process broker. Callers select a trusted command ID and pass
// an argv vector; command strings are never interpreted by a shell.
// The service launches a separate helper with posix_spawn so the multithreaded
// server never performs complex sandbox setup between fork and exec.
class program_sandbox_t {
public:
	// Initialize program sandbox state from the supplied arguments.
	program_sandbox_t(const std::string &user_root,
	    const std::string &project_path,
	    const std::vector<sandbox_command_t> &commands,
	    const sandbox_limits_t &limits = sandbox_limits_t(),
	    const std::string &helper_executable = "",
	    const std::vector<std::string> &readonly_roots = {});

	// Validate command identity, arguments and workspace paths before
	// execution.
	bool validate(const sandbox_request_t &request, std::string &err) const;
	// Run the validated sandbox command with bounded output and
	// cancellation checks.
	bool execute(const sandbox_request_t &request, sandbox_result_t &result,
	    const std::atomic<bool> *cancel_requested = NULL,
	    const std::function<bool()> &should_cancel = {}) const;

	// Fail-closed capability probe. A false result must never fall back to an
	// ordinary child process.
	static bool backend_available(std::string &reason);

	// Read-only check of the trusted browser distribution beside the helper
	// on POSIX, or beside the application on Windows.
	// An empty engine directory is not an installed runtime. No browser starts.
	static bool browser_runtime_available(
	    std::string &reason, const std::string &helper_executable = "");

private:
	// Resolve and validate sandbox paths before launching the helper.
	bool resolve_request(const sandbox_request_t &request,
	    std::string &project_root, std::string &workdir,
	    const sandbox_command_t *&command, std::string &err) const;

	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
	// Trusted command definitions available to the sandbox broker.
	std::vector<sandbox_command_t> commands_;
	// Resource ceilings applied by the sandbox broker.
	sandbox_limits_t limits_;
	// Path of the isolated sandbox helper executable.
	std::string helper_executable_;
	// Additional filesystem roots granted read-only access.
	std::vector<std::string> readonly_roots_;
};

} // namespace ai
} // namespace webcool
