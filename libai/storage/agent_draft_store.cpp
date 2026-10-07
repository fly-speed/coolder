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

namespace webcool {
namespace ai {
namespace {

// One budget spans recursive traversal; per-file budgets would never yield
// when a large tree consists of many small files.
class draft_io_budget_t {
    std::chrono::steady_clock::time_point slice_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point reported_ = slice_;
public:
    size_t entries = 0, yields = 0;
    int work_ms = 5, rest_ms = 10;
    std::string run;
    const char* phase = NULL;
    void checkpoint() {
        ++entries;
        if (phase && std::chrono::steady_clock::now() - reported_ >= std::chrono::seconds(2)) {
            logger("AI component=agent.draft event=phase_progress run_id=%s phase=%s entries=%zu yields=%zu", run.c_str(), phase, entries, yields);
            reported_ = std::chrono::steady_clock::now();
        }
        if (std::chrono::steady_clock::now() - slice_ < std::chrono::milliseconds(work_ms)) return;
        ++yields;
        if (acl_fiber_running()) acl::fiber::delay(rest_ms);
        else std::this_thread::sleep_for(std::chrono::milliseconds(rest_ms));
        slice_ = std::chrono::steady_clock::now();
    }
};
class draft_phase_t {
    static long long thread_cpu_ms() {
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
        struct timespec value;
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return static_cast<long long>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
#endif
        return -1;
    }
    std::string run_;
    const char* phase_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    long long cpu_start_ = thread_cpu_ms();
public:
    bool ok = false;
    draft_io_budget_t budget;
    draft_phase_t(const std::string& run, const char* phase) : run_(run), phase_(phase) {
        budget.run = run;
        budget.phase = phase;
        if (std::string(phase) == "remove_reviewed") { budget.work_ms = 2; budget.rest_ms = 15; }
        logger("AI component=agent.draft event=phase_started run_id=%s phase=%s", run_.c_str(), phase_);
    }
    ~draft_phase_t() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start_).count();
        const long long cpu_end = thread_cpu_ms();
        logger("AI component=agent.draft event=phase_finished run_id=%s phase=%s ok=%d elapsed_ms=%lld thread_cpu_ms=%lld entries=%zu yields=%zu",
            run_.c_str(), phase_, ok, static_cast<long long>(elapsed), cpu_start_ < 0 || cpu_end < 0 ? -1 : cpu_end-cpu_start_, budget.entries, budget.yields);
    }
};

using ::webcool::ai::file_ops::join_path;

bool valid_run_id(const std::string& value) {
    return ::webcool::ai::identifiers::valid_id(value);
}

bool create_private_directory(const std::string& path, std::string& err) {
	struct stat st;
	if (lstat(path.c_str(), &st) == 0) {
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) return true;
		err = "draft workspace path is not a safe directory";
		return ai_error("agent.draft", "validate-directory", err);
	}
#ifdef _WIN32
	const int created = _mkdir(path.c_str());
#else
	const int created = mkdir(path.c_str(), 0700);
#endif
	if (created != 0) {
		err = "cannot create private draft workspace directory";
		return ai_error("agent.draft", "create-directory", err);
	}
#ifndef _WIN32
	if (chmod(path.c_str(), 0700) != 0) {
		err = "cannot protect private draft workspace directory";
		return ai_error("agent.draft", "protect-directory", err);
	}
#endif
	return true;
}

bool remove_tree(const std::string& path, std::string& err, draft_io_budget_t* budget = NULL) {
	draft_io_budget_t local;
	if (!budget) budget = &local;
	budget->checkpoint();
	struct stat st;
	if (lstat(path.c_str(), &st) != 0) return true;
	if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
		if (unlink(path.c_str()) == 0) return true;
		err = "cannot remove private draft file";
		return ai_error("agent.draft", "remove-file", err);
	}
	DIR* directory = opendir(path.c_str());
	if (directory == NULL) {
		err = "cannot enumerate private draft workspace";
		return ai_error("agent.draft", "open-directory", err);
	}
	bool ok = true;
	for (dirent* entry = readdir(directory); entry != NULL;
		entry = readdir(directory))
	{
		const std::string name = entry->d_name;
		if (name == "." || name == ".." || name.find('/') != std::string::npos) {
			continue;
		}
		std::string child_err;
		if (!remove_tree(join_path(path, name), child_err, budget)) {
			err = child_err;
			ok = false;
			break;
		}
	}
	closedir(directory);
	if (!ok) return false;
#ifdef _WIN32
	const int removed = _rmdir(path.c_str());
#else
	const int removed = rmdir(path.c_str());
#endif
	if (removed != 0) {
		err = "cannot remove private draft workspace directory";
		return ai_error("agent.draft", "remove-directory", err);
	}
	return true;
}

