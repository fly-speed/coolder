#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_change_limits.h"
#include "workspace_change_set.h"
#include "agent_workspace.h"
#include "../common/ai_error_log.h"
#include "diff_preview.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace webcool {
namespace ai {
namespace {

webcool::mutex g_change_set_mutex;
const char* kHeaderV1 = "WEBCOOL_WORKSPACE_CHANGE_SET_V1";
const char* kHeaderV2 = "WEBCOOL_WORKSPACE_CHANGE_SET_V2";
const char* kJournalHeaderV1 = "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V1";
const char* kJournalHeaderV2 = "WEBCOOL_WORKSPACE_CHANGE_JOURNAL_V2";
const long long kLifetimeSeconds = 30 * 60;
const size_t kMaxItems = kMaxAgentChanges;
const size_t kMaxItemBytes = 1024 * 1024;
const size_t kMaxTotalBytes = 2 * 1024 * 1024;
const size_t kMaxReasonBytes = 1000;
const size_t kMaxDiffBytes = 128 * 1024;

struct stored_change_t {
	std::string operation;
	std::string path;
	std::string target_path;
	std::string content;
	std::string reason;
	std::string original_sha256;
	std::string proposed_sha256;
	bool creates_file = false;
};

struct stored_set_t {
	std::string id;
	long long created_at = 0;
	std::vector<stored_change_t> changes;
};

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

using ::webcool::ai::file_ops::safe_directory;

bool ensure_directory(const std::string& path) {
	if (safe_directory(path)) return true;
#ifdef _WIN32
	std::wstring wide;
	return webcool_utf8_path_to_wide(path.c_str(), wide)
		&& _wmkdir(wide.c_str()) == 0 && safe_directory(path);
#else
	return mkdir(path.c_str(), 0700) == 0 && safe_directory(path);
#endif
}

using ::webcool::ai::file_ops::replace_file;

bool prepare_directory(const std::string& user_root, std::string& directory,
	std::string& err)
{
	const std::string agent = join_path(user_root, ".webcool_agent");
	directory = join_path(agent, "change_sets");
	if (!ensure_directory(agent) || !ensure_directory(directory)) {
		err = "cannot create private workspace change-set directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(agent.c_str(), 0700) != 0 || chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect workspace change-set directory";
		return false;
	}
#endif
	return true;
}

std::string pending_path(const std::string& directory, const std::string& id) {
	// One authenticated user may review changes in several browser windows at
	// once. Keep each immutable preview under its own identifier; a single shared
	// pending.v1 file let a newer browser overwrite an older browser's preview.
	return join_path(directory, "pending-" + id + ".v2");
}

std::string legacy_pending_path(const std::string& directory) {
	// Retain read compatibility for previews created immediately before an
	// upgraded server restarted. New previews never use this singleton path.
	return join_path(directory, "pending.v1");
}

std::string journal_path(const std::string& directory) {
	return join_path(directory, "apply-journal.v1");
}

bool target_absent(agent_workspace_t& workspace, const std::string& path,
	std::string& err);
bool rollback_change(agent_workspace_t& workspace,
	const stored_change_t& change, const std::string& original,
	std::string& err);

// Write recovery metadata before touching project files. fflush()+fsync() and
// an atomic replacement ensure a crash yields either the previous complete
// journal or the new complete journal, never a partially parsed record.
bool save_journal(const std::string& directory, const stored_set_t& set,
	const std::vector<std::string>& originals, std::string& err)
{
	if (originals.size() != set.changes.size()) {
		err = "invalid workspace recovery journal input";
		return false;
	}
	std::ostringstream serialized;
	serialized << kJournalHeaderV2 << '\n' << set.id << '\n'
		<< set.changes.size() << '\n';
	for (size_t i = 0; i < set.changes.size(); ++i) {
		serialized << set.changes[i].operation << '\n'
			<< hex_encode(set.changes[i].path) << '\n'
			<< hex_encode(set.changes[i].target_path) << '\n'
			<< (set.changes[i].creates_file ? 1 : 0) << '\n'
			<< set.changes[i].original_sha256 << '\n'
			<< set.changes[i].proposed_sha256 << '\n'
			<< hex_encode(originals[i]) << '\n';
	}
	const std::string data = serialized.str();
	const std::string path = journal_path(directory);
	const std::string temporary = path + ".tmp";
	FILE* out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = "cannot create workspace recovery journal";
		return false;
	}
	const bool wrote = data.empty()
		|| fwrite(data.data(), 1, data.size(), out) == data.size();
	bool flushed = wrote && fflush(out) == 0;
#ifdef _WIN32
	if (flushed) flushed = _commit(_fileno(out)) == 0;
#else
	if (flushed) flushed = fsync(fileno(out)) == 0;
#endif
	const bool closed = fclose(out) == 0;
#ifndef _WIN32
	if (flushed && closed && chmod(temporary.c_str(), 0600) != 0) {
		flushed = false;
	}
#endif
	if (!flushed || !closed || !replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = "cannot persist workspace recovery journal";
		return false;
	}
#ifndef _WIN32
	// Persist the directory entry as well as the journal bytes. Some filesystems
	// may otherwise lose the rename after a sudden power failure.
	const int dir_fd = open(directory.c_str(), O_RDONLY);
	if (dir_fd < 0 || fsync(dir_fd) != 0) {
		if (dir_fd >= 0) close(dir_fd);
		err = "cannot flush workspace recovery journal directory";
		return false;
	}
	close(dir_fd);
#endif
	return true;
}

bool load_journal(const std::string& path, stored_set_t& set,
	std::vector<std::string>& originals, bool& found, std::string& err)
{
	found = false;
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) return true;
	found = true;
	std::string line;
	std::string count_text;
	if (!std::getline(in, line)
		|| (line != kJournalHeaderV1 && line != kJournalHeaderV2)
		|| !std::getline(in, set.id) || !valid_id(set.id)
		|| !std::getline(in, count_text)) {
		err = "invalid workspace recovery journal";
		return false;
	}
	const bool version2 = line == kJournalHeaderV2;
	char* end = NULL;
	const unsigned long count = strtoul(count_text.c_str(), &end, 10);
	if (end == count_text.c_str() || *end != '\0' || count < 1
		|| count > kMaxItems) {
		err = "invalid workspace recovery journal item count";
		return false;
	}
	set.changes.clear();
	originals.clear();
	size_t total = 0;
	for (unsigned long i = 0; i < count; ++i) {
		stored_change_t change;
		std::string encoded_path;
		std::string encoded_target;
		std::string create_text;
		std::string encoded_original;
		std::string original;
		if (version2 && (!std::getline(in, change.operation)
			|| !std::getline(in, encoded_path)
			|| !std::getline(in, encoded_target))) {
			err = "invalid workspace recovery journal operation";
			return false;
		}
		if (!version2) {
			change.operation = "write";
			if (!std::getline(in, encoded_path)) {
				err = "invalid workspace recovery journal item";
				return false;
			}
		}
		if (!std::getline(in, create_text)
			|| !std::getline(in, change.original_sha256)
			|| !std::getline(in, change.proposed_sha256)
			|| !std::getline(in, encoded_original)
			|| !hex_decode(encoded_path, change.path)
			|| (version2 && !hex_decode(encoded_target, change.target_path))
			|| !hex_decode(encoded_original, original)
			|| (create_text != "0" && create_text != "1")) {
			err = "invalid workspace recovery journal item";
			return false;
		}
		change.creates_file = create_text == "1";
		if (change.operation != "write" && change.operation != "delete"
			&& change.operation != "move" && change.operation != "mkdir"
			&& change.operation != "replace_empty_file_with_directory") {
			err = "invalid workspace recovery journal operation";
			return false;
		}
		total += original.size();
		const bool original_ok = change.operation == "mkdir"
			? (change.original_sha256 == "absent" && original.empty())
			: (change.creates_file
				? (change.original_sha256 == "absent" && original.empty())
				: agent_workspace_t::content_sha256(original)
					== change.original_sha256);
		if (original.size() > kMaxItemBytes || total > kMaxTotalBytes
			|| !original_ok) {
			err = "workspace recovery journal integrity validation failed";
			return false;
		}
		set.changes.push_back(change);
		originals.push_back(original);
	}
	return true;
}

