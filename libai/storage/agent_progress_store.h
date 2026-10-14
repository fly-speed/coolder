#pragma once

#include "../agent/agent_protocol.h"
#include "../context/agent_context_limits.h"

#include <string>

namespace libai
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

// Host-owned dispatch identity; model text never confirms its outcome.
struct agent_tool_effect_t {
    std::string key, name, arguments_sha256, state, output_sha256, reference;
    size_t completed_before=0, completed_after=0;
    // Frozen before dispatch for trusted receipt reconstruction. Empty IDs
    // mean transcript-only recovery, never invented provider identifiers.
    std::string response_id;
    std::vector<std::string> native_call_ids;
    size_t tool_call_count=0;
};

struct agent_validation_receipt_t {
    std::string effect_key, execution_revision, source_sha256, proposal_sha256, report_json;
};

// Recoverable coding-agent state stored inside the selected project. Unlike the
// run audit, this intentionally contains model context and must remain private.
struct agent_progress_t {
    agent_validation_receipt_t validation_receipt;
    // Host execution configuration fingerprint; empty only for legacy data.
    std::string execution_revision;
    std::vector<agent_tool_effect_t> tool_effects;
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
	// POSIX compare-and-save under the same record lease used by normal save
	// and remove. Requires an existing exact snapshot; returns its new digest.
	bool save_if_revision(const agent_progress_t &progress,
	    const std::string &expected_revision, std::string &revision,
	    std::string &err) const;
	// Load persisted data into the output record; report failures through
	// err.
	bool load(
	    agent_progress_t &progress, bool &found, std::string &err) const;
	// Digest of the exact validated file bytes, from the same bounded read.
	// Empty on missing/error. This is a snapshot identity, not a receipt.
	bool load_snapshot(agent_progress_t &progress, bool &found,
	    std::string &revision, std::string &err) const;
	// Cheap metadata probe used by the conversation list. It deliberately does
	// not parse or return the private prompt/transcript stored in the checkpoint.
	bool exists(bool &found, std::string &err) const;
	// Remove the identified saved record; report failures through err.
	bool remove(std::string &err) const;
	// Return the store's path relative to its owning data root.
	std::string relative_path() const;

private:
	bool save_impl(const agent_progress_t &progress,
	    const std::string *expected_revision, std::string &revision,
	    std::string &err) const;
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
	// Identifier of the owning conversation session.
	std::string session_id_;
};

} // namespace ai
} // namespace libai