bool rename_directory(const std::string& from, const std::string& to) {
#ifdef _WIN32
	std::wstring from_wide;
	std::wstring to_wide;
	return webcool_utf8_path_to_wide(from.c_str(), from_wide)
		&& webcool_utf8_path_to_wide(to.c_str(), to_wide)
		&& MoveFileExW(from_wide.c_str(), to_wide.c_str(),
			MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool copy_build_tree(agent_workspace_t& source, const std::string& source_dir,
	agent_workspace_t& draft, const std::string& draft_dir,
	size_t& skipped_files, long long& dependency_bytes, std::string& err, draft_io_budget_t& budget, const std::vector<std::string>& excluded = {})
{
#ifndef _WIN32
    if (draft_dir.empty()) return source.copy_build_tree_to(source_dir, draft, skipped_files,
        dependency_bytes, [&budget] { budget.checkpoint(); }, err, excluded);
#endif

	std::vector<workspace_entry_t> entries;
	if (!source.list(source_dir, entries, err)) {
		return ai_error("agent.draft", "list-source", err);
	}
	for (size_t i = 0; i < entries.size(); ++i) {
		budget.checkpoint();
		const size_t slash = entries[i].path.rfind('/');
		const std::string name = slash == std::string::npos
			? entries[i].path : entries[i].path.substr(slash + 1);
		if (draft_dir.empty() && name == ".webcool-build") continue;
		const std::string target = draft_dir.empty()
			? name : join_path(draft_dir, name);
		if (entries[i].directory) {
			if (!draft.create_directory_if_absent(target, err)
				|| !copy_build_tree(source, entries[i].path, draft, target,
					skipped_files, dependency_bytes, err, budget))
			{
				return ai_error("agent.draft", "copy-directory", err);
			}
			continue;
		}
		const size_t dot = name.rfind('.');
		const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
		const bool library = ext == ".a" || ext == ".lib" || ext == ".so"
			|| ext == ".dylib" || ext == ".dll" || name.find(".so.") != std::string::npos;
		if (library) {
			dependency_bytes += entries[i].size;
			if (entries[i].size > 512LL * 1024 * 1024 || dependency_bytes > 2LL * 1024 * 1024 * 1024) {
				err = "private build dependencies exceed 512 MiB per file or 2 GiB total: " + entries[i].path;
				return false;
			}
			if (!source.copy_build_dependency(entries[i].path, draft, target, err)) return false;
			continue;
		}
		// read() would truncate these files after reading a full MiB. They are
		// excluded from draft text anyway; libraries were handled above.
		if (entries[i].size > 1024 * 1024) { ++skipped_files; continue; }
		std::string content;
		bool truncated = false;
		std::string read_err;
		if (!source.read(entries[i].path, content, truncated, read_err, false)
			|| truncated)
		{
			// Other binary and oversized assets remain in the formal project.
			// Library bytes above are copied privately, never returned to the model.
			++skipped_files;
			continue;
		}
		if (!source.copy_build_dependency(entries[i].path, draft, target, err)) {
			return ai_error("agent.draft", "copy-file", err);
		}
	}
	return true;
}

bool apply_changes(agent_workspace_t& draft,
	const agent_draft_store_t& store,
	const std::vector<agent_change_proposal_t>& changes, std::string& err)
{
	for (size_t i = 0; i < changes.size(); ++i) {
		std::string path;
		if (!store.to_draft_path(changes[i].path, path) || path.empty()) {
			err = "draft change path is outside the selected project";
			return ai_error("agent.draft", "map-change-path", err);
		}
		if (changes[i].operation == "mkdir") {
			if (!draft.create_directory_if_absent(path, err)) return false;
		} else if (changes[i].operation == "replace_empty_file_with_directory") {
			std::string current;
			bool truncated = false;
			if (!draft.read(path, current, truncated, err) || truncated
				|| !current.empty()) return false;
			if (!draft.delete_text_if_unchanged(path,
				agent_workspace_t::content_sha256(current), err)
				|| !draft.create_directory_if_absent(path, err)) return false;
		}
	}
	for (size_t i = 0; i < changes.size(); ++i) {
		std::string path;
		if (!store.to_draft_path(changes[i].path, path) || path.empty()) {
			err = "draft change path is outside the selected project";
			return ai_error("agent.draft", "map-change-path", err);
		}
		if (changes[i].operation == "write") {
			if (!draft.save_generated_text(path, changes[i].content, err)) return false;
		} else if (changes[i].operation == "delete"
			|| changes[i].operation == "move")
		{
			std::string content;
			bool truncated = false;
			if (!draft.read(path, content, truncated, err) || truncated) return false;
			if (changes[i].operation == "move") {
				std::string target;
				if (!store.to_draft_path(changes[i].target_path, target)
					|| target.empty()
					|| !draft.create_text_if_absent(target, content, err)) return false;
			}
			if (!draft.delete_text_if_unchanged(path,
				agent_workspace_t::content_sha256(content), err)) return false;
		}
	}
	return true;
}

// Manifest contains paths and hashes only, never source text. Older snapshots
// and structural operations deliberately fall back to the full transaction.
std::string write_manifest(const agent_draft_store_t& store,
    const std::vector<agent_change_proposal_t>& changes) {
    std::map<std::string, std::string> entries;
    for (const auto& change : changes) {
        std::string path, normalized, err;
        if (change.operation != "write" || !store.to_draft_path(change.path, path)
            || !agent_workspace_t::normalize_path(path, normalized, false, err)
            || path != normalized || !entries.emplace(path, agent_workspace_t::content_sha256(change.content)).second)
            return "";
    }
    acl::json json;
    auto& root = json.create_node();
    root.add_text("format", "draft-writes-v1");
    for (const auto& item : entries) root.add_text(("path:" + item.first).c_str(), item.second.c_str());
    return root.to_string();
}

bool parse_write_manifest(const std::string& text, std::map<std::string, std::string>& entries) {
    acl::json json(text.c_str());
    if (!json.finish()) return false;
    auto* format = json["format"];
    if (!format || !format->get_text() || std::string(format->get_text()) != "draft-writes-v1") return false;
    auto& root = json.get_root();
    for (auto* node = root.first_child(); node; node = root.next_child()) {
        const char* tag = node->tag_name();
        if (!tag || std::string(tag) == "format") continue;
        if (std::string(tag).compare(0, 5, "path:") != 0 || !node->get_text()) return false;
        entries[std::string(tag).substr(5)] = node->get_text();
    }
    return true;
}

// Eligible writes have existing safe parents and a bounded rollback image.
// Invalidate the durable snapshot BEFORE the first write: after interruption,
// normal materialization reconstructs the checkpoint rather than trusting a
// partially installed revision. Old compiler outputs are never retained.
bool update_verified_writes(const agent_draft_store_t& store, agent_workspace_t& records,
    const std::string& run, const std::string& target, const std::string& baseline,
    const std::string& overlay, const std::string& previous_manifest, const std::string& manifest,
    const std::vector<agent_change_proposal_t>& changes, size_t skipped, bool& attempted, std::string& err) {
    attempted = false;
    std::map<std::string, std::string> previous, current;
    if (!parse_write_manifest(previous_manifest, previous) || !parse_write_manifest(manifest, current)
        || changes.size() > 512) return false;
    for (const auto& item : previous) if (!current.count(item.first)) return false;
    struct edit_t { std::string path, before, after; bool existed; };
    std::vector<edit_t> edits;
    agent_workspace_t draft(target);
    size_t backup_bytes = 0;
    for (const auto& change : changes) {
        std::string path;
        store.to_draft_path(change.path, path);
        if (previous.count(path) && previous[path] == current[path]) {
            std::string existing, check_error;
            bool truncated = false;
            if (!draft.read(path, existing, truncated, check_error) || truncated
                || agent_workspace_t::content_sha256(existing) != current[path]) return false;
            continue;
        }
        if (change.content.size() > 1024 * 1024 || change.content.substr(0, 8192).find('\0') != std::string::npos) return false;
        const size_t slash = path.rfind('/');
        std::string parent, check_error;
        if (!agent_workspace_t::resolve_project_root(target, slash == std::string::npos ? "" : path.substr(0, slash), parent, check_error)) return false;
        struct stat st;
        const std::string absolute = join_path(parent, slash == std::string::npos ? path : path.substr(slash+1));
        const bool exists = lstat(absolute.c_str(), &st) == 0;
        if ((!exists && errno != ENOENT) || (exists && !S_ISREG(st.st_mode))) return false;
        edit_t edit;
        edit.path = path; edit.after = change.content; edit.existed = exists;
        bool truncated = false;
        if (exists && (!draft.read(path, edit.before, truncated, check_error) || truncated)) return false;
        backup_bytes += edit.before.size();
        if (backup_bytes > 16 * 1024 * 1024) return false;
        edits.push_back(edit);
    }
    attempted = true;
    draft_phase_t phase(run, "incremental_writes");
    if (!records.save_generated_text(run + ".snapshot", "incremental-update-in-progress\n", err)) return false;
    if (!remove_tree(join_path(target, ".webcool-build"), err, &phase.budget)) return false;
    size_t applied = 0;
    bool ok = true;
    for (const auto& edit : edits) {
        phase.budget.checkpoint();
        if (!draft.save_generated_text(edit.path, edit.after, err)) { ok = false; break; }
        ++applied;
    }
    std::string hash;
    if (ok) ok = draft.tree_sha256("", hash, err);
    if (ok) {
        std::ostringstream record;
        record << baseline << "\n" << overlay << "\n" << hash << "\n" << skipped << "\n" << manifest << "\n";
        ok = records.save_generated_text(run + ".snapshot", record.str(), err);
    }
    if (!ok) {
        bool restored = true;
        while (applied > 0) {
            const auto& edit = edits[--applied];
            std::string restore_error;
            if (!(edit.existed ? draft.save_generated_text(edit.path, edit.before, restore_error)
                : draft.delete_text_if_unchanged(edit.path, agent_workspace_t::content_sha256(edit.after), restore_error))) restored = false;
        }
        // Keep the snapshot invalid even after rollback. A later retry must
        // verify/reconstruct it; failed rollback is never presented as success.
        logger("AI component=agent.draft event=incremental_rollback run_id=%s restored=%d", run.c_str(), restored);
        return false;
    }
    phase.ok = true;
    logger("AI component=agent.draft event=materialize_decision run_id=%s mode=incremental reason=verified_writes changed_files=%zu", run.c_str(), edits.size());
    return true;
}

} // namespace

agent_draft_store_t::agent_draft_store_t(const std::string& user_root,
	const std::string& project_path, const std::string& run_id)
	: user_root_(user_root), project_path_(project_path), run_id_(run_id) {}

std::string agent_draft_store_t::root_path() const {
	std::string project_root;
	std::string ignored;
	if (!agent_workspace_t::resolve_project_state_root(user_root_, project_path_,
		project_root, ignored)) return "";
	return join_path(join_path(join_path(project_root,
		".webcool_agent"), "worktrees"), run_id_);
}

bool agent_draft_store_t::to_draft_path(const std::string& path,
	std::string& draft_path) const
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
	if (path.compare(0, prefix.size(), prefix) != 0) return false;
	draft_path = path.substr(prefix.size());
	return true;
}