bool remove_journal(const std::string& directory, std::string& err) {
	const std::string path = journal_path(directory);
	if (remove(path.c_str()) != 0 && errno != ENOENT) {
		err = "cannot remove completed workspace recovery journal";
		return false;
	}
#ifndef _WIN32
	const int dir_fd = open(directory.c_str(), O_RDONLY);
	if (dir_fd < 0 || fsync(dir_fd) != 0) {
		if (dir_fd >= 0) close(dir_fd);
		err = "cannot flush completed workspace recovery state";
		return false;
	}
	close(dir_fd);
#endif
	return true;
}

bool recover_locked(const std::string& directory,
	const std::string& user_root, std::string& err)
{
	stored_set_t set;
	std::vector<std::string> originals;
	bool found = false;
	if (!load_journal(journal_path(directory), set, originals, found, err)) {
		return false;
	}
	if (!found) return true;

	agent_workspace_t workspace(user_root);
	// Reverse order matches normal transactional rollback. Each comparison is
	// idempotent, so a second crash during recovery can safely resume here.
	for (size_t position = set.changes.size(); position > 0; --position) {
		const size_t i = position - 1;
		const stored_change_t& change = set.changes[i];
		if (!rollback_change(workspace, change, originals[i], err)) return false;
	}
	return remove_journal(directory, err);
}

