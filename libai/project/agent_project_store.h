#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// One architectural module in a project plan. Paths are always relative to the
// authenticated user's virtual-disk root and are validated by the HTTP layer.
struct agent_project_module_t {
	// Identifier used to look up this record.
	std::string id;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Context layer used when assembling the model prompt.
	std::string layer;
	// Path of the file or resource associated with this record.
	std::string path;
	// Identifiers or paths that this item depends on.
	std::vector<std::string> dependencies;
};

// A bounded unit of work. Large projects progress by changing task states
// instead of asking one model response to create the whole repository.
struct agent_project_task_t {
	// Identifier used to look up this record.
	std::string id;
	// User-visible title of this record.
	std::string title;
	// Identifier of the project module owning this item.
	std::string module_id;
	// Current lifecycle or outcome status.
	std::string status;
	// Identifiers or paths that this item depends on.
	std::vector<std::string> dependencies;
	// Conditions used to judge whether this task is complete.
	std::vector<std::string> acceptance_criteria;
	// Planned checks for validating this task.
	std::vector<std::string> test_plan;
};

// Project identity and the versioned module/task plan.
struct agent_project_record_t {
	// Identifier used to look up this record.
	std::string id;
	// User-visible title of this record.
	std::string title;
	// Logical path identifying the selected project.
	std::string project_path;
	// Language label used to select prompts or toolchains.
	std::string language;
	// Target platform selected for project creation or validation.
	std::string platform;
	// Overall objective recorded in the project plan.
	std::string goal;
	// Current lifecycle or outcome status.
	std::string status;
	// Creation time as seconds since the Unix epoch.
	long long created_at = 0;
	// Last update time as seconds since the Unix epoch.
	long long updated_at = 0;
	// Revision number of the stored project plan.
	long long plan_version = 0;
	// Declared modules used to organize the project plan.
	std::vector<agent_project_module_t> modules;
	// Tasks belonging to the current project plan.
	std::vector<agent_project_task_t> tasks;
};

// Persists and retrieves agent project records within the configured storage
// scope.
class agent_project_store_t {
public:
	// Bind the agent project store to the supplied storage scope.
	explicit agent_project_store_t(const std::string &user_root);

	// Creates a project manifest for a newly scaffolded or existing workspace.
	// A user may have at most one manifest for each normalized project path.
	bool create(const std::string &title, const std::string &project_path,
	    const std::string &language, const std::string &platform,
	    agent_project_record_t &record, std::string &err) const;
	// Read the record identified by the supplied key; report failures
	// through err.
	bool get(const std::string &id, agent_project_record_t &record,
	    std::string &err) const;
	// Return the storage path used for find by.
	bool find_by_path(const std::string &project_path,
	    agent_project_record_t &record, std::string &err) const;
	// Zero returns all registered projects; positive values select the newest N.
	bool list(size_t limit, std::vector<agent_project_record_t> &records,
	    std::string &err) const;
	// Removes only WebCool's AI manifest. The workspace directory and every
	// source file beneath it are deliberately outside this operation.
	bool remove(const std::string &id, agent_project_record_t &removed,
	    std::string &err) const;

	// Replaces the architecture and task graph in one atomic database write.
	// plan_version provides optimistic concurrency for multiple browser tabs.
	bool save_plan(const std::string &id, long long expected_plan_version,
	    const std::string &goal,
	    const std::vector<agent_project_module_t> &modules,
	    const std::vector<agent_project_task_t> &tasks,
	    agent_project_record_t &record, std::string &err) const;

	// Only explicit state transitions are accepted. Starting a task additionally
	// requires every dependency to be completed.
	bool update_task_status(const std::string &project_id,
	    const std::string &task_id, const std::string &status,
	    long long expected_plan_version, agent_project_record_t &record,
	    std::string &err) const;

	// Validate module/task identities, references and dependency
	// ordering.
	static bool validate_plan(
	    const std::vector<agent_project_module_t> &modules,
	    const std::vector<agent_project_task_t> &tasks, std::string &err);
	// Check that the task's prerequisites permit execution.
	static bool task_ready(const agent_project_record_t &project,
	    const agent_project_task_t &task);

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
