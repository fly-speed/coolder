#pragma once

#include <string>
#include <vector>

namespace webcool {
namespace ai {

// One architectural module in a project plan. Paths are always relative to the
// authenticated user's virtual-disk root and are validated by the HTTP layer.
struct agent_project_module_t {
	std::string id;
	std::string name;
	std::string layer;
	std::string path;
	std::vector<std::string> dependencies;
};

// A bounded unit of work. Large projects progress by changing task states
// instead of asking one model response to create the whole repository.
struct agent_project_task_t {
	std::string id;
	std::string title;
	std::string module_id;
	std::string status;
	std::vector<std::string> dependencies;
	std::vector<std::string> acceptance_criteria;
	std::vector<std::string> test_plan;
};

struct agent_project_record_t {
	std::string id;
	std::string title;
	std::string project_path;
	std::string language;
	std::string platform;
	std::string goal;
	std::string status;
	long long created_at = 0;
	long long updated_at = 0;
	long long plan_version = 0;
	std::vector<agent_project_module_t> modules;
	std::vector<agent_project_task_t> tasks;
};

class agent_project_store_t {
public:
	explicit agent_project_store_t(const std::string& user_root);

	// Creates a project manifest for a newly scaffolded or existing workspace.
	// A user may have at most one manifest for each normalized project path.
	bool create(const std::string& title, const std::string& project_path,
		const std::string& language, const std::string& platform,
		agent_project_record_t& record, std::string& err) const;
	bool get(const std::string& id, agent_project_record_t& record,
		std::string& err) const;
	bool find_by_path(const std::string& project_path,
		agent_project_record_t& record, std::string& err) const;
	// Zero returns all registered projects; positive values select the newest N.
	bool list(size_t limit, std::vector<agent_project_record_t>& records,
		std::string& err) const;
	// Removes only WebCool's AI manifest. The workspace directory and every
	// source file beneath it are deliberately outside this operation.
	bool remove(const std::string& id, agent_project_record_t& removed,
		std::string& err) const;

	// Replaces the architecture and task graph in one atomic database write.
	// plan_version provides optimistic concurrency for multiple browser tabs.
	bool save_plan(const std::string& id, long long expected_plan_version,
		const std::string& goal,
		const std::vector<agent_project_module_t>& modules,
		const std::vector<agent_project_task_t>& tasks,
		agent_project_record_t& record, std::string& err) const;

	// Only explicit state transitions are accepted. Starting a task additionally
	// requires every dependency to be completed.
	bool update_task_status(const std::string& project_id,
		const std::string& task_id, const std::string& status,
		long long expected_plan_version, agent_project_record_t& record,
		std::string& err) const;

	static bool validate_plan(const std::vector<agent_project_module_t>& modules,
		const std::vector<agent_project_task_t>& tasks, std::string& err);
	static bool task_ready(const agent_project_record_t& project,
		const agent_project_task_t& task);

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
