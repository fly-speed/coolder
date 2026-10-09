#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct agent_run_record_t {
	// Random public identifier used by status, SSE and cancellation endpoints.
	std::string id;
	std::string agent_id;
	std::string agent_version;
	std::string status;
	std::string provider_id;
	std::string model;
	std::string project_path;
	long long started_at = 0;
	long long finished_at = 0;
	long long input_tokens = 0;
	long long cached_input_tokens = 0;
	long long output_tokens = 0;
	long long reasoning_tokens = 0;
	long long latency_ms = 0;
	long long tool_calls = 0;
	long long proposed_changes = 0;
	long long rejected_changes = 0;
	std::string provider_error_category;
	int provider_http_status = 0;
	bool provider_error_retryable = false;
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
	explicit agent_run_store_t(const std::string &user_root);

	bool create(const agent_run_record_t &record, std::string &err) const;
	bool complete(const std::string &id, long long input_tokens,
		      long long cached_input_tokens, long long output_tokens,
		      long long reasoning_tokens, long long latency_ms,
		      long long tool_calls, long long proposed_changes,
		      long long rejected_changes, std::string &err) const;
	bool fail(const std::string &id, const std::string &error,
		  std::string &err) const;
	bool fail(const std::string &id, const std::string &error,
		  const std::string &provider_error_category,
		  int provider_http_status, bool provider_error_retryable,
		  std::string &err) const;
	bool cancel(const std::string &id, std::string &err) const;
	bool get(const std::string &id, agent_run_record_t &record,
		 std::string &err) const;
	bool list(size_t limit, std::vector<agent_run_record_t> &records,
		  std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
