#pragma once

#include <string>

namespace webcool {
namespace ai {

struct agent_checkpoint_t {
	std::string response_state_mode = "auto";
	std::string run_id;
	std::string provider_id;
	std::string project_path;
	// Encrypted at rest. Never log this field or return it to the browser.
	std::string prompt;
	// Interface language captured per run; never shared between users.
	std::string ui_language = "zh";
	std::string session_id;
	long long max_output_tokens = 0;
	// Provider-normalized per-run choice. Empty keeps compatibility with older
	// checkpoints and models that do not expose a thinking toggle.
	std::string thinking_mode;
	std::string reasoning_effort;
	// quick/standard/large changes bounded execution behavior and must survive a
	// service restart so recovery cannot silently switch planning strategy.
	std::string execution_mode;
	bool remember_session = false;
};

class agent_checkpoint_store_t {
public:
	agent_checkpoint_store_t(const std::string& upload_root,
		const std::string& user_root, const std::string& username);

	bool save(const agent_checkpoint_t& checkpoint, std::string& err) const;
	bool load(const std::string& run_id, agent_checkpoint_t& checkpoint,
		std::string& err) const;
	bool remove(const std::string& run_id, std::string& err) const;
	bool exists(const std::string& run_id) const;

private:
	std::string upload_root_;
	std::string user_root_;
	std::string username_;
};

} // namespace ai
} // namespace webcool
