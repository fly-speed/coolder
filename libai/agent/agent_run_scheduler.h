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
	// Filesystem root belonging to the authenticated user.
	std::string user_root;
	// Logical path identifying the selected project.
	std::string project_path;
	// Identifier of the owning conversation session.
	std::string session_id;
	// Document associated with this run's resource scope.
	std::string document_path;
	// Conversation identifier used to isolate concurrent work.
	std::string conversation_id;
	// Whether the run can proceed without workspace tool access.
	bool tool_free = false;
};

// Normalize a resource path before comparing run scopes.
std::string normalize_agent_resource_path(const std::string &path);
// Test whether two run scopes compete for the same mutable resource.
bool agent_run_scopes_conflict(
    const agent_run_scope_t &left, const agent_run_scope_t &right);

// Scheduler entry recording admission and lifecycle state for one run.
struct agent_run_slot_t {
	// Lookup key for the associated resource or record.
	std::string key;
	// User identity used for per-user admission limits.
	std::string user_scope;
	// Queue ordering number assigned when the request is registered.
	unsigned long long sequence = 0;
	// Whether the scheduler has granted execution permission.
	bool admitted = false;
	// Whether processing has reached a terminal state.
	bool done = false;
	// Whether cancellation ended or will end this operation.
	bool cancelled = false;
	// Whether execution is currently suspended.
	bool paused = false;
};

// Returns the oldest pending run whose user still owns a free personal slot.
// An empty result means the global capacity is full or no run is eligible.
std::string select_next_agent_run(const std::vector<agent_run_slot_t> &runs,
    size_t global_limit, size_t per_user_limit);

} // namespace ai
} // namespace webcool