void build_diff(const stored_change_t& change, const std::string& original,
	workspace_change_preview_item_t& item)
{
	if (change.operation == "replace_empty_file_with_directory") {
		item.diff = "replace empty file with directory " + change.path + "\n";
		return;
	}
	if (change.operation == "mkdir") {
		item.diff = "mkdir " + change.path + "\n";
		return;
	}
	if (change.operation == "move") {
		item.diff = "move " + change.path + " -> " + change.target_path + "\n";
		return;
	}
	const std::string proposed = change.operation == "delete"
		? "" : change.content;
	const line_diff_preview_t diff(original, proposed);
	const std::vector<line_diff_op_t>& operations = diff.operations;
	item.removed_lines = diff.removed_lines;
	item.added_lines = diff.added_lines;
	std::ostringstream out;
	out << "--- " << (change.creates_file ? "/dev/null" : change.path) << '\n'
		<< "+++ " << (change.operation == "delete" ? "/dev/null" : change.path) << '\n';
	const std::vector<bool>& visible = diff.visible;
	bool gap = false;
	for (size_t i = 0; i < operations.size(); ++i) {
		if (!visible[i]) {
			gap = true;
			continue;
		}
		if (gap) {
			out << "@@ unchanged lines omitted @@\n";
			gap = false;
		}
		const line_diff_op_t& operation = operations[i];
		diff.append_line(out, operation);
		if (static_cast<size_t>(out.tellp()) > kMaxDiffBytes - 1024) {
			out << "... diff truncated ...\n";
			break;
		}
	}
	item.diff = out.str();
	if (item.diff.size() > kMaxDiffBytes) item.diff.resize(kMaxDiffBytes);
}

bool target_absent(agent_workspace_t& workspace, const std::string& path,
	std::string& err)
{
	const size_t slash = path.rfind('/');
	const std::string parent = slash == std::string::npos
		? "" : path.substr(0, slash);
	std::vector<workspace_entry_t> entries;
	if (!workspace.list(parent, entries, err)) return false;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].path == path) {
			err = "workspace path already exists";
			return false;
		}
	}
	return true;
}

bool mkdir_input(const workspace_change_input_t& change) {
	return change.operation == "mkdir"
		|| change.operation == "replace_empty_file_with_directory";
}

size_t path_depth(const std::string& path) {
	return static_cast<size_t>(std::count(path.begin(), path.end(), '/'));
}

bool under_planned_directory(const std::string& path,
	const std::vector<std::string>& directories)
{
	for (size_t i = 0; i < directories.size(); ++i) {
		if (path.size() > directories[i].size()
			&& path.compare(0, directories[i].size(), directories[i]) == 0
			&& path[directories[i].size()] == '/') return true;
	}
	return false;
}

bool read_digest(agent_workspace_t& workspace, const std::string& path,
	std::string& digest, bool& absent, std::string& err)
{
	std::string inspect_error;
	if (target_absent(workspace, path, inspect_error)) {
		absent = true;
		digest.clear();
		return true;
	}
	absent = false;
	std::string content;
	bool truncated = false;
	if (inspect_error != "workspace path already exists"
		|| !workspace.read(path, content, truncated, err) || truncated) {
		if (err.empty()) err = inspect_error;
		return false;
	}
	digest = agent_workspace_t::content_sha256(content);
	return !digest.empty();
}

