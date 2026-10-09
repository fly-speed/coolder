#pragma once

#include "program_sandbox.h"

#include <string>

namespace webcool
{
namespace ai
{

// Reviewed command, working directory and resource limits for execution.
struct sandbox_execution_plan_t {
	// 128-bit random ID. The command and limits are a server-created snapshot.
	std::string id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Trusted executable and argument policy selected for this plan.
	sandbox_command_t command;
	// Resource ceilings applied by the sandbox broker.
	sandbox_limits_t limits;
	// Creation time as seconds since the Unix epoch.
	long long created_at = 0;
};

// One coalesced run reconstructed from the append-only metadata audit. Output
// streams are intentionally absent: they exist only in the live runtime cache.
struct sandbox_run_history_t {
	// Identifier used to look up this record.
	std::string id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Identifier of the selected fixed sandbox command.
	std::string command_id;
	// Current lifecycle or outcome status.
	std::string status;
	// Start time as seconds since the Unix epoch.
	long long started_at = 0;
	// Finish time as seconds since the Unix epoch.
	long long finished_at = 0;
	// Process exit code reported by the sandbox helper.
	int exit_code = -1;
	// Signal responsible for terminating the process, when available.
	int signal = 0;
	// Elapsed stream or operation time in milliseconds.
	unsigned long long elapsed_ms = 0;
	// Whether cancellation ended or will end this operation.
	bool cancelled = false;
	// Whether the command exceeded its time limit.
	bool timed_out = false;
	// Whether captured output exceeded its configured byte limit.
	bool output_truncated = false;
	// Diagnostic explaining why the operation failed.
	std::string error;
};

// Stores a single short-lived, one-time sandbox authorization per user.
// The selected command is revalidated against the fixed toolchain catalog
// immediately after the plan is atomically consumed.
//
// Audit methods store metadata only. They never persist source files, prompts,
// API keys or stdout/stderr.
class sandbox_execution_store_t {
public:
	// Bind the sandbox execution store to the supplied storage scope.
	explicit sandbox_execution_store_t(const std::string &user_root);

	// Create a new persistent record; report failures through err.
	bool create(const std::string &project_path,
	    const std::string &command_id, sandbox_execution_plan_t &plan,
	    std::string &err) const;
	// consume() atomically removes authorization before revalidation, so failed
	// or interrupted execution cannot replay a previously confirmed plan.
	bool consume(const std::string &plan_id, sandbox_execution_plan_t &plan,
	    std::string &err) const;
	// Record command-start metadata without persisting source or output
	// text.
	bool audit_started(
	    const sandbox_execution_plan_t &plan, std::string &err) const;
	// Record command outcome and resource-limit metadata in the sandbox
	// audit.
	bool audit_finished(const sandbox_execution_plan_t &plan,
	    const sandbox_result_t &result, std::string &err) const;
	// Reconstructs newest-first run metadata. A started record without a matching
	// finish record is reported as interrupted after the process-local task is
	// gone (for example, following a service restart).
	bool list_history(size_t limit,
	    std::vector<sandbox_run_history_t> &runs, std::string &err) const;

	// Return the validity period of a persisted preview or execution
	// plan.
	static long long lifetime_seconds();

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
