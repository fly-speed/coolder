#pragma once
#include "stdafx.h"
#include "libai/workspace/agent_change_limits.h"
#include "ai_workspace_actions.h"
#include "ai_agent_actions_internal.h"
#include "action/actions.h"
#include "action/action_util.h"
#include "libai/workspace/agent_workspace.h"
#include "libai/common/ai_error_log.h"
#include "libai/agent/ai_admin_policy.h"
#include "libai/sandbox/program_sandbox.h"
#include "libai/project/project_diagnostics.h"
#include "libai/project/agent_project_store.h"
#include "libai/project/project_scaffold.h"
#include "libai/project/project_plan_template.h"
#include "libai/project/project_toolchain.h"
#include "libai/sandbox/sandbox_execution.h"
#include "libai/workspace/workspace_patch.h"
#include "libai/workspace/workspace_change_set.h"
#include "common/webcool_mutex.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace action
{
namespace workspace_action_detail
{

// All workspace/sandbox HTTP failures pass this point before reaching the UI.
// Log only the bounded error summary, never file content or process output.
#undef json_error

// Log a workspace action failure and send its JSON error response.
void ai_workspace_json_error(response_t &res, int status, const char *message,
    bool keep_alive, const char *file, int line, const char *function);

#define json_error(res, status, message, keep_alive)                        \
	ai_workspace_json_error(res, status, message, keep_alive, __FILE__, \
	    __LINE__, __FUNCTION__)

// Resolve the request's authorized workspace or send an HTTP error.
bool current_workspace(request_t &req, response_t &res, std::string &user_root);

// Read a named scalar argument from the HTTP request.
std::string request_text(request_t &req, const char *name);

// Read a JSON scalar as text, using the supplied fallback when applicable.
std::string json_text(acl::json_node *node);

// Read a JSON boolean with the caller's fallback for missing values.
bool json_bool(acl::json_node *node, bool fallback);

// Return the storage path used for split local project.
bool split_local_project_path(const std::string &input, std::string &parent,
    std::string &name, std::string &logical, std::string &err);

// Return the array represented by this JSON node, or null.
acl::json_node *json_array(acl::json_node *node);

// Live command results are transient just like model replies. Persistent audit
// remains metadata-only; stdout/stderr disappear on expiry or service restart.
struct sandbox_runtime_task_t {
	// Identifier used to look up this record.
	std::string id;
	// Filesystem root belonging to the authenticated user.
	std::string user_root;
	// Confirmed command plan owned by this asynchronous execution.
	webcool::ai::sandbox_execution_plan_t plan;
	// Cancellation flag checked at safe execution boundaries.
	std::atomic<bool> cancel_requested;
	// Whether processing has reached a terminal state.
	bool done = false;
	// Current lifecycle or outcome status.
	std::string status = "queued";
	// Start time as seconds since the Unix epoch.
	long long started_at = 0;
	// Finish time as seconds since the Unix epoch.
	long long finished_at = 0;
	// Result collected from the completed operation.
	webcool::ai::sandbox_result_t result;

	// Initialize sandbox runtime task state from the supplied arguments.
	sandbox_runtime_task_t()
	        : cancel_requested(false)
	{
	}
};

// Protects the shared sandbox task registry and execution counts.
extern webcool::mutex g_sandbox_runtime_mutex;
// Live sandbox tasks keyed by authenticated user and run identifier.
extern std::map<std::string, std::shared_ptr<sandbox_runtime_task_t>>
    g_sandbox_runtime_tasks;
// Upper bound for active sandbox runs.
extern const size_t kMaxActiveSandboxRuns;
// Upper bound for active sandbox runs per user.
extern const size_t kMaxActiveSandboxRunsPerUser;
// Upper bound for completed sandbox results.
extern const size_t kMaxCompletedSandboxResults;
// Retention period in seconds for completed live sandbox results.
extern const long long kSandboxResultLifetimeSeconds;

// Combine user identity and run ID for isolated sandbox task lookup.
std::string sandbox_runtime_key(
    const std::string &user_root, const std::string &id);

// Expire completed sandbox tasks while the caller holds the runtime mutex.
void cleanup_sandbox_runtime_locked(long long now);

// Reserve a sandbox execution slot under global and per-user limits.
bool reserve_sandbox_runtime(
    const std::shared_ptr<sandbox_runtime_task_t> &task, std::string &err);

// Remove the identified task from the user's sandbox runtime registry.
void remove_sandbox_runtime(
    const std::string &user_root, const std::string &id);

// Copyable view of a sandbox task for status responses.
struct sandbox_runtime_snapshot_t {
	// Identifier used to look up this record.
	std::string id;
	// Logical path identifying the selected project.
	std::string project_path;
	// Identifier of the selected fixed sandbox command.
	std::string command_id;
	// Current lifecycle or outcome status.
	std::string status;
	// Cancellation flag checked at safe execution boundaries.
	bool cancel_requested = false;
	// Whether processing has reached a terminal state.
	bool done = false;
	// Start time as seconds since the Unix epoch.
	long long started_at = 0;
	// Finish time as seconds since the Unix epoch.
	long long finished_at = 0;
	// Result collected from the completed operation.
	webcool::ai::sandbox_result_t result;
};

// Copy a user's live sandbox state for an HTTP status response.
bool snapshot_sandbox_runtime(const std::string &user_root,
    const std::string &id, sandbox_runtime_snapshot_t &snapshot);

// Request cancellation and report whether the sandbox run already finished.
bool cancel_sandbox_runtime(
    const std::string &user_root, const std::string &id, bool &already_done);

// Execute the registered sandbox plan and publish its terminal result.
void run_async_sandbox_task(
    const std::shared_ptr<sandbox_runtime_task_t> &task);

// Serialize sandbox resource ceilings into the capability response.
void add_limits(acl::json &json, acl::json_node &root,
    const webcool::ai::sandbox_limits_t &limits);

}
}