bool rollback_change(agent_workspace_t& workspace,
	const stored_change_t& change, const std::string& original,
	std::string& err)
{
	if (change.operation == "replace_empty_file_with_directory") {
		// Nested files are rolled back first because rollback walks in reverse.
		// Recovery can also run before this item was applied, so first recognize
		// an already-restored placeholder and the delete-before-mkdir crash gap.
		std::string digest;
		bool absent = false;
		std::string inspect_error;
		if (read_digest(workspace, change.path, digest, absent, inspect_error)) {
			if (!absent && digest == change.original_sha256) return true;
			if (absent) return workspace.create_text_if_absent(
				change.path, original, err);
			err = "placeholder path contains unexpected file content";
			return false;
		}
		// A directory cannot be read as text. At this point it should be empty.
		if (!workspace.delete_empty_directory(change.path, err)) return false;
		return workspace.create_text_if_absent(change.path, original, err);
	}
	if (change.operation == "mkdir") {
		std::string ignored;
		if (target_absent(workspace, change.path, ignored)) return true;
		return workspace.delete_empty_directory(change.path, err);
	}
	if (change.operation == "delete") {
		std::string digest;
		bool absent = false;
		if (!read_digest(workspace, change.path, digest, absent, err)) return false;
		if (!absent) {
			if (digest == change.original_sha256) return true;
			err = "deleted workspace file was recreated with different content";
			return false;
		}
		return workspace.create_text_if_absent(change.path, original, err);
	}
	if (change.operation == "move") {
		std::string source_digest;
		std::string target_digest;
		bool source_absent = false;
		bool target_is_absent = false;
		if (!read_digest(workspace, change.path, source_digest,
			source_absent, err) || !read_digest(workspace, change.target_path,
			target_digest, target_is_absent, err)) return false;
		if ((!source_absent && source_digest != change.original_sha256)
			|| (!target_is_absent && target_digest != change.original_sha256)) {
			err = "moved workspace file conflicts with subsequent content";
			return false;
		}
		if (source_absent && !workspace.create_text_if_absent(change.path,
			original, err)) return false;
		if (!target_is_absent && !workspace.delete_text_if_unchanged(
			change.target_path, change.original_sha256, err)) return false;
		return true;
	}
	if (change.creates_file) {
		std::string digest;
		bool absent = false;
		if (!read_digest(workspace, change.path, digest, absent, err)) return false;
		if (absent) return true;
		if (digest != change.proposed_sha256) {
			err = "new workspace file conflicts with subsequent content";
			return false;
		}
		return workspace.delete_text_if_unchanged(change.path,
			change.proposed_sha256, err);
	}
	std::string digest;
	bool absent = false;
	if (!read_digest(workspace, change.path, digest, absent, err) || absent) {
		if (err.empty()) err = "workspace file required for rollback is absent";
		return false;
	}
	if (digest == change.original_sha256) return true;
	if (digest != change.proposed_sha256) {
		err = "workspace file conflicts with subsequent content";
		return false;
	}
	return workspace.replace_text_if_unchanged(change.path,
		change.proposed_sha256, original, err);
}

bool save_set(const std::string& directory, const stored_set_t& set,
	std::string& err)
{
	const std::string path = pending_path(directory, set.id);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::binary
		| std::ios::trunc);
	if (!out.good()) { err = "cannot write workspace change set"; return false; }
	out << kHeaderV2 << '\n' << set.created_at << '\n' << set.id << '\n'
		<< set.changes.size() << '\n';
	for (size_t i = 0; i < set.changes.size(); ++i) {
		out << set.changes[i].operation << '\n'
			<< hex_encode(set.changes[i].path) << '\n'
			<< hex_encode(set.changes[i].target_path) << '\n'
			<< (set.changes[i].creates_file ? 1 : 0) << '\n'
			<< set.changes[i].original_sha256 << '\n'
			<< set.changes[i].proposed_sha256 << '\n'
			<< hex_encode(set.changes[i].reason) << '\n'
			<< hex_encode(set.changes[i].content) << '\n';
	}
	out.close();
	if (!out.good()) { remove(temporary.c_str()); err = "cannot flush workspace change set"; return false; }
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) { remove(temporary.c_str()); err = "cannot protect workspace change set"; return false; }
#endif
	if (!replace_file(temporary, path)) { remove(temporary.c_str()); err = "cannot install workspace change set"; return false; }
	return true;
}

