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

void ai_workspace_json_error(response_t &res, int status, const char *message,
    bool keep_alive, const char *file, int line, const char *function);

#define json_error(res, status, message, keep_alive)                        \
	ai_workspace_json_error(res, status, message, keep_alive, __FILE__, \
	    __LINE__, __FUNCTION__)

bool current_workspace(request_t &req, response_t &res, std::string &user_root);

std::string request_text(request_t &req, const char *name);

std::string json_text(acl::json_node *node);

bool json_bool(acl::json_node *node, bool fallback);

bool split_local_project_path(const std::string &input, std::string &parent,
    std::string &name, std::string &logical, std::string &err);

acl::json_node *json_array(acl::json_node *node);

// Live command results are transient just like model replies. Persistent audit
// remains metadata-only; stdout/stderr disappear on expiry or service restart.
struct sandbox_runtime_task_t {
	std::string id;
	std::string user_root;
	webcool::ai::sandbox_execution_plan_t plan;
	std::atomic<bool> cancel_requested;
	bool done = false;
	std::string status = "queued";
	long long started_at = 0;
	long long finished_at = 0;
	webcool::ai::sandbox_result_t result;

	sandbox_runtime_task_t()
	        : cancel_requested(false)
	{
	}
};

extern webcool::mutex g_sandbox_runtime_mutex;
extern std::map<std::string, std::shared_ptr<sandbox_runtime_task_t>>
    g_sandbox_runtime_tasks;
extern const size_t kMaxActiveSandboxRuns;
extern const size_t kMaxActiveSandboxRunsPerUser;
extern const size_t kMaxCompletedSandboxResults;
extern const long long kSandboxResultLifetimeSeconds;

std::string sandbox_runtime_key(
    const std::string &user_root, const std::string &id);

void cleanup_sandbox_runtime_locked(long long now);

bool reserve_sandbox_runtime(
    const std::shared_ptr<sandbox_runtime_task_t> &task, std::string &err);

void remove_sandbox_runtime(
    const std::string &user_root, const std::string &id);

struct sandbox_runtime_snapshot_t {
	std::string id;
	std::string project_path;
	std::string command_id;
	std::string status;
	bool cancel_requested = false;
	bool done = false;
	long long started_at = 0;
	long long finished_at = 0;
	webcool::ai::sandbox_result_t result;
};

bool snapshot_sandbox_runtime(const std::string &user_root,
    const std::string &id, sandbox_runtime_snapshot_t &snapshot);

bool cancel_sandbox_runtime(
    const std::string &user_root, const std::string &id, bool &already_done);

void run_async_sandbox_task(
    const std::shared_ptr<sandbox_runtime_task_t> &task);

void add_limits(acl::json &json, acl::json_node &root,
    const webcool::ai::sandbox_limits_t &limits);

}
}
