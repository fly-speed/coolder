#pragma once

#include "agent_project_store.h"

#include <functional>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Filesystem capability enforcing access within an authorized workspace.
class agent_workspace_t;

// Bounded source declaration used as a navigation hint.
struct agent_project_symbol_t {
	// Bounded declaration/dependency navigation data. This is intentionally a
	// language-neutral semantic cache; exact source remains behind workspace.read.
	unsigned long line;
	// Category used to classify this entry.
	std::string kind;
	// Text content presented or retained by this record.
	std::string text;

	// Initialize agent project symbol state from the supplied arguments.
	agent_project_symbol_t()
	        : line(0)
	{
	}
};

// Cached metadata and outline information for one source file.
struct agent_project_index_entry_t {
	// Paths remain relative to the authenticated user's virtual-disk root.
	std::string path;
	// Identifier of the project module owning this item.
	std::string module_id;
	// Category used to classify this entry.
	std::string kind;
	// Language label used to select prompts or toolchains.
	std::string language;
	// Size associated with this entry or bounded collection.
	long long size;
	// Filesystem modification time as seconds since the Unix epoch.
	long long modified_at;
	// Source declarations collected for navigation.
	std::vector<agent_project_symbol_t> symbols;
};

// Bounded project-wide index with scan statistics and source entries.
struct agent_project_index_snapshot_t {
	// Identifier of the registered project record.
	std::string project_id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Revision identifier used to compare successive saved states.
	long long revision;
	// Time when the index was refreshed, in epoch seconds.
	long long indexed_at;
	// Number of directories visited while building the index.
	long long directory_count;
	// Directories omitted because of traversal policy or limits.
	long long skipped_directory_count;
	// Number of files affected by the planned change set.
	long long changed_file_count;
	// Whether limits prevented returning the complete result.
	bool truncated;
	// File paths or file records associated with this operation.
	std::vector<agent_project_index_entry_t> files;

	// Initialize agent project index snapshot state from the supplied
	// arguments.
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
	// Bind the agent project index store to the supplied storage scope.
	explicit agent_project_index_store_t(const std::string &user_root);

	// Load persisted data into the output record; report failures through
	// err.
	bool load(const agent_project_record_t &project,
	    agent_project_index_snapshot_t &snapshot, std::string &err) const;
	// Rebuild the project's bounded file index and persist the refreshed
	// snapshot.
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
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
