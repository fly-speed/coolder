#pragma once

#include "../agent/agent_protocol.h"
#include "../context/agent_context_limits.h"

#include <string>

namespace webcool {
namespace ai {



struct agent_progress_tool_output_t {
	std::string call_id;
	std::string output;
};

// Recoverable coding-agent state stored inside the selected project. Unlike the
// run audit, this intentionally contains model context and must remain private.
struct agent_progress_t {
	// Durable review ledger owning the generations captured by this checkpoint.
	std::string source_run_id;
	std::string provider_id;
	std::string provider_model;
	std::string project_path;
	std::string session_id;
	std::string original_prompt;
	std::string transcript;
	std::string reasoning;
	std::string last_error;
	// Opaque Responses continuation state. Persisting it makes a service restart
	// resume the provider-native chain rather than replaying a lossy text summary.
	std::string provider_response_id;
	bool provider_response_pending = false;
	std::vector<agent_progress_tool_output_t> provider_tool_outputs;
	// Durable private review state is stored independently from the model
	// transcript. This allows context compaction and crash recovery without
	// embedding complete generated files repeatedly in subsequent prompts.
	std::vector<agent_change_proposal_t> staged_changes;
	size_t completed_tool_calls = 0;
	long long updated_at = 0;
};

class agent_progress_store_t {
public:
	agent_progress_store_t(const std::string& user_root,
		const std::string& project_path, const std::string& session_id);

	bool save(const agent_progress_t& progress, std::string& err) const;
	bool load(agent_progress_t& progress, bool& found, std::string& err) const;
	// Cheap metadata probe used by the conversation list. It deliberately does
	// not parse or return the private prompt/transcript stored in the checkpoint.
	bool exists(bool& found, std::string& err) const;
	bool remove(std::string& err) const;
	std::string relative_path() const;

private:
	std::string user_root_;
	std::string project_path_;
	std::string session_id_;
};

} // namespace ai
} // namespace webcool
