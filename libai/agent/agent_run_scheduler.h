#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Resource scopes are independent from per-user/server admission quotas.
// Projects exclude overlapping projects and documents inside them. Documents
// exclude only the same file; free-form assistants exclude the same conversation.
struct agent_run_scope_t {
	std::string user_root;
	std::string project_path;
	std::string session_id;
	std::string document_path;
	std::string conversation_id;
	bool tool_free = false;
};

std::string normalize_agent_resource_path(const std::string &path);
bool agent_run_scopes_conflict(const agent_run_scope_t &left,
			       const agent_run_scope_t &right);

struct agent_run_slot_t {
	std::string key;
	std::string user_scope;
	unsigned long long sequence = 0;
	bool admitted = false;
	bool done = false;
	bool cancelled = false;
	bool paused = false;
};

// Returns the oldest pending run whose user still owns a free personal slot.
// An empty result means the global capacity is full or no run is eligible.
std::string select_next_agent_run(const std::vector<agent_run_slot_t> &runs,
				  size_t global_limit, size_t per_user_limit);

} // namespace ai
} // namespace webcool
