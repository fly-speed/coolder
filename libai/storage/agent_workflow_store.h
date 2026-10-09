#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Diagnostic produced while validating a project workflow.
struct agent_workflow_diagnostic_t {
	// Path of the file or resource associated with this record.
	std::string path;
	// One-based source line number.
	long long line = 0;
	// Source column reported by the diagnostic.
	long long column = 0;
	// Diagnostic severity used for presentation and prioritization.
	std::string severity;
};

// Durable control checkpoint for the user-confirmed validation loop. It never
// stores source, prompts, model replies or raw stdout/stderr.
struct agent_workflow_checkpoint_t {
	// Identifier of the registered project record.
	std::string project_id;
	// Identifier of the owning conversation session.
	std::string session_id;
	// Identifier of the project-plan task being updated.
	std::string task_id;
	// Queued external commands awaiting execution.
	std::vector<std::string> command_queue;
	// Identifier of the command that caused validation to fail.
	std::string failed_command_id;
	// Exit code of the failed validation command.
	long long failed_exit_code = 0;
	// Validation or discovery diagnostics associated with this result.
	std::vector<agent_workflow_diagnostic_t> diagnostics;
	// Last update time as seconds since the Unix epoch.
	long long updated_at = 0;
};

// Persists and retrieves agent workflow records within the configured storage
// scope.
class agent_workflow_store_t {
public:
	// Bind the agent workflow store to the supplied storage scope.
	explicit agent_workflow_store_t(const std::string &user_root);
	// Persist the supplied record; report failures through err.
	bool save(const agent_workflow_checkpoint_t &checkpoint,
	    std::string &err) const;
	// Load persisted data into the output record; report failures through
	// err.
	bool load(const std::string &project_id, const std::string &session_id,
	    const std::string &task_id, agent_workflow_checkpoint_t &checkpoint,
	    bool &found, std::string &err) const;
	// Remove the identified saved record; report failures through err.
	bool remove(const std::string &project_id,
	    const std::string &session_id, const std::string &task_id,
	    std::string &err) const;
	// Removes orphanable AI metadata only. Project source is never touched.
	bool remove_for_project(const std::string &project_id, size_t &removed,
	    std::string &err) const;
	// Remove records owned by the specified conversation session.
	bool remove_for_session(const std::string &session_id, size_t &removed,
	    std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
