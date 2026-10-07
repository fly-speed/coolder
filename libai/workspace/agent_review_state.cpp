#include "stdafx.h"
#include "agent_review_state.h"
#include "agent_workspace.h"

namespace webcool {
namespace ai {

std::string agent_review_draft_hash(const agent_change_proposal_t& change) {
	return agent_workspace_t::content_sha256(change.operation + "\n"
		+ change.path + "\n" + change.target_path + "\n" + change.content);
}

void collapse_agent_review_changes(
	std::vector<agent_change_proposal_t>& changes)
{
	std::vector<agent_change_proposal_t> latest;
	latest.reserve(changes.size());
	for (size_t i = 0; i < changes.size(); ++i) {
		// A later entry is the cumulative file image. Skip this intermediate
		// snapshot, but carry its formal baseline forward if the later transport
		// entry omitted preview metadata.
		size_t last = i;
		for (size_t j = i + 1; j < changes.size(); ++j) {
			if (changes[j].path == changes[i].path) last = j;
		}
		if (last != i) {
			if (!changes[last].original_content_available
				&& changes[i].original_content_available)
			{
				changes[last].original_content = changes[i].original_content;
				changes[last].original_content_available = true;
				changes[last].creates_file = changes[i].creates_file;
			}
			if (changes[last].base_hash.empty()) {
				changes[last].base_hash = changes[i].base_hash;
			}
			continue;
		}
		latest.push_back(changes[i]);
	}
	changes.swap(latest);
}

void assign_agent_review_generations(
	const std::vector<agent_change_proposal_t>& previous,
	std::vector<agent_change_proposal_t>& current)
{
	// Enforce the file-level invariant at the shared review boundary, even if a
	// provider batch or restored legacy checkpoint supplied duplicate paths.
	collapse_agent_review_changes(current);
	for (size_t i = 0; i < current.size(); ++i) {
		const std::string draft_hash = agent_review_draft_hash(current[i]);
		const agent_change_proposal_t* prior = NULL;
		for (size_t j = 0; j < previous.size(); ++j) {
			if (previous[j].path == current[i].path) {
				// Prefer the newest generation when reading an older artifact that
				// still contains several snapshots for one path.
				if (prior == NULL || previous[j].generation >= prior->generation) {
					prior = &previous[j];
				}
			}
		}
		if (prior != NULL && prior->draft_hash == draft_hash
			&& prior->generation > 0)
		{
			current[i].generation = prior->generation;
			current[i].review_status = prior->review_status.empty()
				? "pending" : prior->review_status;
		} else {
			current[i].generation = prior != NULL && prior->generation > 0
				? prior->generation + 1 : 1;
			current[i].review_status = "pending";
		}
		current[i].draft_hash = draft_hash;
		if (current[i].original_content_available) {
			current[i].base_hash = agent_workspace_t::content_sha256(
				current[i].original_content);
		} else if (prior != NULL) {
			current[i].base_hash = prior->base_hash;
		}
	}
}

size_t remove_resolved_agent_review_changes(
	const std::vector<agent_change_proposal_t>& review_snapshot,
	std::vector<agent_change_proposal_t>& active_changes)
{
	std::vector<agent_change_proposal_t> pending;
	pending.reserve(active_changes.size());
	size_t removed = 0;
	for (size_t i = 0; i < active_changes.size(); ++i) {
		bool resolved = false;
		for (size_t j = 0; j < review_snapshot.size(); ++j) {
			if (review_snapshot[j].path != active_changes[i].path
				|| review_snapshot[j].generation != active_changes[i].generation
				|| review_snapshot[j].draft_hash != active_changes[i].draft_hash)
			{
				continue;
			}
			resolved = review_snapshot[j].review_status == "accepted"
				|| review_snapshot[j].review_status == "rejected";
			break;
		}
		if (resolved) ++removed;
		else pending.push_back(active_changes[i]);
	}
	if (removed > 0) active_changes.swap(pending);
	return removed;
}

} // namespace ai
} // namespace webcool
