#pragma once

#include "../agent/agent_protocol.h"
#include "../context/agent_context_limits.h"

#include <string>

namespace webcool
{
namespace ai
{

// Persisted native tool result used to resume a provider response.
struct agent_progress_tool_output_t {
	// Provider-issued identifier linking a tool result to its call.
	std::string call_id;
	// Result payload returned by the operation.
	std::string output;
};

// Recoverable coding-agent state stored inside the selected project. Unlike the
// run audit, this intentionally contains model context and must remain private.
struct agent_progress_t {
	// Durable review ledger owning the generations captured by this checkpoint.
	std::string source_run_id;
	// Identifier of the configured model provider.
	std::string provider_id;
	// Model name retained alongside provider status metadata.
	std::string provider_model;
	// Logical path identifying the selected project.
	std::string project_path;
	// Identifier of the owning conversation session.
	std::string session_id;
	// Original user request, retained across recovery and compaction.
	std::string original_prompt;
	// Working prompt transcript for the current invocation.
	std::string transcript;
	// Reasoning text kept separately from the user-facing answer.
	std::string reasoning;
	// Most recent diagnostic retained for status reporting.
	std::string last_error;
	// Opaque Responses continuation state. Persisting it makes a service restart
	// resume the provider-native chain rather than replaying a lossy text summary.
	std::string provider_response_id;
	// Whether a background response still needs retrieval or consumption.
	bool provider_response_pending = false;
	// Native tool results awaiting delivery to the provider.
	std::vector<agent_progress_tool_output_t> provider_tool_outputs;
	// Durable private review state is stored independently from the model
	// transcript. This allows context compaction and crash recovery without
	// embedding complete generated files repeatedly in subsequent prompts.
	std::vector<agent_change_proposal_t> staged_changes;
	// Number of tool calls completed before this checkpoint.
	size_t completed_tool_calls = 0;
	// Last update time as seconds since the Unix epoch.
	long long updated_at = 0;
};

// Persists and retrieves agent progress records within the configured storage
// scope.
class agent_progress_store_t {
public:
	// Bind the agent progress store to the supplied storage scope.
	agent_progress_store_t(const std::string &user_root,
	    const std::string &project_path, const std::string &session_id);

	// Persist the supplied record; report failures through err.
	bool save(const agent_progress_t &progress, std::string &err) const;
	// Load persisted data into the output record; report failures through
	// err.
	bool load(
	    agent_progress_t &progress, bool &found, std::string &err) const;
	// Cheap metadata probe used by the conversation list. It deliberately does
	// not parse or return the private prompt/transcript stored in the checkpoint.
	bool exists(bool &found, std::string &err) const;
	// Remove the identified saved record; report failures through err.
	bool remove(std::string &err) const;
	// Return the store's path relative to its owning data root.
	std::string relative_path() const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
	// Identifier of the owning conversation session.
	std::string session_id_;
};

} // namespace ai
} // namespace webcool
