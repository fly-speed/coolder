#pragma once

#include "../agent/agent_protocol.h"

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Durable output of one completed coding-agent run. The artifact lives inside
// the selected project rather than the transient runtime map, so a restart or
// result-retention timeout cannot discard generated file contents.
struct agent_result_t {
	// Identifier of the associated agent run.
	std::string run_id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Identifier of the owning conversation session.
	std::string session_id;
	// Text content presented or retained by this record.
	std::string text;
	// Reasoning text kept separately from the user-facing answer.
	std::string reasoning;
	// Concise user-facing summary of completed work.
	std::string completion_summary;
	// task_contract_json: Serialized task contract retained for later
	// validation.
	// task_acceptance_json: Serialized acceptance evidence retained with
	// the task result.
	// task_acceptance_status: Current outcome of the task acceptance
	// checks.
	std::string task_contract_json, task_acceptance_json,
	    task_acceptance_status;
	// File proposals associated with this result or running task.
	std::vector<agent_change_proposal_t> changes;
	// Number of proposals rejected by validation.
	size_t rejected_changes = 0;
	// Final changes remain proposals until explicit user acceptance. These fields
	// distinguish a committed review result from a retained pending proposal.
	bool changes_applied = false;
	// Time when the reviewed changes were applied, in epoch seconds.
	long long changes_applied_at = 0;
	// Diagnostic from the last attempt to apply reviewed changes.
	std::string changes_apply_error;
	// pending, accepted, or rejected. This is user feedback about the generation;
	// rejecting does not delete the durable artifact or automatically undo files.
	std::string decision;
	// Persistence time as seconds since the Unix epoch.
	long long saved_at = 0;
};

// Decision bound to one path, generation and proposed content hash.
struct agent_change_review_t {
	// Path of the file or resource associated with this record.
	std::string path;
	// Revision number identifying an immutable proposal generation.
	unsigned long long generation = 0;
	// Digest identifying this exact proposed content revision.
	std::string draft_hash;
	// Review decision supplied for the identified proposal.
	std::string decision;
	// Optional line-review resolution. These fields are transport data for the
	// authenticated review endpoint; the result store deliberately validates and
	// persists only the generation-bound decision.
	std::string resolved_operation;
	// Complete replacement text obtained by applying the patch.
	std::string resolved_content;
};

// Reviewable result waiting for the user's apply or rejection decision.
struct agent_pending_result_t {
	// Identifier of the associated agent run.
	std::string run_id;
	// Persistence time as seconds since the Unix epoch.
	long long saved_at = 0;
	// Number of outstanding proposals or operations.
	size_t pending_count = 0;
};

// Persists and retrieves agent result records within the configured storage
// scope.
class agent_result_store_t {
public:
	// Bind the agent result store to the supplied storage scope.
	agent_result_store_t(
	    const std::string &user_root, const std::string &project_path);

	// Saving a newer streaming snapshot preserves already committed review
	// decisions for the same immutable generation. A genuinely changed draft has
	// a different generation/hash and remains pending.
	bool save(const agent_result_t &result, std::string &err) const;
	// Load persisted data into the output record; report failures through
	// err.
	bool load(const std::string &run_id, agent_result_t &result,
	    bool &found, std::string &err) const;
	// Return unresolved records awaiting review; report failures through
	// err.
	bool list_pending(const std::string &session_id,
	    std::vector<agent_pending_result_t> &results,
	    std::string &err) const;
	// Persist the review decision for the identified result; report
	// failures through err.
	bool set_decision(const std::string &run_id,
	    const std::string &decision, agent_result_t &result,
	    std::string &err) const;
	// Persists per-generation review decisions. The caller supplies both the
	// generation and draft hash, preventing a stale browser from accepting a
	// newer edit that happens to use the same project path.
	bool set_change_reviews(const std::string &run_id,
	    const std::vector<agent_change_review_t> &reviews,
	    agent_result_t &result, std::string &err) const;
	// Return the store's path relative to its owning data root.
	std::string relative_path(const std::string &run_id) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
};

} // namespace ai
} // namespace webcool
