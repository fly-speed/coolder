#include "stdafx.h"
#include "preview_project_context.h"
#include "../workspace/agent_workspace.h"
#include "agent_project_store.h"
#include "../agent/ai_admin_policy.h"
#include "../context/text_read_page.h"
#include <algorithm>
#include <limits.h>
#ifdef _WIN32
#include "../common/platform_compat.h"
#endif

namespace webcool { namespace ai {
namespace {
bool within(const std::string& root, const std::string& file) {
    return file.size() > root.size() && file.compare(0, root.size(), root) == 0
        && file[root.size()] == '/';
}
std::string json_text(acl::json_node& json) { acl::string text; json.to_string(&text); return text.c_str(); }
std::string failure(const std::string& message) {
    acl::json json; acl::json_node& root = json.create_node();
    root.add_bool("ok", false); root.add_text("error", message.c_str()); return json_text(root);
}
}
std::string preview_project_directory(const std::string& user_root,
    const std::string& document, bool local) {
    if (document.empty()) return "";
    std::string normalized, err;
    if (!local && !agent_workspace_t::normalize_path(document, normalized, false, err)) return "";
    std::vector<agent_project_record_t> projects;
    if (!agent_project_store_t(user_root).list(50, projects, err)) return "";
    for (const auto& project : projects) {
        if (!local && !within(project.project_path, normalized)) continue;
        std::string physical;
        if (!agent_workspace_t::resolve_project_root(user_root, project.project_path, physical, err)) continue;
        char root[PATH_MAX], file[PATH_MAX];
        if (!realpath(physical.c_str(), root)) continue;
        const std::string candidate = local ? document : physical + normalized.substr(project.project_path.size());
        if (!realpath(candidate.c_str(), file) || !within(root, file)) continue;
        const std::string relative = std::string(file).substr(std::string(root).size() + 1);
        if (agent_workspace_t::path_is_sensitive(relative)) continue;
        const std::string canonical(file);
        return canonical.substr(0, canonical.find_last_of('/'));
    }
    return "";
}
bool preview_read_tool(const std::string& name) {
    return name == "workspace.list" || name == "workspace.read" || name == "workspace.search";
}
std::string preview_project_read(const std::string& directory,
    const std::string& tool, const std::string& path, const std::string& query) {
    if (directory.empty() || !preview_read_tool(tool)) return failure("Only directory list, search and text reads are allowed.");
    agent_workspace_t workspace(directory);
    std::string err, resolved;
    if (!agent_workspace_t::resolve_project_root(directory, path, resolved, err)) return failure(err);
    if (resolved != directory && !within(directory, resolved)) return failure("Path is outside the preview directory.");
    acl::json json; acl::json_node& root = json.create_node(); root.add_bool("ok", true);
    root.add_text("path", path.c_str());
    if (tool == "workspace.list") {
        std::vector<workspace_entry_t> entries;
        if (!workspace.list(path, entries, err)) return failure(err);
        acl::json_node& items = json.create_array(); root.add_child("entries", items);
        const size_t count = std::min<size_t>(entries.size(), 200);
        for (size_t i = 0; i < count; ++i) {
            acl::json_node& item = items.add_child(false, true);
            item.add_text("path", entries[i].path.c_str()); item.add_bool("directory", entries[i].directory);
        }
        root.add_bool("truncated", entries.size() > count);
    } else if (tool == "workspace.read") {
        std::string content; bool truncated = false;
        if (!workspace.read(path, content, truncated, err)) return failure(err);
        if (truncated) return failure("File exceeds the text read limit.");
        const ai_admin_policy_t policy = ai_runtime_policy_get();
        text_read_page_t page;
        if (!read_text_page(content, query, page, err, policy.read_chunk_kib * 1024,
            policy.read_file_limit_kib * 1024)) return failure(err);
        root.add_text("content", page.content.c_str()); root.add_bool("eof", page.eof);
        root.add_text("next_query", page.eof ? "" : std::to_string(page.next_offset).c_str());
    } else {
        std::vector<workspace_match_t> matches; bool truncated = false;
        if (!workspace.search(path, query, matches, truncated, err)) return failure(err);
        acl::json_node& items = json.create_array(); root.add_child("matches", items);
        const size_t count = std::min<size_t>(matches.size(), 100);
        for (size_t i = 0; i < count; ++i) {
            acl::json_node& item = items.add_child(false, true);
            item.add_text("path", matches[i].path.c_str()); item.add_number("line", matches[i].line);
            item.add_text("text", matches[i].text.c_str());
        }
        root.add_bool("truncated", truncated || matches.size() > count);
    }
    return json_text(root);
}
} }
