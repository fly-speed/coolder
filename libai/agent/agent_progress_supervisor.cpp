#include "stdafx.h"
#include "agent_progress_supervisor.h"

#include <algorithm>

namespace webcool {
namespace ai {

agent_progress_supervisor_t::agent_progress_supervisor_t(
	size_t max_no_progress_calls)
	: max_no_progress_calls_(std::max(static_cast<size_t>(2),
		max_no_progress_calls)),
	  max_without_draft_change_(max_no_progress_calls_ * 2),
	  replan_after_calls_(max_no_progress_calls_),
	  consecutive_no_progress_(0),
	  consecutive_without_draft_change_(0),
	  repeated_signature_count_(0) {}

agent_progress_decision_t agent_progress_supervisor_t::observe(
	const std::string& signature, bool tool_succeeded, bool draft_changed)
{
	return observe_many(std::vector<std::string>(1, signature), tool_succeeded,
		draft_changed);
}

agent_progress_decision_t agent_progress_supervisor_t::observe_many(
	const std::vector<std::string>& signatures, bool tool_succeeded, bool draft_changed)
{
	bool new_observation = false;
	std::string signature;
	for (size_t i = 0; i < signatures.size(); ++i) {
		signature += signatures[i] + "\n";
		if (tool_succeeded && completed_signatures_.insert(signatures[i]).second)
			new_observation = true;
	}
	if (draft_changed || new_observation) {
		consecutive_no_progress_ = 0;
		consecutive_without_draft_change_ = 0;
		repeated_signature_count_ = 0;
		last_signature_ = signature;
		return agent_progress_continue;
	}

	// New evidence is progress even for analysis-only tasks. The independent
	// total tool budget bounds discovery; repeated observations are bounded here.
	++consecutive_without_draft_change_;
	++consecutive_no_progress_;
	if (signature == last_signature_) ++repeated_signature_count_;
	else {
		last_signature_ = signature;
		repeated_signature_count_ = 1;
	}
	if (consecutive_no_progress_ >= max_no_progress_calls_
		|| consecutive_without_draft_change_ >= max_without_draft_change_) {
		return agent_progress_stop;
	}
	// Two identical unproductive calls are already enough evidence that the
	// current tactic is stuck, even when the global no-progress budget is larger.
	const size_t unproductive_replan_after = std::max(static_cast<size_t>(2),
		max_no_progress_calls_ / 2);
	if (repeated_signature_count_ >= 2
		|| consecutive_no_progress_ >= unproductive_replan_after
		|| consecutive_without_draft_change_ >= replan_after_calls_)
	{
		return agent_progress_replan;
	}
	return agent_progress_continue;
}

bool agent_progress_supervisor_t::observe_proposal_failures(
	const std::vector<std::string>& causes, bool draft_changed) {
	if (draft_changed) proposal_failure_causes_.clear();
	const std::set<std::string> unique(causes.begin(), causes.end());
	bool repeated = false;
	for (const auto& cause : unique) {
		if (!proposal_failure_causes_.insert(cause).second) repeated = true;
	}
	return repeated;
}

void agent_progress_supervisor_t::prime_recovered_read_only_loop() {
	// An empty checkpoint is not evidence of a stalled run.
	reset_after_external_progress();
}

void agent_progress_supervisor_t::reset_after_external_progress() {
	investigation_calls_ = 0;
	consecutive_no_progress_ = 0;
	consecutive_without_draft_change_ = 0;
	repeated_signature_count_ = 0;
	last_signature_.clear();
	completed_signatures_.clear();
	proposal_failure_causes_.clear();
}

void agent_progress_supervisor_t::retain_read_observations(
	const std::vector<std::string>& retained)
{
	const std::set<std::string> available(retained.begin(), retained.end());
	for (auto it = completed_signatures_.begin(); it != completed_signatures_.end();) {
		if (it->compare(0, 5, "read:") == 0 && available.count(*it) == 0)
			it = completed_signatures_.erase(it);
		else ++it;
	}
}

void agent_progress_supervisor_t::observe_investigation(size_t tool_calls, bool draft_changed) {
	investigation_calls_ = draft_changed ? 0 : investigation_calls_ + tool_calls;
}

bool agent_progress_supervisor_t::investigation_checkpoint_due(size_t run_budget) const {
    return investigation_calls_ >= std::max<size_t>(1, std::min<size_t>(24, run_budget / 2));
}

size_t agent_progress_supervisor_t::investigation_calls() const {
	return investigation_calls_;
}

size_t agent_progress_supervisor_t::consecutive_no_progress() const {
	return consecutive_without_draft_change_;
}

} // namespace ai
} // namespace webcool