bool load_set(const std::string& path, const std::string& id,
	stored_set_t& set, std::string& err)
{
	if (!valid_id(id)) { err = "invalid workspace change-set id"; return false; }
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) { err = "workspace change set not found"; return false; }
	std::string line;
	std::string count_text;
	if (!std::getline(in, line)
		|| (line != kHeaderV1 && line != kHeaderV2)) {
		err = "invalid workspace change set";
		return false;
	}
	const bool version2 = line == kHeaderV2;
	if (!std::getline(in, line)) { err = "invalid workspace change set"; return false; }
	char* end = NULL;
	set.created_at = strtoll(line.c_str(), &end, 10);
	if (end == line.c_str() || *end != '\0' || !std::getline(in, set.id)
		|| set.id != id || !std::getline(in, count_text))
	{ err = "invalid workspace change set"; return false; }
	end = NULL;
	const unsigned long count = strtoul(count_text.c_str(), &end, 10);
	if (end == count_text.c_str() || *end != '\0' || count < 1
		|| count > kMaxItems) { err = "invalid workspace change-set item count"; return false; }
	set.changes.clear();
	size_t total = 0;
	for (unsigned long i = 0; i < count; ++i) {
		stored_change_t change;
		std::string encoded_path;
		std::string encoded_target;
		std::string create_text;
		std::string encoded_reason;
		std::string encoded_content;
		if (version2 && (!std::getline(in, change.operation)
			|| !std::getline(in, encoded_path)
			|| !std::getline(in, encoded_target))) {
			err = "invalid workspace change-set operation";
			return false;
		}
		if (!version2) {
			change.operation = "write";
			if (!std::getline(in, encoded_path)) {
				err = "invalid workspace change-set item";
				return false;
			}
		}
		if (!std::getline(in, create_text)
			|| !std::getline(in, change.original_sha256)
			|| !std::getline(in, change.proposed_sha256)
			|| !std::getline(in, encoded_reason)
			|| !std::getline(in, encoded_content)
			|| !hex_decode(encoded_path, change.path)
			|| (version2 && !hex_decode(encoded_target, change.target_path))
			|| !hex_decode(encoded_reason, change.reason)
			|| !hex_decode(encoded_content, change.content)
			|| (create_text != "0" && create_text != "1"))
		{ err = "invalid workspace change-set item"; return false; }
		change.creates_file = create_text == "1";
		if (change.operation != "write" && change.operation != "delete"
			&& change.operation != "move" && change.operation != "mkdir"
			&& change.operation != "replace_empty_file_with_directory") {
			err = "invalid workspace change-set operation";
			return false;
		}
		total += change.content.size();
		const std::string digest = agent_workspace_t::content_sha256(change.content);
		const bool digest_ok = change.operation == "mkdir"
			? (change.content.empty() && change.original_sha256 == "absent"
				&& change.proposed_sha256 == "directory")
			: change.operation == "delete"
				? (digest == change.original_sha256
					&& change.proposed_sha256 == "absent")
			: change.operation == "replace_empty_file_with_directory"
				? (digest == change.original_sha256
					&& change.proposed_sha256 == "directory")
				: digest == change.proposed_sha256;
		if (change.content.size() > kMaxItemBytes || total > kMaxTotalBytes
			|| change.reason.size() > kMaxReasonBytes || !digest_ok)
		{ err = "workspace change-set integrity validation failed"; return false; }
		set.changes.push_back(change);
	}
	return true;
}

} // namespace

workspace_change_set_store_t::workspace_change_set_store_t(
	const std::string& user_root) : user_root_(user_root) {}

bool workspace_change_set_store_t::create(
	const std::vector<workspace_change_input_t>& changes,
	workspace_change_set_preview_t& preview, std::string& err) const
{
	std::lock_guard<webcool::mutex> guard(g_change_set_mutex);
	return create_locked(changes, preview, err);
}

