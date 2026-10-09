#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct agent_workflow_diagnostic_t {
	std::string path;
	long long line = 0;
	long long column = 0;
	std::string severity;
};

// Durable control checkpoint for the user-confirmed validation loop. It never
// stores source, prompts, model replies or raw stdout/stderr.
struct agent_workflow_checkpoint_t {
	std::string project_id;
	std::string session_id;
	std::string task_id;
	std::vector<std::string> command_queue;
	std::string failed_command_id;
	long long failed_exit_code = 0;
	std::vector<agent_workflow_diagnostic_t> diagnostics;
	long long updated_at = 0;
};

class agent_workflow_store_t {
public:
	explicit agent_workflow_store_t(const std::string &user_root);
	bool save(const agent_workflow_checkpoint_t &checkpoint,
	    std::string &err) const;
	bool load(const std::string &project_id, const std::string &session_id,
	    const std::string &task_id, agent_workflow_checkpoint_t &checkpoint,
	    bool &found, std::string &err) const;
	bool remove(const std::string &project_id,
	    const std::string &session_id, const std::string &task_id,
	    std::string &err) const;
	// Removes orphanable AI metadata only. Project source is never touched.
	bool remove_for_project(const std::string &project_id, size_t &removed,
	    std::string &err) const;
	bool remove_for_session(const std::string &session_id, size_t &removed,
	    std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
