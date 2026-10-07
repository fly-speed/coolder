#pragma once

#include "../agent/agent_protocol.h"
#include "../common/utf8_text.h"
#include "acl_cpp/lib_acl.hpp"
#include <algorithm>

namespace webcool { namespace ai {

inline bool staged_interface_path(const std::string& path) {
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos) return false;
	const std::string extension = path.substr(dot);
	return extension == ".h" || extension == ".hpp" || extension == ".hh"
		|| extension == ".hxx" || extension == ".ixx" || extension == ".cppm"
		|| extension == ".h++" || extension == ".H";
}

// Read from the authoritative staged snapshot, never a prior tool argument or
// the formal file. Keep whole small interfaces before implementation pages.
// The budget includes JSON escaping; pages explicitly describe missing bytes.
inline std::string staged_source_records(
	const std::vector<agent_change_proposal_t>& changes, size_t budget) {
	if (budget < 2) return "";
	std::string records = "[";
	for (int priority = 0; priority < 2; ++priority) {
		for (auto i = changes.rbegin(); i != changes.rend(); ++i) {
			if (i->operation != "write" || i->review_status == "rejected"
				|| i->content.find('\0') != std::string::npos
				|| staged_interface_path(i->path) != (priority == 0)) continue;
			size_t page_bytes = std::min(i->content.size(),
				static_cast<size_t>(priority == 0 ? 16 * 1024 : 12 * 1024));
			for (;;) {
				const std::string page = utf8_prefix(i->content, page_bytes);
				acl::json json;
				acl::json_node& root = json.create_node();
				root.add_text("path", i->path.c_str());
				root.add_text("source_view", "private_draft");
				root.add_text("file_sha256", i->draft_hash.c_str());
				root.add_number("generation", i->generation);
				root.add_number("offset", 0);
				root.add_number("total_bytes", i->content.size());
				root.add_number("next_offset", page.size());
				root.add_bool("eof", page.size() == i->content.size());
				if (page.size() < i->content.size())
					root.add_text("next_query", std::to_string(page.size()).c_str());
				root.add_text("content", page.c_str());
				const std::string record = root.to_string().c_str();
				if (records.size() + record.size() + 2 <= budget) {
					if (records.size() > 1) records += ",";
					records += record;
					break;
				}
				if (page_bytes <= 512) break;
				page_bytes /= 2;
			}
		}
	}
	return records + "]";
}

} }