static bool prepare_change_set_item(
	agent_workspace_t& workspace,
	const workspace_change_input_t& input,
	std::vector<std::string>& planned_directories,
	stored_set_t& set,
	size_t& total,
	workspace_change_set_preview_t& preview,
	std::string& err)
{
	stored_change_t change;
	change.operation = input.operation.empty()
		? "write" : input.operation;
	if (change.operation != "write" && change.operation != "delete"
		&& change.operation != "move" && change.operation != "mkdir"
		&& change.operation != "replace_empty_file_with_directory") {
		err = "workspace change operation is not supported";
		return ai_error("workspace.change-set", "validate-operation", err);
	}
	if (!agent_workspace_t::normalize_path(input.path, change.path,
		false, err)) return ai_error("workspace.change-set", "normalize-item", err);
	if (change.operation == "move"
		&& !agent_workspace_t::normalize_path(input.target_path,
			change.target_path, false, err)) {
		return ai_error("workspace.change-set", "normalize-move-target", err);
	}
	if (change.operation == "move" && change.path == change.target_path) {
		err = "workspace move source and target must differ";
		return ai_error("workspace.change-set", "compare-move-paths", err);
	}
	for (size_t j = 0; j < set.changes.size(); ++j) {
		const bool conflict = set.changes[j].path == change.path
			|| (!change.target_path.empty()
				&& (set.changes[j].path == change.target_path
					|| set.changes[j].target_path == change.target_path))
			|| (!set.changes[j].target_path.empty()
				&& set.changes[j].target_path == change.path);
		if (conflict) {
			err = "workspace change set contains duplicate paths";
			return ai_error("workspace.change-set", "check-duplicate", err);
		}
	}
	change.content = change.operation == "write" ? input.content : "";
	change.reason = input.reason.substr(0, kMaxReasonBytes);
	total += change.content.size();
	if (change.content.size() > kMaxItemBytes || total > kMaxTotalBytes
		|| change.content.find('\0') != std::string::npos
		|| (input.enforce_expected_current
			&& (input.expected_current_content.size() > kMaxItemBytes
				|| input.expected_current_content.find('\0')
					!= std::string::npos)))
	{
		err = "workspace change-set content exceeds the safe limit";
		return ai_error("workspace.change-set", "validate-content", err);
	}
	std::string original;
	bool truncated = false;
	std::string inspect_error;
	const bool virtual_parent = under_planned_directory(change.path,
		planned_directories);
	const bool absent = virtual_parent
		|| target_absent(workspace, change.path, inspect_error);
	if (virtual_parent) inspect_error.clear();
	if (input.enforce_expected_current) {
		if (input.expected_current_absent != absent) {
			err = "workspace file changed after AI generation";
			return ai_error("workspace.change-set",
				"compare-review-presence", err);
		}
		if (!absent) {
			std::string expected_actual;
			bool expected_truncated = false;
			std::string expected_err;
			if (inspect_error != "workspace path already exists"
				|| !workspace.read(change.path, expected_actual,
					expected_truncated, expected_err)
				|| expected_truncated
				|| expected_actual != input.expected_current_content)
			{
				err = expected_err.empty()
					? "workspace file changed after AI generation"
					: expected_err;
				return ai_error("workspace.change-set",
					"compare-review-content", err);
			}
		}
	}
	if (change.operation == "mkdir") {
		if (!absent) {
			err = inspect_error.empty() ? "workspace path already exists" : inspect_error;
			return ai_error("workspace.change-set", "inspect-new-directory", err);
		}
		change.original_sha256 = "absent";
		change.proposed_sha256 = "directory";
		change.creates_file = false;
		planned_directories.push_back(change.path);
	} else if (change.operation == "delete" || change.operation == "move"
		|| change.operation == "replace_empty_file_with_directory") {
		if (absent || inspect_error != "workspace path already exists"
			|| !workspace.read(change.path, original, truncated, inspect_error)
			|| truncated) {
			err = absent ? "workspace source file does not exist" : inspect_error;
			if (err.empty()) err = "workspace source file cannot be changed";
			return ai_error("workspace.change-set", "inspect-source-file", err);
		}
		change.content = original;
		total += original.size();
		if (total > kMaxTotalBytes) {
			err = "workspace change-set content exceeds the safe limit";
			return ai_error("workspace.change-set", "validate-source-content", err);
		}
		change.original_sha256 = agent_workspace_t::content_sha256(original);
		if (change.operation == "replace_empty_file_with_directory") {
			if (!original.empty()) {
				err = "only an empty placeholder file can be replaced with a directory";
				return ai_error("workspace.change-set",
					"inspect-empty-placeholder", err);
			}
			change.proposed_sha256 = "directory";
		} else {
			change.proposed_sha256 = change.operation == "delete"
				? "absent" : change.original_sha256;
		}
		if (change.operation == "move") {
			std::string target_error;
			if (!under_planned_directory(change.target_path, planned_directories)
				&& !target_absent(workspace, change.target_path, target_error))
			{
				err = target_error;
				return ai_error("workspace.change-set", "inspect-move-target", err);
			}
		}
	} else if (absent) {
		change.creates_file = true;
		change.original_sha256 = "absent";
		original.clear();
	} else if (inspect_error == "workspace path already exists"
		&& workspace.read(change.path, original, truncated, inspect_error))
	{
		if (truncated || original == change.content) {
			err = truncated ? "workspace file is too large to change"
				: "workspace change is identical to the current file";
			return ai_error("workspace.change-set", "compare-existing", err);
		}
		change.original_sha256 = agent_workspace_t::content_sha256(original);
	} else {
		err = inspect_error;
		return ai_error("workspace.change-set", "inspect-item", err);
	}
	if (change.operation == "replace_empty_file_with_directory") {
		// Children in the same preview see the post-confirmation directory,
		// even though preview generation itself never mutates the workspace.
		planned_directories.push_back(change.path);
	}
	if (change.operation == "write") {
		change.proposed_sha256 = agent_workspace_t::content_sha256(change.content);
	}
	if (change.proposed_sha256.empty()) {
		err = "cannot calculate workspace change digest";
		return ai_error("workspace.change-set", "digest-item", err);
	}
	workspace_change_preview_item_t item;
	item.operation = change.operation;
	item.path = change.path;
	item.target_path = change.target_path;
	item.reason = change.reason;
	item.original_sha256 = change.original_sha256;
	item.proposed_sha256 = change.proposed_sha256;
	item.creates_file = change.creates_file;
	item.deletes_file = change.operation == "delete"
		|| change.operation == "replace_empty_file_with_directory";
	item.creates_directory = change.operation == "mkdir"
		|| change.operation == "replace_empty_file_with_directory";
	build_diff(change, original, item);
	item.original_content = original;
	preview.items.push_back(item);
	set.changes.push_back(change);
	return true;
}

