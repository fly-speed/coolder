#pragma once

#include "../agent/agent_protocol.h"
#include "../common/utf8_text.h"
#include "acl_cpp/lib_acl.hpp"
#include <algorithm>
#include <set>

namespace libai
{
namespace ai
{

// Return the storage path used for staged interface.
inline bool staged_interface_path(const std::string &path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos)
		return false;
	const std::string extension = path.substr(dot);
	return extension == ".h" || extension == ".hpp" || extension == ".hh" ||
	    extension == ".hxx" || extension == ".ixx" ||
	    extension == ".cppm" || extension == ".h++" || extension == ".H";
}

// Read from the authoritative staged snapshot, never a prior tool argument or
// the formal file. Keep whole small interfaces before implementation pages.
// The budget includes JSON escaping; pages explicitly describe missing bytes.
inline void append_staged_source(const agent_change_proposal_t &change,
    int priority, size_t budget, std::string &records,
    const std::string &source_view = "private_draft", bool complete_only = false)
{
	size_t page_bytes = complete_only ? change.content.size() : std::min(change.content.size(),
	    static_cast<size_t>(priority == 0 ? 16 * 1024 : 12 * 1024));
	for (;;) {
		const std::string page =
		    utf8_prefix(change.content, page_bytes);
		acl::json json;
		acl::json_node &root = json.create_node();
		root.add_text("path", change.path.c_str());
		root.add_text("source_view", source_view.c_str());
		root.add_text("file_sha256", change.draft_hash.c_str());
		root.add_number("generation", change.generation);
		root.add_number("offset", 0);
		root.add_number("total_bytes", change.content.size());
		root.add_number("next_offset", page.size());
		root.add_bool("eof", page.size() == change.content.size());
		if (page.size() < change.content.size())
			root.add_text(
			    "next_query", std::to_string(page.size()).c_str());
		root.add_text("content", page.c_str());
		const std::string record = root.to_string().c_str();
		if (records.size() + record.size() + 2 <= budget) {
			if (records.size() > 1)
				records += ",";
			records += record;
			break;
		}
		if (complete_only) break;
		if (page_bytes <= 512)
			break;
		page_bytes /= 2;
	}
}

// Serialize current staged source versions as bounded context records.
inline std::string staged_source_records(
    const std::vector<agent_change_proposal_t> &changes, size_t budget,
    const std::set<std::string> &retained_prefixes = {},
    const std::string &source_view = "private_draft")
{
	if (budget < 2)
		return "";
	std::string records = "[";
	std::vector<const agent_change_proposal_t *> sources;
	for (const auto &change : changes) {
		if (change.operation == "write" && change.review_status != "rejected" &&
		    change.content.find('\0') == std::string::npos &&
		    retained_prefixes.count(change.path) == 0)
			sources.push_back(&change);
	}
	// Keep whole small files and declarations first, then share remaining space
	// between implementation/test files. One large late proposal must not hide
	// every earlier file. Do not duplicate prefixes already in read_working_set.
	std::stable_sort(sources.begin(), sources.end(), [](const agent_change_proposal_t *a,
	    const agent_change_proposal_t *b) {
		const bool ah = staged_interface_path(a->path), bh = staged_interface_path(b->path);
		return ah != bh ? ah : a->content.size() < b->content.size();
	});
	// If the full escaped snapshot fits, retain complete files. Equal per-file
	// shares used to truncate even a snapshot smaller than the total budget.
	std::string complete = "[";
	bool all_fit = true;
	for (const auto *source : sources) {
		if (source->content.size() + complete.size() + 2 > budget) {
			all_fit = false;
			break;
		}
		const size_t previous = complete.size();
		append_staged_source(*source, 1, budget, complete, source_view, true);
		if (complete.size() == previous) {
			all_fit = false;
			break;
		}
	}
	if (all_fit) return complete + "]";
	for (size_t i = 0; i < sources.size() && records.size() + 1 < budget; ++i) {
		const size_t share = (budget - records.size() - 1) / (sources.size() - i);
		append_staged_source(*sources[i], staged_interface_path(sources[i]->path) ? 0 : 1,
		    records.size() + share, records, source_view);
	}
	return records + "]";
}

}
}
