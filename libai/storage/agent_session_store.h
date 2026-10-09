#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// One conversation turn with its usage, reasoning and completion summary.
struct agent_session_message_t {
	// Conversation role, such as user or assistant.
	std::string role;
	// Text content presented or retained by this record.
	std::string text;
	// Assistant reasoning is stored with its own turn so the conversation UI can
	// restore each independently expandable reasoning disclosure after reload.
	std::string reasoning;
	// Per-run, user-facing completion record shown when revisiting history.
	std::string completion_summary;
	// Identifier of the associated agent run.
	std::string run_id;
	// Lifecycle state associated with this record.
	std::string state;
	// Creation time as seconds since the Unix epoch.
	long long created_at = 0;
	// Elapsed operation time in milliseconds.
	long long duration_ms = 0;
	// Provider-reported usage for this turn. Older saved conversations leave
	// these at zero and the UI simply omits the usage footer.
	long long input_tokens = 0;
	// Input tokens served from the provider's prompt cache.
	long long cached_input_tokens = 0;
	// Provider-reported generated token count.
	long long output_tokens = 0;
	// Generated tokens attributed to provider reasoning.
	long long reasoning_tokens = 0;
};

// Conversation identity, compact memory and bounded display history.
struct agent_session_record_t {
	// Identifier used to look up this record.
	std::string id;
	// User-visible title of this record.
	std::string title;
	// Registered agent selected for this run or session.
	std::string agent_id;
	// Identifier of the configured model provider.
	std::string provider_id;
	// Logical path identifying the selected project.
	std::string project_path;
	// The summary is compact model memory; messages are the bounded transcript
	// shown when the user returns to this conversation. Source files, tool output
	// and credentials remain deliberately excluded from both.
	std::string summary;
	// Ordered messages belonging to this conversation.
	std::vector<agent_session_message_t> messages;
	// Identifier of the most recent run in this conversation.
	std::string last_run_id;
	// Creation time as seconds since the Unix epoch.
	long long created_at = 0;
	// Last update time as seconds since the Unix epoch.
	long long updated_at = 0;
	// Number of conversation turns recorded so far.
	long long turn_count = 0;
};

// First nonempty sentence, bounded to the title storage limit (UTF-8 safe).
std::string agent_session_task_title(const std::string &prompt);

// Persists and retrieves agent session records within the configured storage
// scope.
class agent_session_store_t {
public:
	// Bind the agent session store to the supplied storage scope.
	explicit agent_session_store_t(const std::string &user_root);

	// Create a new persistent record; report failures through err.
	bool create(const std::string &title, const std::string &agent_id,
	    const std::string &provider_id, const std::string &project_path,
	    agent_session_record_t &record, std::string &err) const;
	// Append the completed turn and update bounded conversation memory
	// and usage.
	bool update_after_run(const std::string &id, const std::string &title,
	    const std::string &summary, const std::string &last_run_id,
	    const std::string &user_prompt, const std::string &assistant_reply,
	    const std::string &assistant_state,
	    const std::string &assistant_reasoning,
	    const std::string &completion_summary, long long duration_ms,
	    long long input_tokens, long long cached_input_tokens,
	    long long output_tokens, long long reasoning_tokens,
	    std::string &err) const;
	// Read the record identified by the supplied key; report failures
	// through err.
	bool get(const std::string &id, agent_session_record_t &record,
	    std::string &err) const;
	// Read the stored records in this scope; report failures through err.
	bool list(size_t limit, std::vector<agent_session_record_t> &records,
	    std::string &err) const;
	// Supplies the project-memory layer from recent conversations without ever
	// crossing project or user boundaries. The caller still decides how much of
	// each already-bounded summary enters a model prompt.
	bool list_for_project(const std::string &project_path,
	    const std::string &exclude_session_id, size_t limit,
	    std::vector<agent_session_record_t> &records,
	    std::string &err) const;
	// Remove the identified saved record; report failures through err.
	bool remove(const std::string &id, std::string &err) const;
	// Remove records owned by the specified project and report the
	// removed count.
	bool remove_for_project(const std::string &project_path,
	    size_t &removed_count, std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
