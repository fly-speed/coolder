#pragma once

#include <string>

namespace webcool
{
namespace ai
{

// Recovery metadata locating a run's saved context and project.
struct agent_checkpoint_t {
	// Provider continuation mode: retained state or stateless replay.
	std::string response_state_mode = "auto";
	// Identifier of the associated agent run.
	std::string run_id;
	// Identifier of the configured model provider.
	std::string provider_id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Encrypted at rest. Never log this field or return it to the browser.
	std::string prompt;
	// Interface language captured per run; never shared between users.
	std::string ui_language = "zh";
	// Identifier of the owning conversation session.
	std::string session_id;
	// Upper bound on generated tokens for one provider request.
	long long max_output_tokens = 0;
	// Provider-normalized per-run choice. Empty keeps compatibility with older
	// checkpoints and models that do not expose a thinking toggle.
	std::string thinking_mode;
	// Provider reasoning-effort setting selected for the request.
	std::string reasoning_effort;
	// quick/standard/large changes bounded execution behavior and must survive a
	// service restart so recovery cannot silently switch planning strategy.
	std::string execution_mode;
	// Whether conversation memory should be persisted.
	bool remember_session = false;
};

// Persists and retrieves agent checkpoint records within the configured
// storage scope.
class agent_checkpoint_store_t {
public:
	// Bind the agent checkpoint store to the supplied storage scope.
	agent_checkpoint_store_t(const std::string &upload_root,
	    const std::string &user_root, const std::string &username);

	// Persist the supplied record; report failures through err.
	bool save(const agent_checkpoint_t &checkpoint, std::string &err) const;
	// Load persisted data into the output record; report failures through
	// err.
	bool load(const std::string &run_id, agent_checkpoint_t &checkpoint,
	    std::string &err) const;
	// Remove the identified saved record; report failures through err.
	bool remove(const std::string &run_id, std::string &err) const;
	// Check whether the stored artifact is present.
	bool exists(const std::string &run_id) const;

private:
	// Root of the installation's uploaded and per-user data.
	std::string upload_root_;
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Authenticated account name associated with this object.
	std::string username_;
};

} // namespace ai
} // namespace webcool