bool workspace_change_set_store_t::create_locked(
	const std::vector<workspace_change_input_t>& changes,
	workspace_change_set_preview_t& preview, std::string& err) const
{
	preview = workspace_change_set_preview_t();
	std::string directory;
	if (!prepare_directory(user_root_, directory, err)
		|| !recover_locked(directory, user_root_, err)) {
		return ai_error("workspace.change-set", "recover-before-create", err);
	}
	if (changes.empty() || changes.size() > kMaxItems) {
		err = "workspace change set must contain between 1 and " + std::to_string(kMaxItems) + " items";
		return ai_error("workspace.change-set", "validate-count", err);
	}
	agent_workspace_t workspace(user_root_);
	// A single proposal commonly contains mkdir plus files below that directory.
	// Preview does not mutate disk, so order parent directories first and remember
	// them as virtual parents while inspecting subsequent items. apply() persists
	// this same order; rollback already runs in reverse order.
	std::vector<workspace_change_input_t> ordered = changes;
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const workspace_change_input_t& left,
			const workspace_change_input_t& right) {
			if (mkdir_input(left) != mkdir_input(right)) return mkdir_input(left);
			return mkdir_input(left) && path_depth(left.path) < path_depth(right.path);
		});
	std::vector<std::string> planned_directories;
	stored_set_t set;
	set.id = new_id();
	set.created_at = static_cast<long long>(time(NULL));
	size_t total = 0;
	for (size_t i = 0; i < ordered.size(); ++i) {
		if (!prepare_change_set_item(workspace, ordered[i], planned_directories, set, total,
			preview, err)) return false;
	}
	if (set.id.empty()) { err = "cannot generate workspace change-set id"; return ai_error("workspace.change-set", "generate-id", err); }
	if (!save_set(directory, set, err))
		return ai_error("workspace.change-set", "save-plan", err);
	preview.id = set.id;
	return true;
}

bool workspace_change_set_store_t::apply(const std::string& id,
	std::vector<workspace_change_result_t>& results, std::string& err) const
{
	std::lock_guard<webcool::mutex> guard(g_change_set_mutex);
	return apply_locked(id, results, err);
}

