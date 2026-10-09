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
	// Initialize agent progress supervisor state from the supplied
	// arguments.
	explicit agent_progress_supervisor_t(size_t max_no_progress_calls);

	// Classify one tool outcome as progress, repetition or a required
	// intervention.
	agent_progress_decision_t observe(const std::string &signature,
	    bool tool_succeeded, bool draft_changed);
	// Combine a batch of tool signatures into one progress decision.
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
	// Return the current streak of unproductive tool calls.
	size_t consecutive_no_progress() const;
	// Counts actual native calls, independently of new observations/compaction.
	void observe_investigation(size_t tool_calls, bool draft_changed);
	// Report whether read-only investigation has reached its checkpoint
	// budget.
	bool investigation_checkpoint_due(size_t run_budget = 48) const;
	// Return the number of investigation calls since the last checkpoint.
	size_t investigation_calls() const;

private:
	// Read-only investigation calls since the last checkpoint.
	size_t investigation_calls_ = 0;
	// Consecutive unproductive calls allowed before intervention.
	size_t max_no_progress_calls_;
	// Maximum calls allowed without changing the staged draft.
	size_t max_without_draft_change_;
	// Unproductive-call threshold for requesting a revised plan.
	size_t replan_after_calls_;
	// Count of consecutive calls without useful progress.
	size_t consecutive_no_progress_;
	// Calls since the last staged content change.
	size_t consecutive_without_draft_change_;
	// Signature of the most recently observed tool operation.
	std::string last_signature_;
	// Consecutive repetitions of the last operation signature.
	size_t repeated_signature_count_;
	// Successful operation signatures already seen in this run.
	std::set<std::string> completed_signatures_;
	// Distinct proposal rejection causes used to detect loops.
	std::set<std::string> proposal_failure_causes_;
};

} // namespace ai
} // namespace webcool
