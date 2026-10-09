#include "stdafx.h"
#include "workspace_change_set_internal.h"
namespace webcool
{
namespace ai
{
namespace change_set_detail
{
bool save_journal(const std::string &directory, const stored_set_t &set,
    const std::vector<std::string> &originals, std::string &err)
{
	if (originals.size() != set.changes.size()) {
		err = "invalid workspace recovery journal input";
		return false;
	}
	std::ostringstream serialized;
	serialized << kJournalHeaderV2 << '\n'
	           << set.id << '\n'
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
	FILE *out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = "cannot create workspace recovery journal";
		return false;
	}
	const bool wrote = data.empty() ||
	    fwrite(data.data(), 1, data.size(), out) == data.size();
	bool flushed = wrote && fflush(out) == 0;
#ifdef _WIN32
	if (flushed)
		flushed = _commit(_fileno(out)) == 0;
#else
	if (flushed)
		flushed = fsync(fileno(out)) == 0;
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
		if (dir_fd >= 0)
			close(dir_fd);
		err = "cannot flush workspace recovery journal directory";
		return false;
	}
	close(dir_fd);
#endif
	return true;
}

bool load_journal(const std::string &path, stored_set_t &set,
    std::vector<std::string> &originals, bool &found, std::string &err)
{
	found = false;
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	found = true;
	std::string line;
	std::string count_text;
	if (!std::getline(in, line) ||
	    (line != kJournalHeaderV1 && line != kJournalHeaderV2) ||
	    !std::getline(in, set.id) || !valid_id(set.id) ||
	    !std::getline(in, count_text)) {
		err = "invalid workspace recovery journal";
		return false;
	}
	const bool version2 = line == kJournalHeaderV2;
	char *end = NULL;
	const unsigned long count = strtoul(count_text.c_str(), &end, 10);
	if (end == count_text.c_str() || *end != '\0' || count < 1 ||
	    count > kMaxItems) {
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
		if (version2 &&
		    (!std::getline(in, change.operation) ||
		        !std::getline(in, encoded_path) ||
		        !std::getline(in, encoded_target))) {
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
		if (!std::getline(in, create_text) ||
		    !std::getline(in, change.original_sha256) ||
		    !std::getline(in, change.proposed_sha256) ||
		    !std::getline(in, encoded_original) ||
		    !hex_decode(encoded_path, change.path) ||
		    (version2 &&
		        !hex_decode(encoded_target, change.target_path)) ||
		    !hex_decode(encoded_original, original) ||
		    (create_text != "0" && create_text != "1")) {
			err = "invalid workspace recovery journal item";
			return false;
		}
		change.creates_file = create_text == "1";
		if (change.operation != "write" &&
		    change.operation != "delete" &&
		    change.operation != "move" && change.operation != "mkdir" &&
		    change.operation != "replace_empty_file_with_directory") {
			err = "invalid workspace recovery journal operation";
			return false;
		}
		total += original.size();
		const bool original_ok = change.operation == "mkdir" ?
		    (change.original_sha256 == "absent" && original.empty()) :
		    (change.creates_file ?
		            (change.original_sha256 == "absent" &&
		                original.empty()) :
		            agent_workspace_t::content_sha256(original) ==
		                change.original_sha256);
		if (original.size() > kMaxItemBytes || total > kMaxTotalBytes ||
		    !original_ok) {
			err =
			    "workspace recovery journal integrity validation failed";
			return false;
		}
		set.changes.push_back(change);
		originals.push_back(original);
	}
	return true;
}

bool remove_journal(const std::string &directory, std::string &err)
{
	const std::string path = journal_path(directory);
	if (remove(path.c_str()) != 0 && errno != ENOENT) {
		err = "cannot remove completed workspace recovery journal";
		return false;
	}
#ifndef _WIN32
	const int dir_fd = open(directory.c_str(), O_RDONLY);
	if (dir_fd < 0 || fsync(dir_fd) != 0) {
		if (dir_fd >= 0)
			close(dir_fd);
		err = "cannot flush completed workspace recovery state";
		return false;
	}
	close(dir_fd);
#endif
	return true;
}

bool recover_locked(const std::string &directory, const std::string &user_root,
    std::string &err)
{
	stored_set_t set;
	std::vector<std::string> originals;
	bool found = false;
	if (!load_journal(
	        journal_path(directory), set, originals, found, err)) {
		return false;
	}
	if (!found)
		return true;

	agent_workspace_t workspace(user_root);
	// Reverse order matches normal transactional rollback. Each comparison is
	// idempotent, so a second crash during recovery can safely resume here.
	for (size_t position = set.changes.size(); position > 0; --position) {
		const size_t i = position - 1;
		const stored_change_t &change = set.changes[i];
		if (rollback_change(workspace, change, originals[i], err))
			continue;
		return false;
	}
	return remove_journal(directory, err);
}

}
}
}
