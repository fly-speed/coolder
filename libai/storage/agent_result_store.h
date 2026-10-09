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
	std::string run_id;
	std::string project_path;
	std::string session_id;
	std::string text;
	std::string reasoning;
	std::string completion_summary;
	std::string task_contract_json, task_acceptance_json,
	    task_acceptance_status;
	std::vector<agent_change_proposal_t> changes;
	size_t rejected_changes = 0;
	// Final changes remain proposals until explicit user acceptance. These fields
	// distinguish a committed review result from a retained pending proposal.
	bool changes_applied = false;
	long long changes_applied_at = 0;
	std::string changes_apply_error;
	// pending, accepted, or rejected. This is user feedback about the generation;
	// rejecting does not delete the durable artifact or automatically undo files.
	std::string decision;
	long long saved_at = 0;
};

struct agent_change_review_t {
	std::string path;
	unsigned long long generation = 0;
	std::string draft_hash;
	std::string decision;
	// Optional line-review resolution. These fields are transport data for the
	// authenticated review endpoint; the result store deliberately validates and
	// persists only the generation-bound decision.
	std::string resolved_operation;
	std::string resolved_content;
};

struct agent_pending_result_t {
	std::string run_id;
	long long saved_at = 0;
	size_t pending_count = 0;
};

class agent_result_store_t {
public:
	agent_result_store_t(
	    const std::string &user_root, const std::string &project_path);

	// Saving a newer streaming snapshot preserves already committed review
	// decisions for the same immutable generation. A genuinely changed draft has
	// a different generation/hash and remains pending.
	bool save(const agent_result_t &result, std::string &err) const;
	bool load(const std::string &run_id, agent_result_t &result,
	    bool &found, std::string &err) const;
	bool list_pending(const std::string &session_id,
	    std::vector<agent_pending_result_t> &results,
	    std::string &err) const;
	bool set_decision(const std::string &run_id,
	    const std::string &decision, agent_result_t &result,
	    std::string &err) const;
	// Persists per-generation review decisions. The caller supplies both the
	// generation and draft hash, preventing a stale browser from accepting a
	// newer edit that happens to use the same project path.
	bool set_change_reviews(const std::string &run_id,
	    const std::vector<agent_change_review_t> &reviews,
	    agent_result_t &result, std::string &err) const;
	std::string relative_path(const std::string &run_id) const;

private:
	std::string user_root_;
	std::string project_path_;
};

} // namespace ai
} // namespace webcool