bool workspace_change_set_store_t::apply_locked(const std::string& id,
	std::vector<workspace_change_result_t>& results, std::string& err) const
{
	results.clear();
	if (!valid_id(id)) {
		err = "invalid workspace change-set id";
		return ai_error("workspace.change-set", "validate-apply-id", err);
	}
	std::string directory;
	if (!prepare_directory(user_root_, directory, err)) return ai_error("workspace.change-set", "prepare-apply", err);
	if (!recover_locked(directory, user_root_, err)) {
		return ai_error("workspace.change-set", "recover-before-apply", err);
	}
	std::string pending = pending_path(directory, id);
	// A rolling upgrade can leave one legacy singleton preview behind. Only use
	// it when the ID-specific file is absent; load_set still verifies the ID.
	std::ifstream pending_probe(pending.c_str(), std::ios::in | std::ios::binary);
	if (!pending_probe.good()) pending = legacy_pending_path(directory);
	pending_probe.close();
	const std::string consumed = pending + ".consumed";
	remove(consumed.c_str());
	if (!replace_file(pending, consumed)) { err = "workspace change set not found or already consumed"; return ai_error("workspace.change-set", "consume-plan", err); }
	stored_set_t set;
	if (!load_set(consumed, id, set, err)) { remove(consumed.c_str()); return ai_error("workspace.change-set", "load-plan", err); }
	const long long now = static_cast<long long>(time(NULL));
	if (set.created_at <= 0 || now < set.created_at || now - set.created_at > kLifetimeSeconds) {
		remove(consumed.c_str()); err = "workspace change set expired"; return ai_error("workspace.change-set", "check-expiry", err);
	}
	agent_workspace_t workspace(user_root_);
	std::vector<std::string> originals(set.changes.size());
	std::vector<std::string> planned_directories;
	for (size_t i = 0; i < set.changes.size(); ++i) {
		const stored_change_t& change = set.changes[i];
		if (change.operation == "mkdir" || change.creates_file) {
			if (!under_planned_directory(change.path, planned_directories)
				&& !target_absent(workspace, change.path, err))
			{
				remove(consumed.c_str());
				return ai_error("workspace.change-set",
					"revalidate-new-file", err);
			}
			if (change.operation == "mkdir") {
				planned_directories.push_back(change.path);
			}
		} else {
			bool truncated = false;
			if (!workspace.read(change.path, originals[i], truncated, err)
				|| truncated || agent_workspace_t::content_sha256(originals[i])
					!= change.original_sha256)
			{
				remove(consumed.c_str());
				if (err.empty()) err = "workspace file changed after change-set preview";
				return ai_error("workspace.change-set", "revalidate-existing-file", err);
			}
			if (change.operation == "move"
				&& !under_planned_directory(change.target_path, planned_directories)
				&& !target_absent(workspace, change.target_path, err)) {
				remove(consumed.c_str());
				return ai_error("workspace.change-set", "revalidate-move-target", err);
			}
		}
		if (change.operation == "replace_empty_file_with_directory") {
			planned_directories.push_back(change.path);
		}
	}
	if (!save_journal(directory, set, originals, err)) {
		remove(consumed.c_str());
		return ai_error("workspace.change-set", "save-recovery-journal", err);
	}
	size_t applied = 0;
	for (; applied < set.changes.size(); ++applied) {
		const stored_change_t& change = set.changes[applied];
		bool ok = false;
		if (change.operation == "replace_empty_file_with_directory") {
			ok = workspace.delete_text_if_unchanged(change.path,
				change.original_sha256, err);
			if (ok && !workspace.create_directory_if_absent(change.path, err)) {
				// Keep the operation atomic even if directory creation fails after
				// the empty placeholder was removed.
				std::string restore_error;
				if (!workspace.create_text_if_absent(change.path,
					originals[applied], restore_error)) {
					ai_log_error("workspace.change-set",
						"restore-empty-placeholder", restore_error);
				}
				ok = false;
			}
		} else if (change.operation == "mkdir") {
			ok = workspace.create_directory_if_absent(change.path, err);
		} else if (change.operation == "delete") {
			ok = workspace.delete_text_if_unchanged(change.path,
				change.original_sha256, err);
		} else if (change.operation == "move") {
			ok = workspace.create_text_if_absent(change.target_path,
				originals[applied], err);
			if (ok && !workspace.delete_text_if_unchanged(change.path,
				change.original_sha256, err)) {
				std::string cleanup_error;
				if (!workspace.delete_text_if_unchanged(change.target_path,
					change.original_sha256, cleanup_error)) {
					ai_log_error("workspace.change-set", "cleanup-partial-move",
						cleanup_error);
				}
				ok = false;
			}
		} else {
			ok = change.creates_file
				? workspace.create_text_if_absent(change.path, change.content, err)
				: workspace.replace_text_if_unchanged(change.path,
					change.original_sha256, change.content, err);
		}
		if (!ok) break;
	}
	if (applied != set.changes.size()) {
		bool rollback_ok = true;
		while (applied > 0) {
			--applied;
			const stored_change_t& change = set.changes[applied];
			std::string rollback_error;
			const bool restored = rollback_change(workspace, change,
				originals[applied], rollback_error);
			if (!restored) { rollback_ok = false; ai_log_error("workspace.change-set", "rollback-item", rollback_error); }
		}
		remove(consumed.c_str());
		if (rollback_ok) {
			std::string cleanup_error;
			if (!remove_journal(directory, cleanup_error)) {
				rollback_ok = false;
				ai_log_error("workspace.change-set", "remove-rollback-journal",
					cleanup_error);
			}
		}
		if (!rollback_ok) err = "workspace change-set rollback failed; manual review is required";
		return ai_error("workspace.change-set", "apply-item", err);
	}
	remove(consumed.c_str());
	if (!remove_journal(directory, err)) {
		return ai_error("workspace.change-set", "complete-journal", err);
	}
	for (size_t i = 0; i < set.changes.size(); ++i) {
		workspace_change_result_t result;
		result.operation = set.changes[i].operation;
		result.path = set.changes[i].path;
		result.target_path = set.changes[i].target_path;
		result.sha256 = set.changes[i].proposed_sha256;
		result.created = set.changes[i].creates_file;
		result.deleted = set.changes[i].operation == "delete";
		results.push_back(result);
	}
	return true;
}

bool workspace_change_set_store_t::create_and_apply(
	const std::vector<workspace_change_input_t>& changes,
	workspace_change_set_preview_t& preview,
	std::vector<workspace_change_result_t>& results, std::string& err) const
{
	std::lock_guard<webcool::mutex> guard(g_change_set_mutex);
	if (!create_locked(changes, preview, err)) return false;
	return apply_locked(preview.id, results, err);
}

bool workspace_change_set_store_t::recover(std::string& err) const {
	std::lock_guard<webcool::mutex> guard(g_change_set_mutex);
	std::string directory;
	if (!prepare_directory(user_root_, directory, err)
		|| !recover_locked(directory, user_root_, err)) {
		return ai_error("workspace.change-set", "recover", err);
	}
	return true;
}

long long workspace_change_set_store_t::lifetime_seconds() {
	return kLifetimeSeconds;
}

} // namespace ai
} // namespace webcool
