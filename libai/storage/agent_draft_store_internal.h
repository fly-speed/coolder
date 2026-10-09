#pragma once
#include "stdafx.h"
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
class draft_io_budget_t {
	// Start of the current cooperative scheduling time slice.
	std::chrono::steady_clock::time_point slice_ =
	    std::chrono::steady_clock::now();
	// Timestamp of the most recent progress log emission.
	std::chrono::steady_clock::time_point reported_ = slice_;

public:
	// entries: Number of filesystem work checkpoints visited.
	// yields: Number of times the operation yielded execution.
	size_t entries = 0, yields = 0;
	// work_ms: Maximum uninterrupted work slice in milliseconds before
	// yielding.
	// rest_ms: Cooperative delay in milliseconds after each work slice.
	int work_ms = 5, rest_ms = 10;
	// Run identifier associated with this operation's accounting.
	std::string run;
	// Current processing phase reported to progress observers.
	const char *phase = NULL;
	// Account for bounded filesystem work and yield when the time slice
	// expires.
	void checkpoint()
	{
		++entries;
		if (phase &&
		    std::chrono::steady_clock::now() - reported_ >=
		        std::chrono::seconds(2)) {
			logger(
			    "AI component=agent.draft event=phase_progress run_id=%s phase=%s entries=%zu yields=%zu",
			    run.c_str(), phase, entries, yields);
			reported_ = std::chrono::steady_clock::now();
		}
		if (std::chrono::steady_clock::now() - slice_ <
		    std::chrono::milliseconds(work_ms))
			return;
		++yields;
		if (acl_fiber_running())
			acl::fiber::delay(rest_ms);
		else
			std::this_thread::sleep_for(
			    std::chrono::milliseconds(rest_ms));
		slice_ = std::chrono::steady_clock::now();
	}
};
// Records phase timing and owns its cooperative I/O budget.
class draft_phase_t {
	// Read the current thread's CPU time in milliseconds when supported.
	static long long thread_cpu_ms()
	{
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
		struct timespec value;
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
			return static_cast<long long>(value.tv_sec) * 1000 +
			    value.tv_nsec / 1000000;
#endif
		return -1;
	}
	// Run identifier associated with this operation's accounting.
	std::string run_;
	// Name of the phase currently being measured.
	const char *phase_;
	// Start timestamp used for elapsed-time accounting.
	std::chrono::steady_clock::time_point start_ =
	    std::chrono::steady_clock::now();
	// Thread CPU time captured at the start of the operation.
	long long cpu_start_ = thread_cpu_ms();

public:
	// Whether the operation completed successfully.
	bool ok = false;
	// Budget tracker used to bound work and yield cooperatively.
	draft_io_budget_t budget;
	// Initialize draft phase state from the supplied arguments.
	draft_phase_t(const std::string &run, const char *phase)
	        : run_(run)
	        , phase_(phase)
	{
		budget.run = run;
		budget.phase = phase;
		if (std::string(phase) == "remove_reviewed") {
			budget.work_ms = 2;
			budget.rest_ms = 15;
		}
		logger(
		    "AI component=agent.draft event=phase_started run_id=%s phase=%s",
		    run_.c_str(), phase_);
	}
	// Record the final phase outcome and its resource accounting.
	~draft_phase_t()
	{
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        std::chrono::steady_clock::now() - start_)
		        .count();
		const long long cpu_end = thread_cpu_ms();
		logger(
		    "AI component=agent.draft event=phase_finished run_id=%s phase=%s ok=%d elapsed_ms=%lld thread_cpu_ms=%lld entries=%zu yields=%zu",
		    run_.c_str(), phase_, ok, static_cast<long long>(elapsed),
		    cpu_start_ < 0 || cpu_end < 0 ? -1 : cpu_end - cpu_start_,
		    budget.entries, budget.yields);
	}
};

using ::webcool::ai::file_ops::join_path;

// Validate the expected run identifier syntax.
bool valid_run_id(const std::string &value);

// Create a private directory with the required storage protections.
bool create_private_directory(const std::string &path, std::string &err);

// Remove a private tree while honoring its cooperative I/O budget.
bool remove_tree(const std::string &path, std::string &err,
    draft_io_budget_t *budget = NULL);

// Rename a private directory using the platform-compatible operation.
bool rename_directory(const std::string &from, const std::string &to);

// Copy permitted source files into the private draft workspace.
bool copy_build_tree(agent_workspace_t &source, const std::string &source_dir,
    agent_workspace_t &draft, const std::string &draft_dir,
    size_t &skipped_files, long long &dependency_bytes, std::string &err,
    draft_io_budget_t &budget, const std::vector<std::string> &excluded = {});

