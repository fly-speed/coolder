#pragma once

#include "program_sandbox.h"

#include <string>

namespace webcool
{
namespace ai
{

struct sandbox_execution_plan_t {
	// 128-bit random ID. The command and limits are a server-created snapshot.
	std::string id;
	std::string project_path;
	sandbox_command_t command;
	sandbox_limits_t limits;
	long long created_at = 0;
};

// One coalesced run reconstructed from the append-only metadata audit. Output
// streams are intentionally absent: they exist only in the live runtime cache.
struct sandbox_run_history_t {
	std::string id;
	std::string project_path;
	std::string command_id;
	std::string status;
	long long started_at = 0;
	long long finished_at = 0;
	int exit_code = -1;
	int signal = 0;
	unsigned long long elapsed_ms = 0;
	bool cancelled = false;
	bool timed_out = false;
	bool output_truncated = false;
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
	explicit sandbox_execution_store_t(const std::string &user_root);

	bool create(const std::string &project_path,
	    const std::string &command_id, sandbox_execution_plan_t &plan,
	    std::string &err) const;
	// consume() atomically removes authorization before revalidation, so failed
	// or interrupted execution cannot replay a previously confirmed plan.
	bool consume(const std::string &plan_id, sandbox_execution_plan_t &plan,
	    std::string &err) const;
	bool audit_started(
	    const sandbox_execution_plan_t &plan, std::string &err) const;
	bool audit_finished(const sandbox_execution_plan_t &plan,
	    const sandbox_result_t &result, std::string &err) const;
	// Reconstructs newest-first run metadata. A started record without a matching
	// finish record is reported as interrupted after the process-local task is
	// gone (for example, following a service restart).
	bool list_history(size_t limit,
	    std::vector<sandbox_run_history_t> &runs, std::string &err) const;

	static long long lifetime_seconds();

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
