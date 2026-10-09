#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct agent_session_message_t {
	std::string role;
	std::string text;
	// Assistant reasoning is stored with its own turn so the conversation UI can
	// restore each independently expandable reasoning disclosure after reload.
	std::string reasoning;
	// Per-run, user-facing completion record shown when revisiting history.
	std::string completion_summary;
	std::string run_id;
	std::string state;
	long long created_at = 0;
	long long duration_ms = 0;
	// Provider-reported usage for this turn. Older saved conversations leave
	// these at zero and the UI simply omits the usage footer.
	long long input_tokens = 0;
	long long cached_input_tokens = 0;
	long long output_tokens = 0;
	long long reasoning_tokens = 0;
};

struct agent_session_record_t {
	std::string id;
	std::string title;
	std::string agent_id;
	std::string provider_id;
	std::string project_path;
	// The summary is compact model memory; messages are the bounded transcript
	// shown when the user returns to this conversation. Source files, tool output
	// and credentials remain deliberately excluded from both.
	std::string summary;
	std::vector<agent_session_message_t> messages;
	std::string last_run_id;
	long long created_at = 0;
	long long updated_at = 0;
	long long turn_count = 0;
};

// First nonempty sentence, bounded to the title storage limit (UTF-8 safe).
std::string agent_session_task_title(const std::string &prompt);

class agent_session_store_t {
public:
	explicit agent_session_store_t(const std::string &user_root);

	bool create(const std::string &title, const std::string &agent_id,
		    const std::string &provider_id,
		    const std::string &project_path,
		    agent_session_record_t &record, std::string &err) const;
	bool update_after_run(const std::string &id, const std::string &title,
			      const std::string &summary,
			      const std::string &last_run_id,
			      const std::string &user_prompt,
			      const std::string &assistant_reply,
			      const std::string &assistant_state,
			      const std::string &assistant_reasoning,
			      const std::string &completion_summary,
			      long long duration_ms, long long input_tokens,
			      long long cached_input_tokens,
			      long long output_tokens,
			      long long reasoning_tokens,
			      std::string &err) const;
	bool get(const std::string &id, agent_session_record_t &record,
		 std::string &err) const;
	bool list(size_t limit, std::vector<agent_session_record_t> &records,
		  std::string &err) const;
	// Supplies the project-memory layer from recent conversations without ever
	// crossing project or user boundaries. The caller still decides how much of
	// each already-bounded summary enters a model prompt.
	bool list_for_project(const std::string &project_path,
			      const std::string &exclude_session_id,
			      size_t limit,
			      std::vector<agent_session_record_t> &records,
			      std::string &err) const;
	bool remove(const std::string &id, std::string &err) const;
	bool remove_for_project(const std::string &project_path,
				size_t &removed_count, std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