// Apply validated proposals to the private draft tree.
bool apply_changes(agent_workspace_t &draft, const agent_draft_store_t &store,
    const std::vector<agent_change_proposal_t> &changes, std::string &err);

// Manifest contains paths and hashes only, never source text. Older snapshots
// and structural operations deliberately fall back to the full transaction.
std::string write_manifest(const agent_draft_store_t &store,
    const std::vector<agent_change_proposal_t> &changes);

// Read path-to-digest entries from the saved draft manifest.
bool parse_write_manifest(
    const std::string &text, std::map<std::string, std::string> &entries);

// Eligible writes have existing safe parents and a bounded rollback image.
// Invalidate the durable snapshot BEFORE the first write: after interruption,
// normal materialization reconstructs the checkpoint rather than trusting a
// partially installed revision. Old compiler outputs are never retained.
bool update_verified_writes(const agent_draft_store_t &store,
    agent_workspace_t &records, const std::string &run,
    const std::string &target, const std::string &baseline,
    const std::string &overlay, const std::string &previous_manifest,
    const std::string &manifest,
    const std::vector<agent_change_proposal_t> &changes, size_t skipped,
    bool &attempted, std::string &err);

// Test the component-boundary containment relation for dependency paths.
bool dep_inside(const std::string &parent, const std::string &path);
// Collect bounded artifact files and directories under the declared
// dependency.
bool dependency_files(agent_workspace_t &workspace, const std::string &path,
    std::vector<std::string> &files, size_t depth, draft_io_budget_t &budget,
    std::string &err, std::vector<std::string> &directories);
// Create missing parent directories inside the private dependency tree.
bool dependency_parents(
    const std::string &root, const std::string &path, std::string &err);
#ifndef _WIN32
// Cross-process ownership also coalesces requests from multiple fibers. Never
// block the event loop in flock: wait cooperatively, with a deadline/cancel.
struct dependency_build_lock_t {
	// Owned file descriptor, with a negative value indicating no open
	// file.
	int fd = -1;
	// Whether execution had to wait for an admission slot.
	bool waited = false;
	// Release the dependency build lock and close its descriptor.
	~dependency_build_lock_t()
	{
		if (fd >= 0)
			close(fd);
	}
	// Acquire the dependency-cache build lock while allowing
	// cancellation.
	bool acquire(const std::string &path,
	    const std::function<bool()> &cancelled, std::string &err)
	{
		fd = open(path.c_str(),
		    O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC,
		    0600);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			err = "cannot open source dependency build lock";
			return false;
		}
		const auto deadline =
		    std::chrono::steady_clock::now() + std::chrono::minutes(10);
		while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
			waited = true;
			if (errno != EWOULDBLOCK && errno != EAGAIN &&
			    errno != EINTR) {
				err = "cannot lock source dependency build";
				return false;
			}
			if (cancelled && cancelled()) {
				err = "source dependency build cancelled";
				return false;
			}
			if (std::chrono::steady_clock::now() >= deadline) {
				err =
				    "source dependency build already running; wait and retry";
				return false;
			}
			if (acl_fiber_running())
				acl::fiber::delay(50);
			else
				std::this_thread::sleep_for(
				    std::chrono::milliseconds(50));
		}
		return true;
	}
};
// Scope-owned staging directory for preparing dependency artifacts.
struct dependency_temporary_t {
	// Path of the file or resource associated with this record.
	std::string path;
	// Remove the owned staging directory when preparation leaves scope.
	~dependency_temporary_t()
	{
		if (!path.empty()) {
			std::string ignored;
			remove_tree(path, ignored);
		}
	}
	// Create a unique staging directory owned by this cleanup guard.
	bool create(const std::string &parent, std::string &err)
	{
		std::string pattern = parent + "/.build-XXXXXX";
		std::vector<char> name(pattern.begin(), pattern.end());
		name.push_back(0);
		char *value = mkdtemp(name.data());
		if (!value) {
			err =
			    "cannot create private dependency build directory";
			return false;
		}
		path = value;
		return true;
	}
};
// Copy declared build artifacts into the private dependency cache.
bool copy_dependency_artifacts(const std::string &source_root,
    const std::string &base, const std::vector<std::string> &artifacts,
    const std::string &target, draft_io_budget_t &budget,
    const std::function<bool()> &cancelled, std::string &err);
