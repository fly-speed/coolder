#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Persistent run audit containing outcomes and usage rather than prompts.
struct agent_run_record_t {
	// Random public identifier used by status, SSE and cancellation endpoints.
	std::string id;
	// Registered agent selected for this run or session.
	std::string agent_id;
	// Version of the agent definition used for this run.
	std::string agent_version;
	// Current lifecycle or outcome status.
	std::string status;
	// Identifier of the configured model provider.
	std::string provider_id;
	// Provider model name used for generation.
	std::string model;
	// Logical path identifying the selected project.
	std::string project_path;
	// Start time as seconds since the Unix epoch.
	long long started_at = 0;
	// Finish time as seconds since the Unix epoch.
	long long finished_at = 0;
	// Provider-reported input token count.
	long long input_tokens = 0;
	// Input tokens served from the provider's prompt cache.
	long long cached_input_tokens = 0;
	// Provider-reported generated token count.
	long long output_tokens = 0;
	// Generated tokens attributed to provider reasoning.
	long long reasoning_tokens = 0;
	// Elapsed request time in milliseconds.
	long long latency_ms = 0;
	// Total number of tool calls recorded in the completed run audit.
	long long tool_calls = 0;
	// Number of file changes proposed by the agent.
	long long proposed_changes = 0;
	// Number of proposals rejected by validation.
	long long rejected_changes = 0;
	// Normalized category of the failed provider request.
	std::string provider_error_category;
	// HTTP status retained for the failed provider request.
	int provider_http_status = 0;
	// Whether the recorded provider error permits retry.
	bool provider_error_retryable = false;
	// Diagnostic explaining why the operation failed.
	std::string error;
};

// Per-user metadata-only run history. Prompts, file contents, model replies and
// API keys are deliberately excluded from this audit store.
//
// The file is replaced atomically and bounded to the newest 100 records. All
// methods operate under a process mutex and validate POSIX ownership/mode rules
// before reading or replacing the file.
class agent_run_store_t {
public:
	// Bind the agent run store to the supplied storage scope.
	explicit agent_run_store_t(const std::string &user_root);

	// Create a new persistent record; report failures through err.
	bool create(const agent_run_record_t &record, std::string &err) const;
	// Record successful completion and accumulated usage; report failures
	// through err.
	bool complete(const std::string &id, long long input_tokens,
	    long long cached_input_tokens, long long output_tokens,
	    long long reasoning_tokens, long long latency_ms,
	    long long tool_calls, long long proposed_changes,
	    long long rejected_changes, std::string &err) const;
	// Record the failed run outcome and its diagnostic; report failures
	// through err.
	bool fail(const std::string &id, const std::string &error,
	    std::string &err) const;
	// Record the failed run outcome and its diagnostic; report failures
	// through err.
	bool fail(const std::string &id, const std::string &error,
	    const std::string &provider_error_category,
	    int provider_http_status, bool provider_error_retryable,
	    std::string &err) const;
	// Mark the identified run as cancelled; report failures through err.
	bool cancel(const std::string &id, std::string &err) const;
	// Read the record identified by the supplied key; report failures
	// through err.
	bool get(const std::string &id, agent_run_record_t &record,
	    std::string &err) const;
	// Read the stored records in this scope; report failures through err.
	bool list(size_t limit, std::vector<agent_run_record_t> &records,
	    std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
