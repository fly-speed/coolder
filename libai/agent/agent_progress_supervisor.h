#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

enum agent_progress_decision_t {
	agent_progress_continue,
	agent_progress_replan,
	agent_progress_stop
};

// Deterministic guard around repeated tool observations. The signature includes
// result content, so newly discovered evidence counts as progress for analysis,
// diagnosis and verification without requiring artificial file modifications.
class agent_progress_supervisor_t {
public:
	explicit agent_progress_supervisor_t(size_t max_no_progress_calls);

	agent_progress_decision_t observe(const std::string &signature,
	    bool tool_succeeded, bool draft_changed);
	agent_progress_decision_t observe_many(
	    const std::vector<std::string> &signatures, bool tool_succeeded,
	    bool draft_changed);
	// Legacy recovery entry point; resets rather than guessing prior stagnation.
	void prime_recovered_read_only_loop();
	// Stop on the second occurrence of an unresolved proposal precondition.
	// Reads do not repair preconditions; a changed draft or external review resets them.
	bool observe_proposal_failures(
	    const std::vector<std::string> &causes, bool draft_changed);
	// A user review can update the formal workspace while a provider request is
	// in flight. Begin a fresh observation window after rebasing that draft.
	void reset_after_external_progress();
	// Compaction may evict source pages; rereading those is useful evidence.
	void retain_read_observations(const std::vector<std::string> &retained);
	size_t consecutive_no_progress() const;
	// Counts actual native calls, independently of new observations/compaction.
	void observe_investigation(size_t tool_calls, bool draft_changed);
	bool investigation_checkpoint_due(size_t run_budget = 48) const;
	size_t investigation_calls() const;

private:
	size_t investigation_calls_ = 0;
	size_t max_no_progress_calls_;
	size_t max_without_draft_change_;
	size_t replan_after_calls_;
	size_t consecutive_no_progress_;
	size_t consecutive_without_draft_change_;
	std::string last_signature_;
	size_t repeated_signature_count_;
	std::set<std::string> completed_signatures_;
	std::set<std::string> proposal_failure_causes_;
};

} // namespace ai
} // namespace webcool
