#include "stdafx.h"
#include "agent_draft_store_internal.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "agent_draft_store.h"
#include "../workspace/agent_workspace.h"
#include "../project/project_toolchain.h"
#include "../common/ai_error_log.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#include <sys/utime.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/file.h>
#include <fcntl.h>
#endif

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <thread>
#include "fiber/fiber_base.h"
#include <sstream>
#include <sys/stat.h>

namespace webcool
{
namespace ai
{
namespace draft_store_detail
{

// One budget spans recursive traversal; per-file budgets would never yield
// when a large tree consists of many small files.

using ::webcool::ai::file_ops::join_path;

}
using namespace draft_store_detail;
// namespace

agent_draft_store_t::agent_draft_store_t(const std::string &user_root,
    const std::string &project_path, const std::string &run_id)
        : user_root_(user_root)
        , project_path_(project_path)
        , run_id_(run_id)
{
}

std::string agent_draft_store_t::root_path() const
{
	std::string project_root;
	std::string ignored;
	if (agent_workspace_t::resolve_project_state_root(
	        user_root_, project_path_, project_root, ignored))
		return join_path(
		    join_path(
		        join_path(project_root, ".webcool_agent"), "worktrees"),
		    run_id_);
	return "";
}

bool agent_draft_store_t::to_draft_path(
    const std::string &path, std::string &draft_path) const
{
	if (project_path_.empty()) {
		draft_path = path;
		return true;
	}
	if (path == project_path_) {
		draft_path.clear();
		return true;
	}
	const std::string prefix = project_path_ + "/";
	if (path.compare(0, prefix.size(), prefix) != 0)
		return false;
	draft_path = path.substr(prefix.size());
	return true;
}

std::string agent_draft_store_t::to_project_path(
    const std::string &draft_path) const
{
	if (!project_path_.empty())
		return draft_path.empty() ?
		    project_path_ :
		    join_path(project_path_, draft_path);
	return draft_path;
}

bool agent_draft_store_t::readonly_dependencies(
    std::map<std::string, std::string> &mounts, std::string &err) const
{
	mounts.clear();
	std::vector<prebuilt_dependency_t> deps;
	agent_workspace_t source(user_root_);
	if (!source.prebuilt_dependencies(project_path_, deps, err))
		return false;
#ifndef _WIN32
	std::string project_root;
	if (!agent_workspace_t::resolve_project_state_root(
	        user_root_, project_path_, project_root, err))
		return false;
	const std::string prefix =
	    project_root + "/.webcool_agent/dependencies/";
	for (const auto &dep : deps) {
		char target[4096];
		const ssize_t size =
		    readlink((root_path() + "/" + dep.root).c_str(), target,
		        sizeof(target));
		if (size <= 0) {
			struct stat st;
			// Existing drafts created before a declaration keep their isolated
			// ordinary directory until materialize replaces it with a mount.
			if (lstat((root_path() + "/" + dep.root).c_str(),
			        &st) == 0 &&
			    S_ISDIR(st.st_mode))
				continue;
			err = "missing read-only dependency mount: " + dep.root;
			return false;
		}
		const std::string path(target, static_cast<size_t>(size));
		if (path.compare(0, prefix.size(), prefix) != 0 ||
		    path.size() != prefix.size() + 64 ||
		    path.substr(prefix.size())
		            .find_first_not_of("0123456789abcdef") !=
		        std::string::npos) {
			err = "invalid read-only dependency mount";
			return false;
		}
		mounts[dep.root] = path;
	}
#endif
	return true;
}

static bool check_readonly_dependency_changes(const agent_draft_store_t &store,
    const prebuilt_dependency_t &dep,
    const std::vector<agent_change_proposal_t> &changes, std::string &err)
{
	for (const auto &change : changes)
		for (const auto *input :
		    { &change.path, &change.target_path }) {
			if (input->empty())
				continue;
			std::string relative;
			if (!store.to_draft_path(*input, relative))
				return false;
			if (!(dep_inside(dep.root, relative) ||
			        dep_inside(relative, dep.root)))
				continue;
			err = "prebuilt dependency is read-only: " + dep.root +
			    "; change its declaration explicitly before editing or rebuilding it";
			return false;
		}

	return true;
}

static bool check_readonly_changes(const agent_draft_store_t &store,
    const std::vector<prebuilt_dependency_t> &dependencies,
    const std::vector<agent_change_proposal_t> &changes,
    std::vector<std::string> &excluded, std::string &err)
{
	for (const auto &dep : dependencies) {
		excluded.push_back(dep.root);
		if (!check_readonly_dependency_changes(
		        store, dep, changes, err))
			return false;
	}
	return true;
}

static void save_draft_snapshot(const std::string &run_id,
    const std::string &target, const std::string &baseline,
    const std::string &overlay_hash, size_t skipped_files,
    const std::string &manifest, agent_workspace_t &records)
{
	// An unchanged tree returned above retains its build directory. Changed
	// revisions rebuild conservatively: reverting to formal bytes may restore
	// older mtimes, so carrying old object files forward could hide the revert.
	if (!baseline.empty()) {
		std::string draft_hash, state_error;
		bool hashed = false;
		{
			draft_phase_t phase(run_id, "snapshot_fingerprint");
			hashed = phase.ok =
			    agent_workspace_t(target).tree_sha256(
			        "", draft_hash, state_error);
		}
		if (hashed) {
			std::ostringstream record;
			record << baseline << "\n"
			       << overlay_hash << "\n"
			       << draft_hash << "\n"
			       << skipped_files << "\n"
			       << manifest << "\n";
			if (!records.save_generated_text(run_id + ".snapshot",
			        record.str(), state_error))
				ai_log_error("agent.draft", "save-reuse-state",
				    state_error);
		}
	}
}

static bool validate_draft_identity(const std::string &run_id_,
    const std::string &project_path_, std::string &err)
{
	std::string normalized_project;
	if (!(!valid_run_id(run_id_) ||
	        !agent_workspace_t::normalize_path(
	            project_path_, normalized_project, true, err) ||
	        normalized_project != project_path_))
		return true;
	if (!err.empty())
		return ai_error("agent.draft", "validate-identity", err);
	err = "invalid private draft identity";
	return ai_error("agent.draft", "validate-identity", err);
}

bool agent_draft_store_t::materialize(
    const std::vector<agent_change_proposal_t> &changes, size_t &skipped_files,
    std::string &err, bool *reused, std::string *source_fingerprint,
    const std::function<bool()> &should_cancel) const
{
	if (reused)
		*reused = false;
	if (source_fingerprint)
		source_fingerprint->clear();
	if (!validate_draft_identity(run_id_, project_path_, err))
		return false;
	std::string project_root;
	if (!agent_workspace_t::resolve_project_state_root(
	        user_root_, project_path_, project_root, err))
		return false;
	const std::string metadata = join_path(project_root, ".webcool_agent");
	const std::string worktrees = join_path(metadata, "worktrees");
	const std::string target = root_path();
	const std::string next = target + ".next";
	const std::string old = target + ".old";
	agent_workspace_t source(user_root_);
	std::vector<prebuilt_dependency_t> dependencies;
	if (!source.prebuilt_dependencies(project_path_, dependencies, err))
		return false;
	std::vector<std::string> excluded;
	if (!check_readonly_changes(
	        *this, dependencies, changes, excluded, err))
		return false;
	std::map<std::string, std::string> mounts;
	{
		draft_phase_t phase(run_id_, "prepare_dependencies");
		if (!(phase.ok = prepare_dependencies(user_root_, project_path_,
		          metadata, dependencies, mounts, phase.budget, err,
		          should_cancel)))
			return false;
	}
	std::string baseline, fingerprint_error;
	{
		draft_phase_t phase(run_id_, "source_fingerprint");
		phase.ok = source.tree_sha256(
		    project_path_, baseline, fingerprint_error);
	}
	if (source_fingerprint)
		*source_fingerprint = baseline;
	std::string overlay;
	for (const auto &change : changes)
		for (const auto *value : { &change.operation, &change.path,
		         &change.target_path, &change.content })
			overlay += std::to_string(value->size()) + ":" + *value;
	const std::string overlay_hash =
	    agent_workspace_t::content_sha256(overlay);
	std::string saved_baseline, saved_overlay, saved_draft, saved_record,
	    saved_manifest;
	const std::string manifest = write_manifest(*this, changes);
	size_t saved_skipped = 0;
	bool record_truncated = false;
	struct stat target_st, state_st;
	const bool target_exists = lstat(target.c_str(), &target_st) == 0 &&
	    S_ISDIR(target_st.st_mode) && !S_ISLNK(target_st.st_mode);
	agent_workspace_t records(worktrees);
	if (lstat((target + ".snapshot").c_str(), &state_st) == 0 &&
	    records.read(run_id_ + ".snapshot", saved_record, record_truncated,
	        fingerprint_error) &&
	    !record_truncated) {
		std::istringstream record(saved_record);
		record >> saved_baseline >> saved_overlay >> saved_draft >>
		    saved_skipped;
		if (!record)
			saved_baseline.clear();
		else {
			std::string ignored;
			std::getline(record, ignored);
			std::getline(record, saved_manifest);
		}
	}
	bool trusted_previous = false;
	if (target_exists && !baseline.empty() && baseline == saved_baseline) {
		std::string current_draft;
		{
			draft_phase_t phase(run_id_, "draft_fingerprint");
			phase.ok = agent_workspace_t(target).tree_sha256(
			    "", current_draft, fingerprint_error);
			trusted_previous = phase.ok &&
			    current_draft == saved_draft &&
			    dependency_links(target, mounts);
		}
		if (trusted_previous && saved_overlay == overlay_hash) {
			skipped_files = saved_skipped;
			logger(
			    "AI component=agent.draft event=materialize_decision run_id=%s mode=reuse reason=unchanged",
			    run_id_.c_str());
			if (!reused)
				return true;
			*reused = true;
			return true;
		}
	}

	if (trusted_previous && !manifest.empty() && !saved_manifest.empty()) {
		bool attempted = false;
		if (update_verified_writes(*this, records, run_id_, target,
		        baseline, overlay_hash, saved_manifest, manifest,
		        changes, saved_skipped, attempted, err)) {
			skipped_files = saved_skipped;
			return true;
		}
		if (attempted)
			return false;
	}

	const char *reason = !target_exists ? "missing_draft" :
	    baseline.empty()                ? "fingerprint_unavailable" :
	    baseline != saved_baseline ? "source_changed_or_snapshot_missing" :
	    !trusted_previous          ? "draft_changed" :
	                                 "revision_changed";
	logger(
	    "AI component=agent.draft event=materialize_decision run_id=%s mode=rebuild reason=%s changes=%zu",
	    run_id_.c_str(), reason, changes.size());
	if (!create_private_directory(metadata, err) ||
	    !create_private_directory(worktrees, err))
		return false;
	std::string stale_err;
	if (!cleanup_stale(7 * 24 * 60 * 60, stale_err)) {
		// Stale sibling cleanup is maintenance, not a reason to discard the current
		// run. Keep the failure observable and continue its isolated materialization.
		ai_log_error(
		    "agent.draft", "cleanup-stale-worktrees", stale_err);
	}
	if (!remove_tree(next, err) || !remove_tree(old, err) ||
	    !create_private_directory(next, err))
		return false;
	agent_workspace_t draft(next);
	skipped_files = 0;
	long long dependency_bytes = 0;
	bool prepared = false;
	{
		draft_phase_t phase(run_id_, "copy_source");
		prepared = phase.ok = copy_build_tree(source, project_path_,
		    draft, "", skipped_files, dependency_bytes, err,
		    phase.budget, excluded);
	}
	logger(
	    "AI component=agent.draft event=copy_summary run_id=%s skipped_files=%zu dependency_bytes=%lld ok=%d",
	    run_id_.c_str(), skipped_files, dependency_bytes, prepared);
#ifndef _WIN32
	if (prepared)
		for (const auto &mount : mounts) {
			if (!(!dependency_parents(next, mount.first, err) ||
			        symlink(mount.second.c_str(),
			            (next + "/" + mount.first).c_str()) != 0))
				continue;
			if (err.empty())
				err =
				    "cannot mount read-only prebuilt dependency";
			prepared = false;
			break;
		}
#endif
	if (prepared) {
		draft_phase_t phase(run_id_, "apply_revision");
		prepared = phase.ok = apply_changes(draft, *this, changes, err);
	}
	if (!prepared) {
		std::string cleanup_err;
		if (remove_tree(next, cleanup_err))
			return false;
		ai_log_error("agent.draft", "cleanup-failed-materialization",
		    cleanup_err);

		return false;
	}
	struct stat st;
	const bool had_target = lstat(target.c_str(), &st) == 0;
	if (had_target && !rename_directory(target, old)) {
		err = "cannot preserve previous private draft workspace";
		return ai_error("agent.draft", "rotate-current", err);
	}
	if (!rename_directory(next, target)) {
		if (had_target && !rename_directory(old, target)) {
			ai_log_error("agent.draft", "restore-previous-worktree",
			    "cannot restore previous private draft workspace");
		}
		err = "cannot install private draft workspace";
		return ai_error("agent.draft", "install", err);
	}
	save_draft_snapshot(run_id_, target, baseline, overlay_hash,
	    skipped_files, manifest, records);
	if (had_target) {
		std::string cleanup_err;
		draft_phase_t phase(run_id_, "remove_previous");
		phase.ok = remove_tree(old, cleanup_err, &phase.budget);
		if (!phase.ok) {
			ai_log_error("agent.draft", "cleanup-previous-worktree",
			    cleanup_err);
		}
	}
	return true;
}

bool agent_draft_store_t::retire(
    const std::string &cleanup_id, bool &detached, std::string &err) const
{
	detached = false;
	if (!valid_run_id(run_id_) || !valid_run_id(cleanup_id) ||
	    run_id_ == cleanup_id) {
		err = "invalid retired draft identity";
		return false;
	}
	const std::string source = root_path();
	const std::string target =
	    agent_draft_store_t(user_root_, project_path_, cleanup_id)
	        .root_path();
	if (source.empty() || target.empty()) {
		err = "cannot resolve retired draft";
		return false;
	}
	struct stat st;
	if (lstat(source.c_str(), &st) != 0) {
		if (errno == ENOENT)
			return true;
		err = "cannot inspect retiring draft";
		return false;
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
		err = "retiring draft is not a directory";
		return false;
	}
	if (lstat(target.c_str(), &st) == 0 || errno != ENOENT) {
		err = "retired draft identity already exists";
		return false;
	}
	if (!rename_directory(source, target)) {
		err = "cannot detach reviewed draft";
		return false;
	}
	detached = true;
	// A long-lived finished draft may have an old root mtime. Keep concurrent
	// retention cleanup from selecting the newly detached directory.
#ifdef _WIN32
	const int touch_result = _utime(target.c_str(), NULL);
#else
	const int touch_result = utime(target.c_str(), NULL);
#endif
	if (touch_result != 0)
		ai_log_error("agent.draft", "touch-retired-draft",
		    "cannot refresh retired draft retention time");
	std::string ignored;
	if (remove_tree(source + ".snapshot", ignored))
		return true;
	ai_log_error("agent.draft", "remove-retired-snapshot", ignored);
	return true;
}

bool agent_draft_store_t::remove(std::string &err) const
{
	if (!valid_run_id(run_id_)) {
		err = "invalid private draft identity";
		return ai_error("agent.draft", "validate-remove", err);
	}
	draft_phase_t phase(run_id_, "remove_reviewed");
	phase.ok = remove_tree(root_path(), err, &phase.budget) &&
	    remove_tree(root_path() + ".snapshot", err, &phase.budget);
	return phase.ok;
}

bool agent_draft_store_t::cleanup_stale(
    long long max_age_seconds, std::string &err) const
{
	if (max_age_seconds <= 0) {
		err = "invalid private draft retention";
		return ai_error("agent.draft", "validate-retention", err);
	}
	std::string project_root;
	if (!agent_workspace_t::resolve_project_state_root(
	        user_root_, project_path_, project_root, err))
		return false;
	const std::string worktrees = join_path(
	    join_path(join_path(project_root, ".webcool_agent"), "worktrees"),
	    "");
	DIR *directory = opendir(worktrees.c_str());
	if (directory == NULL) {
		if (errno == ENOENT)
			return true;
		err = "cannot enumerate private draft worktrees";
		return ai_error("agent.draft", "open-worktrees", err);
	}
	const time_t cutoff = time(NULL) - static_cast<time_t>(max_age_seconds);
	bool ok = true;
	for (dirent *entry = readdir(directory); entry != NULL;
	     entry = readdir(directory)) {
		const std::string name = entry->d_name;
		if (name == "." || name == ".." || name == run_id_ ||
		    name.find('/') != std::string::npos)
			continue;
		const std::string path = join_path(worktrees, name);
		struct stat st;
		if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode) ||
		    !S_ISDIR(st.st_mode) || st.st_mtime >= cutoff)
			continue;
		std::string child_err;
		if (!(!remove_tree(path, child_err) ||
		        !remove_tree(path + ".snapshot", child_err)))
			continue;
		err = child_err;
		ok = false;
		break;
	}
	closedir(directory);
	return ok;
}
} // namespace ai
} // namespace webcool
