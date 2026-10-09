#include "stdafx.h"
#include "agent_draft_store_internal.h"
namespace webcool
{
namespace ai
{
namespace draft_store_detail
{
bool valid_run_id(const std::string &value)
{
	return ::webcool::ai::identifiers::valid_id(value);
}

bool create_private_directory(const std::string &path, std::string &err)
{
	struct stat st;
	if (lstat(path.c_str(), &st) == 0) {
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode))
			return true;
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

bool remove_tree(
    const std::string &path, std::string &err, draft_io_budget_t *budget)
{
	draft_io_budget_t local;
	if (!budget)
		budget = &local;
	budget->checkpoint();
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return true;
	if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
		if (unlink(path.c_str()) == 0)
			return true;
		err = "cannot remove private draft file";
		return ai_error("agent.draft", "remove-file", err);
	}
	DIR *directory = opendir(path.c_str());
	if (directory == NULL) {
		err = "cannot enumerate private draft workspace";
		return ai_error("agent.draft", "open-directory", err);
	}
	bool ok = true;
	for (dirent *entry = readdir(directory); entry != NULL;
	     entry = readdir(directory)) {
		const std::string name = entry->d_name;
		if (name == "." || name == ".." ||
		    name.find('/') != std::string::npos) {
			continue;
		}
		std::string child_err;
		if (remove_tree(join_path(path, name), child_err, budget))
			continue;
		err = child_err;
		ok = false;
		break;
	}
	closedir(directory);
	if (!ok)
		return false;
#ifdef _WIN32
	const int removed = _rmdir(path.c_str());
#else
	const int removed = rmdir(path.c_str());
#endif
	if (!(removed != 0))
		return true;
	err = "cannot remove private draft workspace directory";
	return ai_error("agent.draft", "remove-directory", err);
}

bool rename_directory(const std::string &from, const std::string &to)
{
#ifdef _WIN32
	std::wstring from_wide;
	std::wstring to_wide;
	return webcool_utf8_path_to_wide(from.c_str(), from_wide) &&
	    webcool_utf8_path_to_wide(to.c_str(), to_wide) &&
	    MoveFileExW(from_wide.c_str(), to_wide.c_str(),
	        MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool copy_build_tree(agent_workspace_t &source, const std::string &source_dir,
    agent_workspace_t &draft, const std::string &draft_dir,
    size_t &skipped_files, long long &dependency_bytes, std::string &err,
    draft_io_budget_t &budget, const std::vector<std::string> &excluded)
{
#ifndef _WIN32
	if (draft_dir.empty())
		return source.copy_build_tree_to(source_dir, draft,
		    skipped_files, dependency_bytes,
		    [&budget] { budget.checkpoint(); }, err, excluded);
#endif

	std::vector<workspace_entry_t> entries;
	if (!source.list(source_dir, entries, err)) {
		return ai_error("agent.draft", "list-source", err);
	}
	for (size_t i = 0; i < entries.size(); ++i) {
		budget.checkpoint();
		const size_t slash = entries[i].path.rfind('/');
		const std::string name = slash == std::string::npos ?
		    entries[i].path :
		    entries[i].path.substr(slash + 1);
		if (draft_dir.empty() && name == ".webcool-build")
			continue;
		const std::string target =
		    draft_dir.empty() ? name : join_path(draft_dir, name);
		if (entries[i].directory) {
			if (!(!draft.create_directory_if_absent(target, err) ||
			        !copy_build_tree(source, entries[i].path, draft,
			            target, skipped_files, dependency_bytes,
			            err, budget)))
				continue;
			return ai_error("agent.draft", "copy-directory", err);

			continue;
		}
		const size_t dot = name.rfind('.');
		const std::string ext =
		    dot == std::string::npos ? "" : name.substr(dot);
		const bool library = ext == ".a" || ext == ".lib" ||
		    ext == ".so" || ext == ".dylib" || ext == ".dll" ||
		    name.find(".so.") != std::string::npos;
		if (library) {
			dependency_bytes += entries[i].size;
			if (entries[i].size > 512LL * 1024 * 1024 ||
			    dependency_bytes > 2LL * 1024 * 1024 * 1024) {
				err =
				    "private build dependencies exceed 512 MiB per file or 2 GiB total: " +
				    entries[i].path;
				return false;
			}
			if (source.copy_build_dependency(
			        entries[i].path, draft, target, err))
				continue;
			return false;
			continue;
		}
		// read() would truncate these files after reading a full MiB. They are
		// excluded from draft text anyway; libraries were handled above.
		if (entries[i].size > 1024 * 1024) {
			++skipped_files;
			continue;
		}
		std::string content;
		bool truncated = false;
		std::string read_err;
		if (!source.read(
		        entries[i].path, content, truncated, read_err, false) ||
		    truncated) {
			// Other binary and oversized assets remain in the formal project.
			// Library bytes above are copied privately, never returned to the model.
			++skipped_files;
			continue;
		}
		if (source.copy_build_dependency(
		        entries[i].path, draft, target, err))
			continue;
		return ai_error("agent.draft", "copy-file", err);
	}
	return true;
}

static bool create_draft_move_target(const agent_draft_store_t &store,
    agent_workspace_t &draft, const agent_change_proposal_t &change,
    const std::string &content, std::string &err)
{
	std::string target;
	if (!store.to_draft_path(change.target_path, target) ||
	    target.empty() ||
	    !draft.create_text_if_absent(target, content, err))
		return false;

	return true;
}

bool apply_changes(agent_workspace_t &draft, const agent_draft_store_t &store,
    const std::vector<agent_change_proposal_t> &changes, std::string &err)
{
	for (size_t i = 0; i < changes.size(); ++i) {
		std::string path;
		if (!store.to_draft_path(changes[i].path, path) ||
		    path.empty()) {
			err =
			    "draft change path is outside the selected project";
			return ai_error("agent.draft", "map-change-path", err);
		}
		if (changes[i].operation == "mkdir") {
			if (draft.create_directory_if_absent(path, err))
				continue;
			return false;
		} else if (changes[i].operation ==
		    "replace_empty_file_with_directory") {
			std::string current;
			bool truncated = false;
			if (!draft.read(path, current, truncated, err) ||
			    truncated || !current.empty())
				return false;
			if (!draft.delete_text_if_unchanged(path,
			        agent_workspace_t::content_sha256(current),
			        err) ||
			    !draft.create_directory_if_absent(path, err))
				return false;
		}
	}
	for (size_t i = 0; i < changes.size(); ++i) {
		std::string path;
		if (!store.to_draft_path(changes[i].path, path) ||
		    path.empty()) {
			err =
			    "draft change path is outside the selected project";
			return ai_error("agent.draft", "map-change-path", err);
		}
		if (changes[i].operation == "write") {
			if (draft.save_generated_text(
			        path, changes[i].content, err))
				continue;
			return false;
		} else if (changes[i].operation == "delete" ||
		    changes[i].operation == "move") {
			std::string content;
			bool truncated = false;
			if (!draft.read(path, content, truncated, err) ||
			    truncated)
				return false;
			if (changes[i].operation == "move" &&
			    !create_draft_move_target(
			        store, draft, changes[i], content, err))
				return false;
			if (!draft.delete_text_if_unchanged(path,
			        agent_workspace_t::content_sha256(content),
			        err))
				return false;
		}
	}
	return true;
}

// Manifest contains paths and hashes only, never source text. Older snapshots
// and structural operations deliberately fall back to the full transaction.
std::string write_manifest(const agent_draft_store_t &store,
    const std::vector<agent_change_proposal_t> &changes)
{
	std::map<std::string, std::string> entries;
	for (const auto &change : changes) {
		std::string path, normalized, err;
		if (!(change.operation != "write" ||
		        !store.to_draft_path(change.path, path) ||
		        !agent_workspace_t::normalize_path(
		            path, normalized, false, err) ||
		        path != normalized ||
		        !entries
		             .emplace(path,
		                 agent_workspace_t::content_sha256(
		                     change.content))
		             .second))
			continue;
		return "";
	}
	acl::json json;
	auto &root = json.create_node();
	root.add_text("format", "draft-writes-v1");
	for (const auto &item : entries)
		root.add_text(
		    ("path:" + item.first).c_str(), item.second.c_str());
	return root.to_string();
}

bool parse_write_manifest(
    const std::string &text, std::map<std::string, std::string> &entries)
{
	acl::json json(text.c_str());
	if (!json.finish())
		return false;
	auto *format = json["format"];
	if (!format || !format->get_text() ||
	    std::string(format->get_text()) != "draft-writes-v1")
		return false;
	auto &root = json.get_root();
	for (auto *node = root.first_child(); node; node = root.next_child()) {
		const char *tag = node->tag_name();
		if (!tag || std::string(tag) == "format")
			continue;
		if (std::string(tag).compare(0, 5, "path:") != 0 ||
		    !node->get_text())
			return false;
		entries[std::string(tag).substr(5)] = node->get_text();
	}
	return true;
}

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
    bool &attempted, std::string &err)
{
	attempted = false;
	std::map<std::string, std::string> previous, current;
	if (!parse_write_manifest(previous_manifest, previous) ||
	    !parse_write_manifest(manifest, current) || changes.size() > 512)
		return false;
	for (const auto &item : previous) {
		if (current.count(item.first))
			continue;
		return false;
	}
	struct edit_t {
		std::string path, before, after;
		bool existed;
	};
	std::vector<edit_t> edits;
	agent_workspace_t draft(target);
	size_t backup_bytes = 0;
	for (const auto &change : changes) {
		std::string path;
		store.to_draft_path(change.path, path);
		if (previous.count(path) && previous[path] == current[path]) {
			std::string existing, check_error;
			bool truncated = false;
			if (!(!draft.read(
			          path, existing, truncated, check_error) ||
			        truncated ||
			        agent_workspace_t::content_sha256(existing) !=
			            current[path]))
				continue;
			return false;
			continue;
		}
		if (change.content.size() > 1024 * 1024 ||
		    change.content.substr(0, 8192).find('\0') !=
		        std::string::npos)
			return false;
		const size_t slash = path.rfind('/');
		std::string parent, check_error;
		if (!agent_workspace_t::resolve_project_root(target,
		        slash == std::string::npos ? "" : path.substr(0, slash),
		        parent, check_error))
			return false;
		struct stat st;
		const std::string absolute = join_path(parent,
		    slash == std::string::npos ? path : path.substr(slash + 1));
		const bool exists = lstat(absolute.c_str(), &st) == 0;
		if ((!exists && errno != ENOENT) ||
		    (exists && !S_ISREG(st.st_mode)))
			return false;
		edit_t edit;
		edit.path = path;
		edit.after = change.content;
		edit.existed = exists;
		bool truncated = false;
		if (exists &&
		    (!draft.read(path, edit.before, truncated, check_error) ||
		        truncated))
			return false;
		backup_bytes += edit.before.size();
		if (backup_bytes > 16 * 1024 * 1024)
			return false;
		edits.push_back(edit);
	}
	attempted = true;
	draft_phase_t phase(run, "incremental_writes");
	if (!records.save_generated_text(
	        run + ".snapshot", "incremental-update-in-progress\n", err))
		return false;
	if (!remove_tree(
	        join_path(target, ".webcool-build"), err, &phase.budget))
		return false;
	size_t applied = 0;
	bool ok = true;
	for (const auto &edit : edits) {
		phase.budget.checkpoint();
		if (!draft.save_generated_text(edit.path, edit.after, err)) {
			ok = false;
			break;
		}
		++applied;
	}
	std::string hash;
	if (ok)
		ok = draft.tree_sha256("", hash, err);
	if (ok) {
		std::ostringstream record;
		record << baseline << "\n"
		       << overlay << "\n"
		       << hash << "\n"
		       << skipped << "\n"
		       << manifest << "\n";
		ok = records.save_generated_text(
		    run + ".snapshot", record.str(), err);
	}
	if (!ok) {
		bool restored = true;
		while (applied > 0) {
			const auto &edit = edits[--applied];
			std::string restore_error;
			if (edit.existed ?
			        draft.save_generated_text(
			            edit.path, edit.before, restore_error) :
			        draft.delete_text_if_unchanged(edit.path,
			            agent_workspace_t::content_sha256(
			                edit.after),
			            restore_error))
				continue;
			restored = false;
		}
		// Keep the snapshot invalid even after rollback. A later retry must
		// verify/reconstruct it; failed rollback is never presented as success.
		logger(
		    "AI component=agent.draft event=incremental_rollback run_id=%s restored=%d",
		    run.c_str(), restored);
		return false;
	}
	phase.ok = true;
	logger(
	    "AI component=agent.draft event=materialize_decision run_id=%s mode=incremental reason=verified_writes changed_files=%zu",
	    run.c_str(), edits.size());
	return true;
}

}

}
}
