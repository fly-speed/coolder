#pragma once
#include "agent_staged_context.h"
#include "coding_test_changes.h"
#include "../workspace/agent_workspace.h"

namespace libai { namespace ai {

// A small, versioned navigation index complements exact source pages. It does
// not invent API contracts, request another planning turn, or gate valid edits.
inline std::string coding_interface_context(
    const std::vector<agent_change_proposal_t> &changes, size_t budget = 4096)
{
    if (budget < 256) return "";
    std::string records = "[";
    size_t files = 0;
    for (const auto &change : changes) {
        if (change.operation != "write" || change.review_status == "rejected" ||
            coding_test_source(change.path) || change.content.find('\0') != std::string::npos)
            continue;
        const auto dot = change.path.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : change.path.substr(dot);
        const std::set<std::string> extensions = {".go", ".h", ".hpp", ".hh", ".c", ".cc", ".cpp",
            ".rs", ".py", ".java", ".kt", ".swift", ".js", ".ts", ".tsx"};
        if (!extensions.count(ext)) continue;
        if (++files > 24) break;
        std::vector<workspace_outline_item_t> outline;
        bool truncated = false;
        source_outline(change.content, outline, truncated);
        if (outline.empty()) continue;
        acl::json json; auto &root = json.create_node();
        root.add_text("path", change.path.c_str());
        root.add_text("file_sha256", agent_workspace_t::content_sha256(change.content).c_str());
        auto &lines = json.create_array(); root.add_child("declaration_lines", lines);
        size_t count = 0;
        for (const auto &entry : outline) {
            if (entry.kind == "dependency") continue;
            if (++count > 8) break;
            auto &line = lines.add_child(false, true);
            line.add_number("line", entry.line).add_text("text", utf8_prefix(entry.text, 256).c_str());
        }
        if (!count) continue;
        // Always partial: lexical extraction cannot establish API completeness.
        root.add_bool("partial", true);
        const std::string encoded = root.to_string().c_str();
        if (records.size() + encoded.size() + 2 > budget) continue;
        if (records.size() > 1) records += ",";
        records += encoded;
    }
    return records == "[" ? "" : records + "]";
}
} }
