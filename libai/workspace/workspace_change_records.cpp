#include "stdafx.h"
#include "workspace_change_set_internal.h"
namespace webcool
{
namespace ai
{
namespace change_set_detail
{
bool save_set(
    const std::string &directory, const stored_set_t &set, std::string &err)
{
	const std::string path = pending_path(directory, set.id);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(),
	    std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write workspace change set";
		return false;
	}
	out << kHeaderV2 << '\n'
	    << set.created_at << '\n'
	    << set.id << '\n'
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
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush workspace change set";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect workspace change set";
		return false;
	}
#endif
	if (replace_file(temporary, path))
		return true;
	remove(temporary.c_str());
	err = "cannot install workspace change set";
	return false;
}

bool load_set(const std::string &path, const std::string &id, stored_set_t &set,
    std::string &err)
{
	if (!valid_id(id)) {
		err = "invalid workspace change-set id";
		return false;
	}
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) {
		err = "workspace change set not found";
		return false;
	}
	std::string line;
	std::string count_text;
	if (!std::getline(in, line) ||
	    (line != kHeaderV1 && line != kHeaderV2)) {
		err = "invalid workspace change set";
		return false;
	}
	const bool version2 = line == kHeaderV2;
	if (!std::getline(in, line)) {
		err = "invalid workspace change set";
		return false;
	}
	char *end = NULL;
	set.created_at = strtoll(line.c_str(), &end, 10);
	if (end == line.c_str() || *end != '\0' || !std::getline(in, set.id) ||
	    set.id != id || !std::getline(in, count_text)) {
		err = "invalid workspace change set";
		return false;
	}
	end = NULL;
	const unsigned long count = strtoul(count_text.c_str(), &end, 10);
	if (end == count_text.c_str() || *end != '\0' || count < 1 ||
	    count > kMaxItems) {
		err = "invalid workspace change-set item count";
		return false;
	}
	set.changes.clear();
	size_t total = 0;
	for (unsigned long i = 0; i < count; ++i) {
		stored_change_t change;
		std::string encoded_path;
		std::string encoded_target;
		std::string create_text;
		std::string encoded_reason;
		std::string encoded_content;
		if (version2 &&
		    (!std::getline(in, change.operation) ||
		        !std::getline(in, encoded_path) ||
		        !std::getline(in, encoded_target))) {
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
		if (!std::getline(in, create_text) ||
		    !std::getline(in, change.original_sha256) ||
		    !std::getline(in, change.proposed_sha256) ||
		    !std::getline(in, encoded_reason) ||
		    !std::getline(in, encoded_content) ||
		    !hex_decode(encoded_path, change.path) ||
		    (version2 &&
		        !hex_decode(encoded_target, change.target_path)) ||
		    !hex_decode(encoded_reason, change.reason) ||
		    !hex_decode(encoded_content, change.content) ||
		    (create_text != "0" && create_text != "1")) {
			err = "invalid workspace change-set item";
			return false;
		}
		change.creates_file = create_text == "1";
		if (change.operation != "write" &&
		    change.operation != "delete" &&
		    change.operation != "move" && change.operation != "mkdir" &&
		    change.operation != "replace_empty_file_with_directory") {
			err = "invalid workspace change-set operation";
			return false;
		}
		total += change.content.size();
		const std::string digest =
		    agent_workspace_t::content_sha256(change.content);
		const bool digest_ok = change.operation == "mkdir" ?
		    (change.content.empty() &&
		        change.original_sha256 == "absent" &&
		        change.proposed_sha256 == "directory") :
		    change.operation == "delete" ?
		    (digest == change.original_sha256 &&
		        change.proposed_sha256 == "absent") :
		    change.operation == "replace_empty_file_with_directory" ?
		    (digest == change.original_sha256 &&
		        change.proposed_sha256 == "directory") :
		    digest == change.proposed_sha256;
		if (change.content.size() > kMaxItemBytes ||
		    total > kMaxTotalBytes ||
		    change.reason.size() > kMaxReasonBytes || !digest_ok) {
			err =
			    "workspace change-set integrity validation failed";
			return false;
		}
		set.changes.push_back(change);
	}
	return true;
}

}

}
}
