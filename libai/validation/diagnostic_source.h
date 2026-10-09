#pragma once
#include "../workspace/agent_workspace.h"
#include <string>
#include <vector>
namespace webcool
{
namespace ai
{
// Select a source path only when diagnostic evidence identifies it uniquely.
inline std::string unique_diagnostic_source(
    const std::string &basename, const std::vector<std::string> &paths)
{
	std::string found;
	for (const auto &path : paths) {
		const auto slash = path.rfind('/');
		if (path.substr(slash == std::string::npos ? 0 : slash + 1) !=
		    basename)
			continue;
		if (!found.empty())
			return ""; // Never guess between packages.
		found = path;
	}
	return found;
}
// Collect bounded candidate source paths mentioned by diagnostics.
inline std::vector<std::string> diagnostic_source_files(
    const agent_workspace_t &workspace)
{
	std::vector<std::string> directories(1, ""), paths;
	size_t entries_seen = 0;
	for (size_t i = 0; i < directories.size(); ++i) {
		std::vector<workspace_entry_t> entries;
		std::string error;
		if (!workspace.list(directories[i], entries, error))
			return {};
		// list() caps at 2000 entries; a truncated inventory cannot prove uniqueness.
		if (entries.size() >= 2000)
			return {};
		for (const auto &entry : entries) {
			if (++entries_seen > 4096)
				return {};
			const auto slash = entry.path.rfind('/');
			const auto name = entry.path.substr(
			    slash == std::string::npos ? 0 : slash + 1);
			if (name.empty() || name[0] == '.' || name == "build" ||
			    name == "vendor" || name == "node_modules" ||
			    entry.path == "Library/Caches")
				continue;
			if (entry.directory && directories.size() >= 128)
				return {};
			if (entry.directory) {
				directories.push_back(entry.path);
			} else
				paths.push_back(entry.path);
		}
	}
	return paths;
}
}
}