std::string agent_draft_store_t::to_project_path(
	const std::string& draft_path) const
{
	if (project_path_.empty()) return draft_path;
	return draft_path.empty() ? project_path_
		: join_path(project_path_, draft_path);
}

namespace {
bool dep_inside(const std::string& parent, const std::string& path) {
    return path == parent || path.compare(0, parent.size()+1, parent+"/") == 0;
}
bool dependency_files(agent_workspace_t& workspace, const std::string& path,
    std::vector<std::string>& files, size_t depth, draft_io_budget_t& budget, std::string& err,
    std::vector<std::string>& directories) {
    if (depth > 64 || files.size() >= 20000) { err = "prebuilt dependency exceeds file/depth limit"; return false; }
    budget.checkpoint();
    std::vector<workspace_entry_t> entries;
    if (!workspace.list(path, entries, err)) return false;
    if (entries.size() >= 2000) { err = "prebuilt dependency directory listing exceeds limit"; return false; }
    for (const auto& entry : entries) {
        if (entry.directory) {
            if (directories.size() >= 16384) { err = "prebuilt dependency exceeds directory limit"; return false; }
            directories.push_back(entry.path);
            if (!dependency_files(workspace, entry.path, files, depth+1, budget, err, directories)) return false;
        } else {
            if (files.size() >= 20000) { err = "prebuilt dependency exceeds file limit"; return false; }
            files.push_back(entry.path);
        }
    }
    return true;
}
bool dependency_parents(const std::string& root, const std::string& path, std::string& err) {
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash+1))
        if (!create_private_directory(root+"/"+path.substr(0, slash), err)) return false;
    return true;
}
#ifndef _WIN32
// Cross-process ownership also coalesces requests from multiple fibers. Never
// block the event loop in flock: wait cooperatively, with a deadline/cancel.
struct dependency_build_lock_t {
    int fd = -1;
    bool waited = false;
    ~dependency_build_lock_t() { if (fd >= 0) close(fd); }
    bool acquire(const std::string& path, const std::function<bool()>& cancelled, std::string& err) {
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { err = "cannot open source dependency build lock"; return false; }
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::minutes(10);
        while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            waited = true;
            if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { err = "cannot lock source dependency build"; return false; }
            if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
            if (std::chrono::steady_clock::now() >= deadline) { err = "source dependency build already running; wait and retry"; return false; }
            if (acl_fiber_running()) acl::fiber::delay(50); else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
};
struct dependency_temporary_t {
    std::string path;
    ~dependency_temporary_t() { if (!path.empty()) { std::string ignored; remove_tree(path, ignored); } }
    bool create(const std::string& parent, std::string& err) {
        std::string pattern = parent+"/.build-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
        char* value = mkdtemp(name.data());
        if (!value) { err = "cannot create private dependency build directory"; return false; }
        path = value; return true;
    }
};
bool copy_dependency_artifacts(const std::string& source_root, const std::string& base,
    const std::vector<std::string>& artifacts, const std::string& target,
    draft_io_budget_t& budget, const std::function<bool()>& cancelled, std::string& err) {
    agent_workspace_t source(source_root), destination(target);
    for (const auto& artifact : artifacts) {
        if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
        const std::string path = base.empty() ? artifact : base+"/"+artifact;
        std::string absolute;
        if (!agent_workspace_t::resolve_project_root(source_root, path, absolute, err)) {
            err = "dependency artifact unavailable: " + path + ": " + err; return false;
        }
        struct stat st;
        if (lstat(absolute.c_str(), &st) != 0) return false;
        std::vector<std::string> files, directories;
        if (S_ISDIR(st.st_mode)) {
            directories.push_back(path);
            if (!dependency_files(source, path, files, 0, budget, err, directories)) return false;
        } else if (S_ISREG(st.st_mode)) files.push_back(path);
        else { err = "dependency artifact is not a regular file or directory: " + path; return false; }
        const auto relative = [&](const std::string& value) { return base.empty() ? value : value.substr(base.size()+1); };
        for (const auto& directory : directories) {
            budget.checkpoint();
            if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
            if (!dependency_parents(target, relative(directory)+"/placeholder", err)) return false;
        }
        for (const auto& file : files) {
            budget.checkpoint();
            if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
            if (!dependency_parents(target, relative(file), err)
                || !source.copy_build_dependency(file, destination, relative(file), err)) return false;
        }
    }
    return true;
}
// Installed shared libraries often export file aliases (libfoo.so ->
// libfoo.so.1). Flatten only aliases that resolve to regular files inside the
// install prefix. The cache never inherits an escaping or directory symlink.
struct installed_artifact_copy_t {
    std::string source_root, target_root;
    draft_io_budget_t& budget;
    const std::function<bool()>& cancelled;
    size_t files = 0, directories = 0;
    long long bytes = 0;
    installed_artifact_copy_t(const std::string& input, const std::string& output,
        draft_io_budget_t& io, const std::function<bool()>& stop)
        : source_root(input), target_root(output), budget(io), cancelled(stop) {}
    bool copy(const std::string& relative, size_t depth, std::string& err) {
        budget.checkpoint();
        if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
        struct stat st;
        const std::string path = source_root+"/"+relative;
        if (lstat(path.c_str(), &st) != 0) { err = "missing installed artifact: " + relative; return false; }
        if (S_ISDIR(st.st_mode)) {
            if (++directories > 16384 || depth > 64) { err = "installed artifact directory limit"; return false; }
            if (!dependency_parents(target_root, relative+"/placeholder", err)) return false;
            DIR* dir = opendir(path.c_str());
            if (!dir) { err = "cannot read installed artifact directory"; return false; }
            std::vector<std::string> names;
            bool complete = true;
            while (true) {
                errno = 0; dirent* entry = readdir(dir);
                if (!entry) { complete = errno == 0; break; }
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (names.size() >= 2000) { complete = false; break; }
                names.push_back(name);
            }
            closedir(dir);
            if (!complete) { err = "installed artifact directory incomplete or exceeds limit"; return false; }
            std::sort(names.begin(), names.end());
            for (const auto& name : names) if (!copy(relative+"/"+name, depth+1, err)) return false;
            return true;
        }
        std::string source = relative;
        if (S_ISLNK(st.st_mode)) {
            char canonical[4096];
            if (!realpath(path.c_str(), canonical) || !dep_inside(source_root, canonical)
                || stat(canonical, &st) != 0 || !S_ISREG(st.st_mode)) {
                err = "installed artifact symlink escapes install prefix or is not a regular file: " + relative; return false;
            }
            source = std::string(canonical).substr(source_root.size()+1);
        }
        bytes += st.st_size;
        if (!S_ISREG(st.st_mode) || ++files > 20000 || bytes > 2LL*1024*1024*1024) {
            err = "installed artifact file limit or unsupported file type"; return false;
        }
        return dependency_parents(target_root, relative, err)
            && agent_workspace_t(source_root).copy_build_dependency(source, agent_workspace_t(target_root), relative, err);
    }
};
std::string dependency_tool_identity(const std::string& path) {
    char canonical[4096]; struct stat st;
    if (!realpath(path.c_str(), canonical) || stat(canonical, &st) != 0) return path+":missing";
#ifdef __APPLE__
    const auto mt = st.st_mtimespec, ct = st.st_ctimespec;
#else
    const auto mt = st.st_mtim, ct = st.st_ctim;
#endif
    return std::string(canonical)+":"+std::to_string(st.st_ino)+":"+std::to_string(st.st_size)
        +":"+std::to_string(mt.tv_sec)+":"+std::to_string(mt.tv_nsec)
        +":"+std::to_string(ct.tv_sec)+":"+std::to_string(ct.tv_nsec);
}
bool prepare_source_dependency(const std::string& user_root, const std::string& base,
    const std::string& metadata, const prebuilt_dependency_t& dep, const std::string& platform,
    std::string& cache, draft_io_budget_t& budget, const std::function<bool()>& cancelled, std::string& err) {
    if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
    agent_workspace_t source(user_root);
    std::vector<workspace_entry_t> entries;
    std::vector<std::string> source_paths;
    if (!source.list(base, entries, err)) return false;
    if (entries.size() >= 2000) { err = "source dependency has too many root entries"; return false; }
    bool has_cmake = false, has_make = false;
    for (const auto& entry : entries) {
        const std::string relative = entry.path.substr(base.size()+1);
        if (relative != ".webcool-build") source_paths.push_back(relative);
        if (relative == "CMakeLists.txt") has_cmake = true;
        if (relative == "Makefile" || relative == "makefile" || relative == "GNUmakefile") has_make = true;
    }
    std::string system = dep.build_system;
    if (system == "auto") {
        if (has_cmake == has_make) {
            err = has_cmake ? "dependency has both CMakeLists.txt and Makefile; specify build.system=cmake or make"
                            : "dependency has no CMakeLists.txt or Makefile; configure its supported build files first";
            return false;
        }
        system = has_cmake ? "cmake" : "make";
    }
    if ((system == "make" && dep.has_cmake_options)
        || (system == "cmake" && dep.has_make_options)) {
        err = "dependency options do not match detected build system; specify build.system and matching options"; return false;
    }
    project_toolchain_t toolchain;
    if (!project_toolchain_catalog_t(user_root).discover(base, toolchain, err)) return false;
    sandbox_command_t configure, build;
    for (const auto& command : toolchain.commands) {
        if (system == "cmake" && command.id.find(".cmake.configure") != std::string::npos) configure = command;
        if (system == "cmake" && command.id.find(".cmake.build") != std::string::npos) build = command;
        if (system == "make" && (command.id == "c.make" || command.id == "cpp.make")) build = command;
    }
    if (build.executable.empty() || (system == "cmake" && configure.executable.empty())) {
        err = "source dependency " + dep.root + " requires matching build files and an enabled " + system + " toolchain"; return false;
    }
#ifdef __APPLE__
    if (system == "make") {
        const std::string xcode = "/Applications/Xcode.app/Contents/Developer";
        const std::string clt = "/Library/Developer/CommandLineTools";
        const bool full = access((xcode+"/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++").c_str(), X_OK) == 0;
        const std::string developer = full ? xcode : clt;
        const std::string bin = developer + (full ? "/Toolchains/XcodeDefault.xctoolchain/usr/bin/" : "/usr/bin/");
        if (build.executable == "/usr/bin/make" && access((developer+"/usr/bin/make").c_str(), X_OK) == 0) build.executable = developer+"/usr/bin/make";
        for (const auto& tool : {std::make_pair("CC=", "clang"), std::make_pair("CXX=", "clang++"), std::make_pair("AR=", "ar"), std::make_pair("RANLIB=", "ranlib")})
            if (access((bin+tool.second).c_str(), X_OK) == 0) {
                std::string value = std::string(tool.first)+bin+tool.second;
                if (std::string(tool.first) == "CC=" || std::string(tool.first) == "CXX=")
                    value += " -isysroot " + developer + (full ? "/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk" : "/SDKs/MacOSX.sdk");
                build.fixed_arguments.push_back(value);
            }
    }
#endif
    std::string source_digest;
    if (source_paths.empty() || !source.tree_sha256(base, source_digest, err, source_paths)) return false;
    std::string identity = "source-"+system+"-v1\n"+platform+"\n"+source_digest+"\n"+dep.build_type+"\n";
    if (system == "make") {
        identity += dep.make_target+"\n"+dep.install_target+"\n"+dep.prefix_variable+"\n";
        for (const auto& arg : build.fixed_arguments) {
            identity += arg+"\n";
            const size_t equal = arg.find('=');
            if (equal != std::string::npos) {
                const std::string value = arg.substr(equal+1);
                const size_t sdk = value.find(" -isysroot ");
                identity += dependency_tool_identity(value.substr(0, sdk))+"\n";
                if (sdk != std::string::npos) identity += dependency_tool_identity(value.substr(sdk+11))+"\n";
            }
        }
    }
    auto artifacts = dep.artifacts; std::sort(artifacts.begin(), artifacts.end());
    for (const auto& item : artifacts) identity += "artifact:"+item+"\n";
    for (const auto& item : dep.definitions) identity += "option:"+item.first+"="+item.second+"\n";
    identity += dependency_tool_identity(configure.executable)+"\n"+dependency_tool_identity(build.executable)+"\n";
    for (const auto& argument : configure.fixed_arguments) {
        identity += argument+"\n";
        const size_t equal = argument.find('=');
        if (equal != std::string::npos && equal+1 < argument.size() && argument[equal+1] == '/')
            identity += dependency_tool_identity(argument.substr(equal+1))+"\n";
    }
    for (const auto* path : {"/usr/bin/cc", "/usr/bin/c++", "/usr/bin/make", "/usr/bin/ld"}) identity += dependency_tool_identity(path)+"\n";
    identity += configure.allow_outbound_network ? "network:on" : "network:off";
    const std::string key = agent_workspace_t::content_sha256(identity);
    const std::string packages = metadata+"/dependencies";
    cache = packages+"/"+key;
    dependency_build_lock_t lock;
    if (!lock.acquire(packages+"/"+key+".lock", cancelled, err)) return false;
    if (lock.waited) {
        std::string current;
        if (!source.tree_sha256(base, current, err, source_paths) || current != source_digest) {
            err = "source dependency changed while waiting for its build; retry with current source"; return false;
        }
    }
    agent_workspace_t records(packages);
    struct stat st;
    if (lstat(cache.c_str(), &st) == 0) {
        std::string expected, actual; bool truncated = false;
        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)
            || !records.read(key+".built", expected, truncated, err) || truncated
            || !agent_workspace_t(cache).tree_sha256("", actual, err, dep.artifacts) || expected != actual) {
            err = "source dependency cache is incomplete or changed: " + dep.root; return false;
        }
        logger("AI component=agent.draft event=source_dependency run_id=%s root=%s cache_hit=1 build_executed=0", budget.run.c_str(), dep.root.c_str());
        return true;
    }
    if (lstat((packages+"/"+key+".failed").c_str(), &st) == 0 && time(NULL)-st.st_mtime < 300) {
        std::string failure; bool truncated = false;
        if (!records.read(key+".failed", failure, truncated, err)) return false;
        err = "source dependency recently failed; identical retry suppressed for 5 minutes (change source/options or retry later): " + failure;
        return false;
    }
    dependency_temporary_t work, package;
    if (!work.create(packages, err) || !package.create(packages, err)) return false;
    { draft_phase_t phase(budget.run, "dependency_copy_source");
      if (!(phase.ok = copy_dependency_artifacts(user_root, base, source_paths, work.path, phase.budget, cancelled, err))) return false; }
    std::string copied_digest;
    if (!agent_workspace_t(work.path).tree_sha256("", copied_digest, err, source_paths) || copied_digest != source_digest) {
        err = "source dependency changed during preparation: " + dep.root; return false;
    }
    std::vector<sandbox_command_t> commands;
    std::vector<const char*> phases;
    if (system == "cmake") {
        configure.fixed_arguments.push_back("-DCMAKE_BUILD_TYPE="+dep.build_type);
        configure.fixed_arguments.push_back("-DCMAKE_INSTALL_PREFIX="+work.path+"/.webcool-build/install");
        for (const auto& option : dep.definitions) configure.fixed_arguments.push_back("-D"+option.first+"="+option.second);
        build.fixed_arguments.insert(build.fixed_arguments.end(), {"--config", dep.build_type});
        sandbox_command_t install = build;
        install.id = "dependency.cmake.install";
        install.fixed_arguments = {"--install", ".webcool-build", "--config", dep.build_type};
        commands = {configure, build, install};
        phases = {"dependency_configure", "dependency_build", "dependency_install"};
    } else {
        build.fixed_arguments.push_back(dep.prefix_variable+"="+work.path+"/.webcool-build/install");
        sandbox_command_t install = build;
        build.fixed_arguments.push_back(dep.make_target);
        install.id = "dependency.make.install";
        install.fixed_arguments.push_back(dep.install_target);
        commands = {build, install};
        phases = {"dependency_build", "dependency_install"};
    }
    sandbox_limits_t limits;
    limits.timeout_ms = 300000; limits.cpu_seconds = 240;
    limits.memory_bytes = 2ULL*1024*1024*1024; limits.open_files = 512;
    limits.file_size_bytes = 512ULL*1024*1024; limits.output_bytes = 128*1024;
    for (size_t i = 0; i < commands.size(); ++i) {
        if (cancelled && cancelled()) { err = "source dependency build cancelled"; return false; }
        sandbox_request_t request; request.command_id = commands[i].id;
        sandbox_result_t result;
        bool ok;
        { draft_phase_t phase(budget.run, phases[i]);
          ok = phase.ok = program_sandbox_t(work.path, "", {commands[i]}, limits).execute(request, result, NULL, cancelled)
              && result.exit_code == 0 && !result.cancelled && !result.timed_out; }
        logger("AI component=agent.draft event=dependency_build_stage run_id=%s root=%s phase=%s ok=%d elapsed_ms=%llu exit_code=%d",
            budget.run.c_str(), dep.root.c_str(), phases[i], ok, result.elapsed_ms, result.exit_code);
        if (!ok) {
            std::string diagnostic = result.error+"\n"+result.standard_output+"\n"+result.standard_error;
            if (diagnostic.size() > 8000) diagnostic = diagnostic.substr(diagnostic.size()-8000);
            err = "source dependency " + dep.root + " failed at " + phases[i]+" (exit="+std::to_string(result.exit_code)+"): "+diagnostic;
            if (!result.cancelled) { std::string ignored; records.save_generated_text(key+".failed", err, ignored); }
            return false;
        }
    }
    std::string installed;
    // Resolve through the private workspace before treating install as a root;
    // CMake must not trick the publisher into following an escaping symlink.
    if (!agent_workspace_t::resolve_project_root(work.path, ".webcool-build/install", installed, err)) {
        err = "source dependency " + dep.root + " did not produce a safe install directory: " + err;
        std::string ignored; records.save_generated_text(key+".failed", err, ignored); return false;
    }
    installed_artifact_copy_t exporter{installed, package.path, budget, cancelled};
    for (const auto& artifact : dep.artifacts) if (!exporter.copy(artifact, 0, err)) {
        err = "source dependency " + dep.root + " produced missing/invalid installed artifacts: " + err;
        if (!cancelled || !cancelled()) { std::string ignored; records.save_generated_text(key+".failed", err, ignored); }
        return false;
    }
    std::string artifact_digest;
    if (!agent_workspace_t(package.path).tree_sha256("", artifact_digest, err, dep.artifacts)) return false;
    std::string current_source;
    if (!source.tree_sha256(base, current_source, err, source_paths) || current_source != source_digest) {
        err = "source dependency changed during build; retry with current source"; return false;
    }
    if (!records.save_generated_text(key+".built", artifact_digest, err) || rename(package.path.c_str(), cache.c_str()) != 0) {
        if (err.empty()) err = "cannot publish source dependency cache";
        return false;
    }
    logger("AI component=agent.draft event=source_dependency run_id=%s root=%s cache_hit=0 build_executed=1", budget.run.c_str(), dep.root.c_str());
    return true;
}
#endif

