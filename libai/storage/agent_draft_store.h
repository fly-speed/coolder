#pragma once

#include "../agent/agent_protocol.h"

#include <string>
#include <functional>
#include <map>
#include <vector>

namespace webcool
{
namespace ai
{

// Durable, per-run private project tree. The model and validation sandbox read
// this tree while the formal virtual-disk project remains unchanged until an
// authenticated review accepts an exact change generation.
class agent_draft_store_t {
public:
	// Bind the agent draft store to the supplied storage scope.
	agent_draft_store_t(const std::string &user_root,
	    const std::string &project_path, const std::string &run_id);

	// Reuses a verified unchanged tree; compatible write-only revisions update
	// changed files with rollback and invalidate old compiler outputs. Structural
	// edits or untrusted snapshots rebuild from the formal baseline plus all
	// accumulated proposals, then install with a directory rename. An interrupted
	// incremental update leaves an invalid snapshot that must be reconstructed.
	bool materialize(const std::vector<agent_change_proposal_t> &changes,
	    size_t &skipped_files, std::string &err, bool *reused = NULL,
	    std::string *source_fingerprint = NULL,
	    const std::function<bool()> &should_cancel = {}) const;
	// Remove the identified saved record; report failures through err.
	bool remove(std::string &err) const;
	// Detach a fully reviewed tree to a fresh identity before background cleanup.
	// A resumed run can recreate its original identity without being deleted.
	bool retire(const std::string &cleanup_id, bool &detached,
	    std::string &err) const;
	// Removes abandoned sibling worktrees after the recovery retention period.
	// Active/recent runs are never selected and symlinks are not traversed.
	bool cleanup_stale(long long max_age_seconds, std::string &err) const;

	// Resolve only server-created package links; callers grant these read access.
	bool readonly_dependencies(
	    std::map<std::string, std::string> &mounts, std::string &err) const;
	// Return the resolved filesystem root managed by this object.
	std::string root_path() const;
	// Map a project path into the private draft, rejecting paths outside
	// it.
	bool to_draft_path(const std::string &project_relative_user_path,
	    std::string &draft_path) const;
	// Map a private draft path back to its logical project path.
	std::string to_project_path(const std::string &draft_path) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
	// Identifier of the associated agent run.
	std::string run_id_;
};

} // namespace ai
} // namespace webcool
