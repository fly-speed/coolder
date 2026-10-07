#pragma once

#include "../agent/agent_protocol.h"

#include <string>
#include <vector>

namespace webcool {
namespace ai {

// Returns the immutable identity of one proposed file generation. The hash
// includes the operation and both paths, so a move or delete cannot be mistaken
// for a write that happens to contain the same bytes.
std::string agent_review_draft_hash(const agent_change_proposal_t& change);

// Keeps one cumulative proposal per project path. Model providers may emit the
// same file more than once in a batch or an older recovery artifact may still
// contain intermediate snapshots. Review is a decision about the latest file
// image, so those intermediate images must never become separate confirmations.
void collapse_agent_review_changes(
	std::vector<agent_change_proposal_t>& changes);

// Assigns monotonically increasing generations after every model tool turn.
// An unchanged proposal retains its prior decision; any changed proposal is a
// new pending generation, even when the previous generation was accepted.
void assign_agent_review_generations(
	const std::vector<agent_change_proposal_t>& previous,
	std::vector<agent_change_proposal_t>& current);

// Removes generations that were accepted or rejected by a browser while the
// agent was waiting for the model. The immutable identity prevents a newer edit
// of the same path from being removed accidentally.
size_t remove_resolved_agent_review_changes(
	const std::vector<agent_change_proposal_t>& review_snapshot,
	std::vector<agent_change_proposal_t>& active_changes);

} // namespace ai
} // namespace webcool