// Installed shared libraries often export file aliases (libfoo.so ->
// libfoo.so.1). Flatten only aliases that resolve to regular files inside the
// install prefix. The cache never inherits an escaping or directory symlink.
struct installed_artifact_copy_t {
	// source_root: Root of the source tree used for copying.
	// target_root: Root of the destination tree used for copying.
	std::string source_root, target_root;
	// Budget tracker used to bound work and yield cooperatively.
	draft_io_budget_t &budget;
	// Borrowed callback checked to stop artifact copying on cancellation.
	const std::function<bool()> &cancelled;
	// files: Number of files accounted for by this operation.
	// directories: Number of directories accounted for by this operation.
	size_t files = 0, directories = 0;
	// Number of bytes accounted for by this operation.
	long long bytes = 0;
	// Initialize installed artifact copy state from the supplied
	// arguments.
	installed_artifact_copy_t(const std::string &input,
	    const std::string &output, draft_io_budget_t &io,
	    const std::function<bool()> &stop)
	        : source_root(input)
	        , target_root(output)
	        , budget(io)
	        , cancelled(stop)
	{
	}
	// Copy one installed artifact while checking type, containment and
	// size limits.
	bool copy(const std::string &relative, size_t depth, std::string &err);
};

// Copy one installed artifact while checking type, containment and size
// limits.
inline bool installed_artifact_copy_t::copy(
    const std::string &relative, size_t depth, std::string &err)
{
	budget.checkpoint();
	if (cancelled && cancelled()) {
		err = "source dependency build cancelled";
		return false;
	}
	struct stat st;
	const std::string path = source_root + "/" + relative;
	if (lstat(path.c_str(), &st) != 0) {
		err = "missing installed artifact: " + relative;
		return false;
	}
	if (S_ISDIR(st.st_mode)) {
		if (++directories > 16384 || depth > 64) {
			err = "installed artifact directory limit";
			return false;
		}
		if (!dependency_parents(
		        target_root, relative + "/placeholder", err))
			return false;
		DIR *dir = opendir(path.c_str());
		if (!dir) {
			err = "cannot read installed artifact directory";
			return false;
		}
		std::vector<std::string> names;
		bool complete = true;
		while (true) {
			errno = 0;
			dirent *entry = readdir(dir);
			if (!entry) {
				complete = errno == 0;
				break;
			}
			const std::string name = entry->d_name;
			if (name == "." || name == "..")
				continue;
			if (names.size() >= 2000) {
				complete = false;
				break;
			}
			names.push_back(name);
		}
		closedir(dir);
		if (!complete) {
			err =
			    "installed artifact directory incomplete or exceeds limit";
			return false;
		}
		std::sort(names.begin(), names.end());
		for (const auto &name : names) {
			if (copy(relative + "/" + name, depth + 1, err))
				continue;
			return false;
		}
		return true;
	}
	std::string source = relative;
	if (S_ISLNK(st.st_mode)) {
		char canonical[4096];
		if (!realpath(path.c_str(), canonical) ||
		    !dep_inside(source_root, canonical) ||
		    stat(canonical, &st) != 0 || !S_ISREG(st.st_mode)) {
			err =
			    "installed artifact symlink escapes install prefix or is not a regular file: " +
			    relative;
			return false;
		}
		source = std::string(canonical).substr(source_root.size() + 1);
	}
	bytes += st.st_size;
	if (!(!S_ISREG(st.st_mode) || ++files > 20000 ||
	        bytes > 2LL * 1024 * 1024 * 1024))
		return dependency_parents(target_root, relative, err) &&
		    agent_workspace_t(source_root)
		        .copy_build_dependency(source,
		            agent_workspace_t(target_root), relative, err);
	err = "installed artifact file limit or unsupported file type";
	return false;
}

// Fingerprint the configured build executable for dependency cache keys.
std::string dependency_tool_identity(const std::string &path);
// Build or reuse a private cached dependency from its declared source inputs.
bool prepare_source_dependency(const std::string &user_root,
    const std::string &base, const std::string &metadata,
    const prebuilt_dependency_t &dep, const std::string &platform,
    std::string &cache, draft_io_budget_t &budget,
    const std::function<bool()> &cancelled, std::string &err);
#endif

// Resolve declared dependencies into verified read-only cache mounts.
bool prepare_dependencies(const std::string &user_root,
    const std::string &project, const std::string &metadata,
    const std::vector<prebuilt_dependency_t> &deps,
    std::map<std::string, std::string> &mounts, draft_io_budget_t &budget,
    std::string &err, const std::function<bool()> &should_cancel);
// Install the verified read-only dependency links in the private draft.
bool dependency_links(
    const std::string &root, const std::map<std::string, std::string> &mounts);

}
}
}
