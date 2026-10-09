#pragma once

#include "agent_project_store.h"

#include <functional>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

class agent_workspace_t;

struct agent_project_symbol_t {
	// Bounded declaration/dependency navigation data. This is intentionally a
	// language-neutral semantic cache; exact source remains behind workspace.read.
	unsigned long line;
	std::string kind;
	std::string text;

	agent_project_symbol_t()
	        : line(0)
	{
	}
};

struct agent_project_index_entry_t {
	// Paths remain relative to the authenticated user's virtual-disk root.
	std::string path;
	std::string module_id;
	std::string kind;
	std::string language;
	long long size;
	long long modified_at;
	std::vector<agent_project_symbol_t> symbols;
};

struct agent_project_index_snapshot_t {
	std::string project_id;
	std::string project_path;
	long long revision;
	long long indexed_at;
	long long directory_count;
	long long skipped_directory_count;
	long long changed_file_count;
	bool truncated;
	std::vector<agent_project_index_entry_t> files;

	agent_project_index_snapshot_t()
	        : revision(0)
	        , indexed_at(0)
	        , directory_count(0)
	        , skipped_directory_count(0)
	        , changed_file_count(0)
	        , truncated(false)
	{
	}
};

// Durable per-project metadata index. Refreshing walks only safe workspace
// paths and stores no source content, API keys, prompts, or model responses.
class agent_project_index_store_t {
public:
	explicit agent_project_index_store_t(const std::string &user_root);

	bool load(const agent_project_record_t &project,
	    agent_project_index_snapshot_t &snapshot, std::string &err) const;
	bool refresh(const agent_project_record_t &project,
	    agent_workspace_t &workspace,
	    agent_project_index_snapshot_t &snapshot, std::string &err,
	    const std::function<void(const char *)> &phase = {}) const;
	// Removes the generated metadata snapshot only, never project source files.
	bool remove(const std::string &project_id, std::string &err) const;

	// Creates a bounded model-facing file/symbol map. Module ownership,
	// declarations and dependencies are retained, while exact file contents
	// remain behind explicit read tools.
	static std::string prompt_summary(
	    const agent_project_index_snapshot_t &snapshot, size_t byte_limit,
	    bool chinese = false);

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