bool prepare_dependencies(const std::string& user_root, const std::string& project,
    const std::string& metadata, const std::vector<prebuilt_dependency_t>& deps,
    std::map<std::string, std::string>& mounts, draft_io_budget_t& budget, std::string& err,
    const std::function<bool()>& should_cancel) {
    mounts.clear();
    if (deps.empty()) return true;
#ifdef _WIN32
    err = "prebuilt dependency reuse requires a read-only dependency sandbox on this platform"; return false;
#else
    if (!create_private_directory(metadata, err) || !create_private_directory(metadata+"/dependencies", err)) return false;
    agent_workspace_t source(user_root);
    struct utsname platform;
    if (uname(&platform) != 0) { err = "cannot identify prebuilt dependency platform"; return false; }
    for (const auto& dep : deps) {
        const std::string base = project.empty() ? dep.root : project+"/"+dep.root;
        if (!dep.build_system.empty()) {
            std::string cache;
            if (!prepare_source_dependency(user_root, base, metadata, dep,
                std::string(platform.sysname)+"/"+platform.machine+"/"+platform.release, cache, budget, should_cancel, err)) return false;
            mounts[dep.root] = cache;
            continue;
        }
        std::vector<std::string> files, directories;
        for (const auto& artifact : dep.artifacts) {
            const std::string path = base+"/"+artifact;
            std::string absolute;
            if (!agent_workspace_t::resolve_project_root(user_root, path, absolute, err)) { err = "prebuilt artifact unavailable: " + path + ": " + err; return false; }
            struct stat st;
            if (lstat(absolute.c_str(), &st) != 0) { err = "missing prebuilt artifact: " + path; return false; }
            if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
                err = "prebuilt artifact must be a regular file or directory: " + path; return false;
            }
        }
        std::string digest;
        if (!source.tree_sha256(base, digest, err, dep.artifacts)) return false;
        std::string declaration;
        for (const auto& artifact : dep.artifacts) declaration += artifact + "\n";
        const std::string key = agent_workspace_t::content_sha256("prebuilt-v1\n" + std::string(platform.sysname)+"\n"+platform.machine+"\n"+declaration+digest);
        const std::string cache = metadata+"/dependencies/"+key;
        struct stat st;
        bool hit = lstat(cache.c_str(), &st) == 0;
        if (hit && (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))) { err = "unsafe prebuilt dependency cache directory"; return false; }
        if (!hit) {
            for (const auto& artifact : dep.artifacts) {
                const std::string path = base+"/"+artifact;
                std::string absolute;
                if (!agent_workspace_t::resolve_project_root(user_root, path, absolute, err)) return false;
                if (lstat(absolute.c_str(), &st) != 0) return false;
                if (S_ISDIR(st.st_mode)) {
                    directories.push_back(path);
                    if (!dependency_files(source, path, files, 0, budget, err, directories)) return false;
                } else files.push_back(path);
            }
            std::sort(files.begin(), files.end());
            std::string pattern = metadata+"/dependencies/.prepare-XXXXXX";
            std::vector<char> temporary(pattern.begin(), pattern.end()); temporary.push_back(0);
            char* created = mkdtemp(temporary.data());
            if (!created) { err = "cannot create dependency cache staging directory"; return false; }
            const std::string next = created;
            agent_workspace_t target(next);
            bool ok = true;
            for (const auto& directory : directories) {
                budget.checkpoint();
                if (!dependency_parents(next, directory.substr(base.size()+1)+"/placeholder", err)) { ok = false; break; }
            }
            for (const auto& file : files) {
                if (!ok) break;
                budget.checkpoint();
                const std::string relative = file.substr(base.size()+1);
                if (!dependency_parents(next, relative, err) || !source.copy_build_dependency(file, target, relative, err)) { ok = false; break; }
            }
            std::string copied_digest;
            if (ok) ok = target.tree_sha256("", copied_digest, err, dep.artifacts) && copied_digest == digest;
            if (ok && rename(next.c_str(), cache.c_str()) != 0)
                ok = lstat(cache.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
            std::string cleanup_error; remove_tree(next, cleanup_error);
            if (!ok) { if (err.empty()) err = "prebuilt dependency changed while preparing cache"; return false; }
        }
        // Verify cached content before granting read access. Content hashes use
        // the bounded inode/ctime cache, so unchanged packages are not reread.
        agent_workspace_t cached(cache);
        std::string actual;
        if (!cached.tree_sha256("", actual, err, dep.artifacts) || actual != digest) {
            err = "prebuilt dependency cache changed; remove the damaged cache before retrying: " + key; return false;
        }
        mounts[dep.root] = cache;
        logger("AI component=agent.draft event=prebuilt_dependency root=%s cache_hit=%d artifact_paths=%zu copied_files=%zu",
            dep.root.c_str(), hit, dep.artifacts.size(), hit ? 0 : files.size());
    }
    return true;
#endif
}
bool dependency_links(const std::string& root, const std::map<std::string, std::string>& mounts) {
#ifndef _WIN32
    for (const auto& mount : mounts) {
        char target[4096];
        const ssize_t size = readlink((root+"/"+mount.first).c_str(), target, sizeof(target));
        if (size < 0 || std::string(target, static_cast<size_t>(size)) != mount.second) return false;
    }
#endif
    return true;
}
}

bool agent_draft_store_t::readonly_dependencies(std::map<std::string, std::string>& mounts, std::string& err) const {
    mounts.clear();
    std::vector<prebuilt_dependency_t> deps;
    agent_workspace_t source(user_root_);
    if (!source.prebuilt_dependencies(project_path_, deps, err)) return false;
#ifndef _WIN32
    std::string project_root;
    if (!agent_workspace_t::resolve_project_state_root(user_root_, project_path_, project_root, err)) return false;
    const std::string prefix = project_root+"/.webcool_agent/dependencies/";
    for (const auto& dep : deps) {
        char target[4096];
        const ssize_t size = readlink((root_path()+"/"+dep.root).c_str(), target, sizeof(target));
        if (size <= 0) {
            struct stat st;
            // Existing drafts created before a declaration keep their isolated
            // ordinary directory until materialize replaces it with a mount.
            if (lstat((root_path()+"/"+dep.root).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
            err = "missing read-only dependency mount: " + dep.root; return false;
        }
        const std::string path(target, static_cast<size_t>(size));
        if (path.compare(0, prefix.size(), prefix) != 0 || path.size() != prefix.size()+64
            || path.substr(prefix.size()).find_first_not_of("0123456789abcdef") != std::string::npos) {
            err = "invalid read-only dependency mount"; return false;
        }
        mounts[dep.root] = path;
    }
#endif
    return true;
}

bool agent_draft_store_t::materialize(
	const std::vector<agent_change_proposal_t>& changes,
	size_t& skipped_files, std::string& err, bool* reused, std::string* source_fingerprint, const std::function<bool()>& should_cancel) const
{
	if (reused) *reused = false;
	if (source_fingerprint) source_fingerprint->clear();
	std::string normalized_project;
	if (!valid_run_id(run_id_)
		|| !agent_workspace_t::normalize_path(project_path_, normalized_project,
			true, err) || normalized_project != project_path_)
	{
		if (err.empty()) err = "invalid private draft identity";
		return ai_error("agent.draft", "validate-identity", err);
	}
	std::string project_root;
	if (!agent_workspace_t::resolve_project_state_root(user_root_, project_path_,
		project_root, err)) return false;
	const std::string metadata = join_path(project_root, ".webcool_agent");
	const std::string worktrees = join_path(metadata, "worktrees");
	const std::string target = root_path();
	const std::string next = target + ".next";
	const std::string old = target + ".old";
	agent_workspace_t source(user_root_);
    std::vector<prebuilt_dependency_t> dependencies;
    if (!source.prebuilt_dependencies(project_path_, dependencies, err)) return false;
    std::vector<std::string> excluded;
    for (const auto& dep : dependencies) {
        excluded.push_back(dep.root);
        for (const auto& change : changes) for (const auto* input : {&change.path, &change.target_path}) {
            if (input->empty()) continue;
            std::string relative;
            if (!to_draft_path(*input, relative)) return false;
            if (dep_inside(dep.root, relative) || dep_inside(relative, dep.root)) {
                err = "prebuilt dependency is read-only: " + dep.root + "; change its declaration explicitly before editing or rebuilding it"; return false;
            }
        }
    }
    std::map<std::string, std::string> mounts;
    { draft_phase_t phase(run_id_, "prepare_dependencies");
      if (!(phase.ok = prepare_dependencies(user_root_, project_path_, metadata, dependencies, mounts, phase.budget, err, should_cancel))) return false; }
	std::string baseline, fingerprint_error;
	{ draft_phase_t phase(run_id_, "source_fingerprint");
	  phase.ok = source.tree_sha256(project_path_, baseline, fingerprint_error); }
	if (source_fingerprint) *source_fingerprint = baseline;
	std::string overlay;
	for (const auto& change : changes) for (const auto* value : {&change.operation, &change.path, &change.target_path, &change.content})
		overlay += std::to_string(value->size()) + ":" + *value;
	const std::string overlay_hash = agent_workspace_t::content_sha256(overlay);
	std::string saved_baseline, saved_overlay, saved_draft, saved_record, saved_manifest;
	const std::string manifest = write_manifest(*this, changes);
	size_t saved_skipped = 0;
	bool record_truncated = false;
	struct stat target_st, state_st;
	const bool target_exists = lstat(target.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode) && !S_ISLNK(target_st.st_mode);
	agent_workspace_t records(worktrees);
	if (lstat((target + ".snapshot").c_str(), &state_st) == 0
		&& records.read(run_id_ + ".snapshot", saved_record, record_truncated, fingerprint_error) && !record_truncated) {
		std::istringstream record(saved_record);
		record >> saved_baseline >> saved_overlay >> saved_draft >> saved_skipped;
		if (!record) saved_baseline.clear();
		else { std::string ignored; std::getline(record, ignored); std::getline(record, saved_manifest); }
	}
	bool trusted_previous = false;
	if (target_exists && !baseline.empty() && baseline == saved_baseline) {
		std::string current_draft;
		{ draft_phase_t phase(run_id_, "draft_fingerprint");
		  phase.ok = agent_workspace_t(target).tree_sha256("", current_draft, fingerprint_error);
		  trusted_previous = phase.ok && current_draft == saved_draft && dependency_links(target, mounts); }
		if (trusted_previous && saved_overlay == overlay_hash) {
			skipped_files = saved_skipped;
			logger("AI component=agent.draft event=materialize_decision run_id=%s mode=reuse reason=unchanged", run_id_.c_str());
			if (reused) *reused = true;
			return true;
		}
	}

    if (trusted_previous && !manifest.empty() && !saved_manifest.empty()) {
        bool attempted = false;
        if (update_verified_writes(*this, records, run_id_, target, baseline, overlay_hash,
            saved_manifest, manifest, changes, saved_skipped, attempted, err)) {
            skipped_files = saved_skipped;
            return true;
        }
        if (attempted) return false;
    }

    const char* reason = !target_exists ? "missing_draft" : baseline.empty() ? "fingerprint_unavailable"
        : baseline != saved_baseline ? "source_changed_or_snapshot_missing"
        : !trusted_previous ? "draft_changed" : "revision_changed";
    logger("AI component=agent.draft event=materialize_decision run_id=%s mode=rebuild reason=%s changes=%zu", run_id_.c_str(), reason, changes.size());
	if (!create_private_directory(metadata, err)
		|| !create_private_directory(worktrees, err)) return false;
	std::string stale_err;
	if (!cleanup_stale(7 * 24 * 60 * 60, stale_err)) {
		// Stale sibling cleanup is maintenance, not a reason to discard the current
		// run. Keep the failure observable and continue its isolated materialization.
		ai_log_error("agent.draft", "cleanup-stale-worktrees", stale_err);
	}
	if (!remove_tree(next, err) || !remove_tree(old, err)
		|| !create_private_directory(next, err)) return false;
	agent_workspace_t draft(next);
	skipped_files = 0;
	long long dependency_bytes = 0;
	bool prepared = false;
    { draft_phase_t phase(run_id_, "copy_source");
      prepared = phase.ok = copy_build_tree(source, project_path_, draft, "", skipped_files, dependency_bytes, err, phase.budget, excluded); }
    logger("AI component=agent.draft event=copy_summary run_id=%s skipped_files=%zu dependency_bytes=%lld ok=%d", run_id_.c_str(), skipped_files, dependency_bytes, prepared);
#ifndef _WIN32
    if (prepared) for (const auto& mount : mounts) {
        if (!dependency_parents(next, mount.first, err) || symlink(mount.second.c_str(), (next+"/"+mount.first).c_str()) != 0) {
            if (err.empty()) err = "cannot mount read-only prebuilt dependency";
            prepared = false; break;
        }
    }
#endif
    if (prepared) { draft_phase_t phase(run_id_, "apply_revision");
      prepared = phase.ok = apply_changes(draft, *this, changes, err); }
	if (!prepared)
	{
		std::string cleanup_err;
		if (!remove_tree(next, cleanup_err)) {
			ai_log_error("agent.draft", "cleanup-failed-materialization",
				cleanup_err);
		}
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
	// An unchanged tree returned above retains its build directory. Changed
	// revisions rebuild conservatively: reverting to formal bytes may restore
	// older mtimes, so carrying old object files forward could hide the revert.
	if (!baseline.empty()) {
		std::string draft_hash, state_error;
		bool hashed = false;
		{ draft_phase_t phase(run_id_, "snapshot_fingerprint");
		  hashed = phase.ok = agent_workspace_t(target).tree_sha256("", draft_hash, state_error); }
		if (hashed) {
			std::ostringstream record;
			record << baseline << "\n" << overlay_hash << "\n" << draft_hash << "\n" << skipped_files << "\n" << manifest << "\n";
			if (!records.save_generated_text(run_id_ + ".snapshot", record.str(), state_error))
				ai_log_error("agent.draft", "save-reuse-state", state_error);
		}
	}
	if (had_target) {
		std::string cleanup_err;
		draft_phase_t phase(run_id_, "remove_previous");
		phase.ok = remove_tree(old, cleanup_err, &phase.budget);
		if (!phase.ok) {
			ai_log_error("agent.draft", "cleanup-previous-worktree", cleanup_err);
		}
	}
	return true;
}

bool agent_draft_store_t::retire(const std::string& cleanup_id, bool& detached, std::string& err) const {
    detached = false;
    if (!valid_run_id(run_id_) || !valid_run_id(cleanup_id) || run_id_ == cleanup_id) {
        err = "invalid retired draft identity"; return false;
    }
    const std::string source = root_path();
    const std::string target = agent_draft_store_t(user_root_, project_path_, cleanup_id).root_path();
    if (source.empty() || target.empty()) { err = "cannot resolve retired draft"; return false; }
    struct stat st;
    if (lstat(source.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        err = "cannot inspect retiring draft"; return false;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) { err = "retiring draft is not a directory"; return false; }
    if (lstat(target.c_str(), &st) == 0 || errno != ENOENT) { err = "retired draft identity already exists"; return false; }
    if (!rename_directory(source, target)) { err = "cannot detach reviewed draft"; return false; }
    detached = true;
    // A long-lived finished draft may have an old root mtime. Keep concurrent
    // retention cleanup from selecting the newly detached directory.
#ifdef _WIN32
    const int touch_result = _utime(target.c_str(), NULL);
#else
    const int touch_result = utime(target.c_str(), NULL);
#endif
    if (touch_result != 0) ai_log_error("agent.draft", "touch-retired-draft", "cannot refresh retired draft retention time");
    std::string ignored;
    if (!remove_tree(source + ".snapshot", ignored)) ai_log_error("agent.draft", "remove-retired-snapshot", ignored);
    return true;
}

bool agent_draft_store_t::remove(std::string& err) const {
	if (!valid_run_id(run_id_)) {
		err = "invalid private draft identity";
		return ai_error("agent.draft", "validate-remove", err);
	}
	draft_phase_t phase(run_id_, "remove_reviewed");
	phase.ok = remove_tree(root_path(), err, &phase.budget) && remove_tree(root_path() + ".snapshot", err, &phase.budget);
	return phase.ok;
}

bool agent_draft_store_t::cleanup_stale(long long max_age_seconds,
	std::string& err) const
{
	if (max_age_seconds <= 0) {
		err = "invalid private draft retention";
		return ai_error("agent.draft", "validate-retention", err);
	}
	std::string project_root;
	if (!agent_workspace_t::resolve_project_state_root(user_root_, project_path_,
		project_root, err)) return false;
	const std::string worktrees = join_path(join_path(join_path(
		project_root, ".webcool_agent"), "worktrees"), "");
	DIR* directory = opendir(worktrees.c_str());
	if (directory == NULL) {
		if (errno == ENOENT) return true;
		err = "cannot enumerate private draft worktrees";
		return ai_error("agent.draft", "open-worktrees", err);
	}
	const time_t cutoff = time(NULL) - static_cast<time_t>(max_age_seconds);
	bool ok = true;
	for (dirent* entry = readdir(directory); entry != NULL;
		entry = readdir(directory))
	{
		const std::string name = entry->d_name;
		if (name == "." || name == ".." || name == run_id_
			|| name.find('/') != std::string::npos) continue;
		const std::string path = join_path(worktrees, name);
		struct stat st;
		if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode)
			|| !S_ISDIR(st.st_mode) || st.st_mtime >= cutoff) continue;
		std::string child_err;
		if (!remove_tree(path, child_err) || !remove_tree(path + ".snapshot", child_err)) {
			err = child_err;
			ok = false;
			break;
		}
	}
	closedir(directory);
	return ok;
}

} // namespace ai
} // namespace webcool
