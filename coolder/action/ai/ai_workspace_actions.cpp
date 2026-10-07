#include "stdafx.h"
#include "libai/workspace/agent_change_limits.h"
#include "ai_workspace_actions.h"
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
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace action {
namespace {

// All workspace/sandbox HTTP failures pass this point before reaching the UI.
// Log only the bounded error summary, never file content or process output.
#undef json_error

void ai_workspace_json_error(response_t& res, int status, const char* message,
	bool keep_alive, const char* file, int line, const char* function)
{
	webcool::ai::ai_log_error("http.ai.workspace", "response",
		message ? message : "unspecified workspace action error");
	action::json_error_at(res, status, message, keep_alive, file, line, function);
}

#define json_error(res, status, message, keep_alive) \
	ai_workspace_json_error(res, status, message, keep_alive, __FILE__, __LINE__, \
		__FUNCTION__)

bool current_workspace(request_t& req, response_t& res,
	std::string& user_root)
{
	const std::string upload_root = runtime_upload_dir_get();
	std::string username;
	bool admin = false;
	if (!auth_current_user(req, upload_root, username, admin)) {
		auth_send_required(req, res);
		return false;
	}
	std::string err;
	if (!authenticated_user_upload_dir(req, upload_root, user_root, err)) {
		json_error(res, err == "authentication required" ? 401 : 500,
			err.c_str(), req.isKeepAlive());
		return false;
	}
	return true;
}

std::string request_text(request_t& req, const char* name) {
	const char* value = req.getParameter(name);
	return value ? value : "";
}

std::string json_text(acl::json_node* node) {
	if (node == NULL) return "";
	const char* value = node->get_string();
	if (value == NULL) value = node->get_text();
	return value ? value : "";
}

bool json_bool(acl::json_node* node, bool fallback) {
	if (node == NULL) return fallback;
	const std::string value = json_text(node);
	if (value == "true" || value == "1") return true;
	if (value == "false" || value == "0") return false;
	return fallback;
}

bool external_project_allowed(request_t& req, const std::string& scope,
	std::string& err)
{
	if (scope != "personal") {
		err = "coolder supports the configured workspace only";
		return false;
	}

	if (scope == "personal") return true;
	if (scope != "shared" && scope != "local") {
		err = "unsupported project storage scope";
		return false;
	}
	std::string username;
	bool admin = false;
	const std::string upload_root = runtime_upload_dir_get();
	if (!auth_current_user(req, upload_root, username, admin)) {
		err = "authentication required";
		return false;
	}
	const webcool::ai::ai_admin_policy_t policy =
		webcool::ai::ai_runtime_policy_get();
	if (!admin && ((scope == "shared" && !policy.allow_users_shared_projects)
		|| (scope == "local" && !policy.allow_users_local_projects)))
	{
		err = scope == "shared"
			? "administrator has disabled shared-directory projects"
			: "administrator has disabled local-disk projects";
		return false;
	}
	if (scope == "local" && !local_disk_access_allowed(upload_root, admin, err)) {
		if (err.empty()) err = "local disk access is disabled";
		return false;
	}
	return true;
}

bool split_local_project_path(const std::string& input, std::string& parent,
	std::string& name, std::string& logical, std::string& err)
{
	std::string raw = input;
	while (raw.size() > 1 && (raw[raw.size() - 1] == '/'
		|| raw[raw.size() - 1] == '\\')) raw.resize(raw.size() - 1);
#ifdef _WIN32
	const bool absolute = (raw.size() >= 3 && raw[1] == ':'
		&& (raw[2] == '/' || raw[2] == '\\'))
		|| (raw.size() >= 2 && raw[0] == '\\' && raw[1] == '\\');
#else
	const bool absolute = !raw.empty() && raw[0] == '/';
#endif
	const size_t slash = raw.find_last_of("/\\");
	if (!absolute || slash == std::string::npos || slash + 1 >= raw.size()) {
		err = "local project path must be an absolute directory path";
		return false;
	}
	parent = raw.substr(0, slash);
	if (parent.empty()) parent = "/";
	name = raw.substr(slash + 1);
	if (name == "." || name == "..") {
		err = "invalid local project directory name";
		return false;
	}
	std::string normalized;
	if (!webcool::ai::agent_workspace_t::normalize_path(raw, normalized, false,
		err)) return false;
	logical = std::string("本地磁盘/") + normalized;
	return true;
}

acl::json_node* json_array(acl::json_node* node) {
	if (node == NULL) return NULL;
	if (node->is_array()) return node;
	acl::json_node* value = node->get_obj();
	return value != NULL && value->is_array() ? value : NULL;
}

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

	sandbox_runtime_task_t() : cancel_requested(false) {}
};

webcool::mutex g_sandbox_runtime_mutex;
std::map<std::string, std::shared_ptr<sandbox_runtime_task_t> >
	g_sandbox_runtime_tasks;
const size_t kMaxActiveSandboxRuns = 4;
const size_t kMaxActiveSandboxRunsPerUser = 1;
const size_t kMaxCompletedSandboxResults = 20;
const long long kSandboxResultLifetimeSeconds = 60 * 60;

std::string sandbox_runtime_key(const std::string& user_root,
	const std::string& id)
{
	return user_root + "\n" + id;
}

void cleanup_sandbox_runtime_locked(long long now) {
	for (std::map<std::string, std::shared_ptr<sandbox_runtime_task_t> >::iterator
		it = g_sandbox_runtime_tasks.begin(); it != g_sandbox_runtime_tasks.end();)
	{
		if (it->second->done && it->second->finished_at > 0
			&& now - it->second->finished_at > kSandboxResultLifetimeSeconds)
		{
			it = g_sandbox_runtime_tasks.erase(it);
		} else ++it;
	}
	for (;;) {
		size_t completed = 0;
		std::map<std::string, std::shared_ptr<sandbox_runtime_task_t> >::iterator
			oldest = g_sandbox_runtime_tasks.end();
		for (std::map<std::string,
			std::shared_ptr<sandbox_runtime_task_t> >::iterator
			it = g_sandbox_runtime_tasks.begin();
			it != g_sandbox_runtime_tasks.end(); ++it)
		{
			if (!it->second->done) continue;
			++completed;
			if (oldest == g_sandbox_runtime_tasks.end()
				|| it->second->finished_at < oldest->second->finished_at) oldest = it;
		}
		if (completed <= kMaxCompletedSandboxResults
			|| oldest == g_sandbox_runtime_tasks.end()) break;
		g_sandbox_runtime_tasks.erase(oldest);
	}
}

bool reserve_sandbox_runtime(
	const std::shared_ptr<sandbox_runtime_task_t>& task, std::string& err)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	cleanup_sandbox_runtime_locked(static_cast<long long>(time(NULL)));
	size_t active = 0;
	size_t user_active = 0;
	for (std::map<std::string, std::shared_ptr<sandbox_runtime_task_t> >::iterator
		it = g_sandbox_runtime_tasks.begin(); it != g_sandbox_runtime_tasks.end(); ++it)
	{
		if (it->second->done) continue;
		++active;
		if (it->second->user_root == task->user_root) ++user_active;
	}
	if (active >= kMaxActiveSandboxRuns
		|| user_active >= kMaxActiveSandboxRunsPerUser)
	{
		err = "sandbox execution concurrency limit reached";
		return false;
	}
	const std::string key = sandbox_runtime_key(task->user_root, task->id);
	if (g_sandbox_runtime_tasks.find(key) != g_sandbox_runtime_tasks.end()) {
		err = "sandbox execution plan is already running";
		return false;
	}
	g_sandbox_runtime_tasks[key] = task;
	return true;
}

void remove_sandbox_runtime(const std::string& user_root,
	const std::string& id)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	g_sandbox_runtime_tasks.erase(sandbox_runtime_key(user_root, id));
}

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

bool snapshot_sandbox_runtime(const std::string& user_root,
	const std::string& id, sandbox_runtime_snapshot_t& snapshot)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	cleanup_sandbox_runtime_locked(static_cast<long long>(time(NULL)));
	const std::map<std::string,
		std::shared_ptr<sandbox_runtime_task_t> >::const_iterator it =
		g_sandbox_runtime_tasks.find(sandbox_runtime_key(user_root, id));
	if (it == g_sandbox_runtime_tasks.end()) return false;
	const std::shared_ptr<sandbox_runtime_task_t>& task = it->second;
	snapshot.id = task->id;
	snapshot.project_path = task->plan.project_path;
	snapshot.command_id = task->plan.command.id;
	snapshot.status = task->status;
	snapshot.cancel_requested = task->cancel_requested.load(
		std::memory_order_relaxed);
	snapshot.done = task->done;
	snapshot.started_at = task->started_at;
	snapshot.finished_at = task->finished_at;
	snapshot.result = task->result;
	return true;
}

bool cancel_sandbox_runtime(const std::string& user_root,
	const std::string& id, bool& already_done)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	const std::map<std::string,
		std::shared_ptr<sandbox_runtime_task_t> >::iterator it =
		g_sandbox_runtime_tasks.find(sandbox_runtime_key(user_root, id));
	if (it == g_sandbox_runtime_tasks.end()) return false;
	already_done = it->second->done;
	if (!already_done) {
		it->second->cancel_requested.store(true, std::memory_order_relaxed);
		it->second->status = "cancelling";
	}
	return true;
}

void run_async_sandbox_task(
	const std::shared_ptr<sandbox_runtime_task_t>& task)
{
	{
		std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
		task->status = "running";
		task->started_at = static_cast<long long>(time(NULL));
	}
	webcool::ai::sandbox_result_t result;
	bool dispatched = false;
	try {
		std::vector<webcool::ai::sandbox_command_t> commands(1,
			task->plan.command);
		webcool::ai::program_sandbox_t sandbox(task->user_root,
			task->plan.project_path, commands, task->plan.limits);
		webcool::ai::sandbox_request_t request;
		request.command_id = task->plan.command.id;
		dispatched = sandbox.execute(request, result,
			&task->cancel_requested);
	} catch (const std::exception& exception) {
		result.error = exception.what();
		webcool::ai::ai_log_error("sandbox.runtime", "exception", result.error);
	} catch (...) {
		result.error = "unexpected sandbox runtime exception";
		webcool::ai::ai_log_error("sandbox.runtime", "unknown-exception",
			result.error);
	}
	webcool::ai::sandbox_execution_store_t store(task->user_root);
	std::string audit_error;
	(void) store.audit_finished(task->plan, result, audit_error);
	{
		std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
		task->result = result;
		task->done = true;
		task->finished_at = static_cast<long long>(time(NULL));
		if (result.cancelled) task->status = "cancelled";
		else if (!dispatched || result.timed_out || result.output_truncated
			|| !result.error.empty()) task->status = "failed";
		else task->status = "completed";
	}
}

void add_limits(acl::json& json, acl::json_node& root,
	const webcool::ai::sandbox_limits_t& limits)
{
	acl::json_node& value = json.create_node();
	root.add_child("limits", value);
	value.add_number("timeout_ms", static_cast<long long>(limits.timeout_ms));
	value.add_number("cpu_seconds", static_cast<long long>(limits.cpu_seconds));
	value.add_number("memory_bytes",
		static_cast<long long>(limits.memory_bytes));
	value.add_number("process_count",
		static_cast<long long>(limits.process_count));
	value.add_number("open_files", static_cast<long long>(limits.open_files));
	value.add_number("file_size_bytes",
		static_cast<long long>(limits.file_size_bytes));
	value.add_number("output_bytes",
		static_cast<long long>(limits.output_bytes));
}

} // namespace

bool AiWorkspaceListAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	std::string storage_scope = request_text(req, "storage_scope");
	if (storage_scope.empty()) storage_scope = "personal";
	std::string err;
	if (storage_scope != "personal" && storage_scope != "shared") {
		json_error(res, 400, "unsupported workspace directory scope",
			req.isKeepAlive());
		return true;
	}
	if (!external_project_allowed(req, storage_scope, err)) {
		json_error(res, err == "authentication required" ? 401 : 403,
			err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string workspace_root = user_root;
	if (storage_scope == "shared") {
		if (!ensure_shared_upload_dir(err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		workspace_root = runtime_upload_dir_get() + "/" + shared_folder_name();
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!workspace.list(request_text(req, "path"), entries, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("storage_scope", storage_scope.c_str());
	acl::json_node& items = json.create_array();
	root.add_child("entries", items);
	for (size_t i = 0; i < entries.size(); ++i) {
		acl::json_node& item = items.add_child(false, true);
		item.add_text("path", entries[i].path.c_str());
		item.add_bool("directory", entries[i].directory);
		item.add_number("size", entries[i].size);
		item.add_number("modified_at", entries[i].modified_at);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceReadAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	webcool::ai::agent_workspace_t workspace(user_root);
	std::string content;
	std::string err;
	bool truncated = false;
	const std::string path = request_text(req, "path");
	if (!workspace.read(path, content, truncated, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", path.c_str());
	root.add_text("content", content.c_str());
	root.add_bool("truncated", truncated);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceSearchAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	webcool::ai::agent_workspace_t workspace(user_root);
	std::vector<webcool::ai::workspace_match_t> matches;
	std::string err;
	bool truncated = false;
	if (!workspace.search(request_text(req, "path"), request_text(req, "q"),
		matches, truncated, err))
	{
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("truncated", truncated);
	acl::json_node& items = json.create_array();
	root.add_child("matches", items);
	for (size_t i = 0; i < matches.size(); ++i) {
		acl::json_node& item = items.add_child(false, true);
		item.add_text("path", matches[i].path.c_str());
		item.add_number("line", static_cast<long long>(matches[i].line));
		item.add_text("text", matches[i].text.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceProjectCreateAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string path = json_text((*body)["path"]);
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty()) storage_scope = "personal";
	const std::string requested_language = json_text((*body)["language"]);
	const std::string requested_platform = json_text((*body)["platform"]);
	// Defaults preserve compatibility with clients released before language and
	// target-platform selection was added.
	const std::string language = requested_language.empty()
		? "cpp" : requested_language;
	const std::string platform = requested_platform.empty()
		? "cross-platform" : requested_platform;
	if (!json_bool((*body)["confirm"], false)) {
		json_error(res, 400, "project workspace creation requires confirmation",
			req.isKeepAlive());
		return true;
	}

	std::string permission_err;
	if (!external_project_allowed(req, storage_scope, permission_err)) {
		json_error(res, permission_err == "authentication required" ? 401 : 403,
			permission_err.c_str(), req.isKeepAlive());
		return true;
	}

	// Select a bounded physical root. The durable project record still uses a
	// logical path, so downstream workspace and sandbox APIs never accept an
	// arbitrary absolute path from the browser.
	std::string workspace_root = user_root;
	std::string scaffold_path = path;
	std::string logical_path;
	if (storage_scope == "shared") {
		std::string shared_err;
		if (!ensure_shared_upload_dir(shared_err)) {
			json_error(res, 500, shared_err.c_str(), req.isKeepAlive());
			return true;
		}
		workspace_root = runtime_upload_dir_get() + "/" + shared_folder_name();
		const std::string prefix = std::string(shared_folder_name()) + "/";
		if (scaffold_path.compare(0, prefix.size(), prefix) == 0) {
			scaffold_path = scaffold_path.substr(prefix.size());
		}
	} else if (storage_scope == "local") {
		std::string name;
		if (!split_local_project_path(path, workspace_root, name, logical_path,
			permission_err))
		{
			json_error(res, 400, permission_err.c_str(), req.isKeepAlive());
			return true;
		}
		scaffold_path = name;
		bool directory_allowed = false;
		std::string locked_path;
		const std::string password = json_text((*body)["local_dir_password"]);
		if (!local_dir_lock_path_allows(runtime_upload_dir_get(), workspace_root,
			password, directory_allowed, locked_path, permission_err))
		{
			json_error(res, 500, permission_err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!directory_allowed) {
			json_error(res, 403, "local project parent directory is locked",
				req.isKeepAlive());
			return true;
		}
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	std::string normalized;
	std::string err;
	webcool::ai::project_scaffold_result_t scaffold;
	if (!webcool::ai::agent_workspace_t::normalize_path(scaffold_path, normalized,
		false, err) || !webcool::ai::create_project_scaffold(workspace,
			normalized, language, platform, scaffold, err))
	{
		const int status = err == "workspace path already exists"
			|| err == "workspace project directory is not empty" ? 409 : 400;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string physical_project = workspace_root + "/" + normalized;
	if (storage_scope == "shared") {
		logical_path = std::string(shared_folder_name()) + "/" + normalized;
	} else if (storage_scope == "personal") {
		logical_path = normalized;
	}
	if (storage_scope != "personal"
		&& !webcool::ai::agent_workspace_t::register_project_root(user_root,
			logical_path, physical_project, err))
	{
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}

	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", logical_path.c_str());
	root.add_text("storage_scope", storage_scope.c_str());
	root.add_text("workspace_base", storage_scope == "personal"
		? "virtual_disk_root" : (storage_scope == "shared"
			? "shared_virtual_disk" : "local_disk"));
	root.add_text("language", scaffold.language.c_str());
	root.add_text("platform", scaffold.platform.c_str());
	root.add_bool("reused_empty_directory", scaffold.reused_empty_directory);
	// Register the new workspace as a durable large-project manifest. Project
	// creation remains successful if metadata persistence is temporarily
	// unavailable; the UI can retry registration without recreating source files.
	const size_t slash = logical_path.rfind('/');
	const std::string title = slash == std::string::npos
		? logical_path : logical_path.substr(slash + 1);
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project_record;
	std::string project_err;
	const bool planning_available = project_store.create(title, logical_path,
		language, platform, project_record, project_err);
	root.add_bool("planning_available", planning_available);
	if (planning_available) {
		// A fresh scaffold receives a deterministic first plan. This gives users
		// visible module boundaries and a ready first task before any model call,
		// while remaining fully editable through versioned plan APIs.
		std::string goal;
		std::vector<webcool::ai::agent_project_module_t> modules;
		std::vector<webcool::ai::agent_project_task_t> tasks;
		webcool::ai::build_project_plan_template(logical_path, language, platform,
			goal, modules, tasks);
		webcool::ai::agent_project_record_t planned_record;
		const bool plan_seeded = project_record.plan_version != 0
			|| project_store.save_plan(project_record.id,
				project_record.plan_version, goal, modules, tasks, planned_record,
				project_err);
		if (plan_seeded && project_record.plan_version == 0) {
			project_record = planned_record;
		}
		root.add_text("project_id", project_record.id.c_str());
		root.add_number("plan_version", project_record.plan_version);
		root.add_bool("plan_seeded", plan_seeded);
		if (!plan_seeded) {
			webcool::ai::ai_log_error("http.ai.workspace",
				"seed-project-plan", project_err);
			root.add_text("planning_warning", project_err.c_str());
		}
	} else {
		webcool::ai::ai_log_error("http.ai.workspace",
			"register-project-manifest", project_err);
		root.add_text("planning_warning", project_err.c_str());
	}
	acl::json_node& files = json.create_array();
	root.add_child("files", files);
	for (size_t i = 0; i < scaffold.files.size(); ++i) {
		files.add_child(json.create_array_text(scaffold.files[i].c_str()));
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceDirectoryCreateAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string parent_input = json_text((*body)["parent_path"]);
	const std::string name = json_text((*body)["name"]);
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty()) storage_scope = "personal";
	// A child name is deliberately one component. Rejecting both separator
	// styles keeps the request identical on Windows, Linux and macOS.
	if (name.empty() || name.size() > 255 || name == "." || name == ".."
		|| name.find('/') != std::string::npos
		|| name.find('\\') != std::string::npos
		|| name.find_first_of("\r\n\t\0") != std::string::npos)
	{
		json_error(res, 400, "invalid child directory name", req.isKeepAlive());
		return true;
	}

	std::string err;
	if (storage_scope != "personal" && storage_scope != "shared") {
		json_error(res, 400, "unsupported workspace directory scope",
			req.isKeepAlive());
		return true;
	}
	if (!external_project_allowed(req, storage_scope, err)) {
		json_error(res, err == "authentication required" ? 401 : 403,
			err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string parent;
	if (!webcool::ai::agent_workspace_t::normalize_path(parent_input, parent,
		true, err))
	{
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string child = parent.empty() ? name : parent + "/" + name;
	std::string workspace_root = user_root;
	if (storage_scope == "shared") {
		if (!ensure_shared_upload_dir(err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		workspace_root = runtime_upload_dir_get() + "/" + shared_folder_name();
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	if (!workspace.create_directory_if_absent(child, err)) {
		const int status = err == "workspace path already exists" ? 409 : 400;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}

	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", child.c_str());
	root.add_text("parent_path", parent.c_str());
	root.add_text("storage_scope", storage_scope.c_str());
	root.add_text("workspace_base", storage_scope == "shared"
		? "shared_virtual_disk" : "virtual_disk_root");
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxCapabilitiesAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	webcool::ai::project_toolchain_catalog_t catalog(user_root);
	webcool::ai::project_toolchain_t toolchain;
	std::string err;
	const std::string path = request_text(req, "path");
	if (!catalog.discover(path, toolchain, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string backend_reason;
	const bool backend_available =
		webcool::ai::program_sandbox_t::backend_available(backend_reason);
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", path.c_str());
	root.add_bool("backend_available", backend_available);
	root.add_bool("execution_enabled", backend_available);
	root.add_bool("browser_acceptance_enabled", toolchain.browser_acceptance_enabled);
	root.add_text("browser_unavailable_reason", toolchain.browser_unavailable_reason.c_str());
	root.add_bool("requires_confirmation", true);
	if (!backend_reason.empty()) {
		root.add_text("backend_reason", backend_reason.c_str());
	}
	acl::json_node& languages = json.create_array();
	root.add_child("detected_languages", languages);
	for (size_t i = 0; i < toolchain.detected_languages.size(); ++i) {
		languages.add_child(json.create_array_text(
			toolchain.detected_languages[i].c_str()));
	}
	acl::json_node& commands = json.create_array();
	root.add_child("commands", commands);
	for (size_t i = 0; i < toolchain.commands.size(); ++i) {
		acl::json_node& item = commands.add_child(false, true);
		item.add_text("id", toolchain.commands[i].id.c_str());
		item.add_bool("dynamic_arguments", false);
		item.add_bool("requires_confirmation", true);
		acl::json_node& arguments = json.create_array();
		item.add_child("fixed_arguments", arguments);
		for (size_t j = 0;
			j < toolchain.commands[i].fixed_arguments.size(); ++j)
		{
			arguments.add_child(json.create_array_text(
				toolchain.commands[i].fixed_arguments[j].c_str()));
		}
	}
	acl::json_node& unavailable = json.create_array();
	root.add_child("unavailable_tools", unavailable);
	for (size_t i = 0; i < toolchain.unavailable_tools.size(); ++i) {
		unavailable.add_child(json.create_array_text(
			toolchain.unavailable_tools[i].c_str()));
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxPlanAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	std::string backend_reason;
	if (!webcool::ai::program_sandbox_t::backend_available(backend_reason)) {
		json_error(res, 503, backend_reason.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	webcool::ai::sandbox_execution_store_t store(user_root);
	webcool::ai::sandbox_execution_plan_t plan;
	std::string err;
	if (!store.create(json_text((*body)["path"]),
		json_text((*body)["command_id"]), plan, err))
	{
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("plan_id", plan.id.c_str());
	root.add_text("path", plan.project_path.c_str());
	root.add_text("command_id", plan.command.id.c_str());
	root.add_text("executable", plan.command.executable.c_str());
	root.add_bool("requires_confirmation", true);
	root.add_number("expires_in_seconds",
		webcool::ai::sandbox_execution_store_t::lifetime_seconds());
	acl::json_node& arguments = json.create_array();
	root.add_child("fixed_arguments", arguments);
	for (size_t i = 0; i < plan.command.fixed_arguments.size(); ++i) {
		arguments.add_child(json.create_array_text(
			plan.command.fixed_arguments[i].c_str()));
	}
	add_limits(json, root, plan.limits);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxExecuteAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	if (!json_bool((*body)["confirm"], false)) {
		json_error(res, 400, "explicit sandbox confirmation is required",
			req.isKeepAlive());
		return true;
	}
	const std::string plan_id = json_text((*body)["plan_id"]);
	std::shared_ptr<sandbox_runtime_task_t> task(new sandbox_runtime_task_t());
	task->id = plan_id;
	task->user_root = user_root;
	std::string err;
	// Reserve capacity before consuming the one-time authorization. A busy
	// server therefore returns 429 without destroying a still-valid plan.
	if (!reserve_sandbox_runtime(task, err)) {
		json_error(res, 429, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::sandbox_execution_store_t store(user_root);
	webcool::ai::sandbox_execution_plan_t plan;
	if (!store.consume(plan_id, plan, err)) {
		remove_sandbox_runtime(user_root, plan_id);
		int status = 400;
		if (err == "sandbox execution plan not found") status = 404;
		else if (err == "sandbox execution plan expired") status = 410;
		else if (err == "sandbox execution plan was already consumed") status = 409;
		else if (err == "sandbox toolchain changed after plan preview") status = 409;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (!store.audit_started(plan, err)) {
		remove_sandbox_runtime(user_root, plan_id);
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	{
		std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
		task->plan = plan;
	}
	// program_sandbox_t supervises native child pipes with poll(). Running that
	// loop inside an ACL fiber makes ACL replace poll() with its descriptor
	// registry and abort on freshly spawned pipe descriptors. A detached native
	// worker is appropriate here: task lifetime is shared, cancellation is
	// atomic, and all published state remains protected by the runtime mutex.
	std::thread([task] { run_async_sandbox_task(task); }).detach();
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("started", true);
	root.add_bool("running", true);
	root.add_text("run_id", plan.id.c_str());
	root.add_text("plan_id", plan.id.c_str());
	root.add_text("path", plan.project_path.c_str());
	root.add_text("command_id", plan.command.id.c_str());
	root.add_text("status", "queued");
	return sendJson(res, 202, root, req.isKeepAlive());
}

bool AiSandboxRunStatusAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	sandbox_runtime_snapshot_t snapshot;
	if (!snapshot_sandbox_runtime(user_root, request_text(req, "id"), snapshot)) {
		json_error(res, 404, "sandbox run not found", req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", snapshot.id.c_str());
	root.add_text("path", snapshot.project_path.c_str());
	root.add_text("command_id", snapshot.command_id.c_str());
	root.add_text("status", snapshot.status.c_str());
	root.add_bool("done", snapshot.done);
	root.add_bool("cancel_requested", snapshot.cancel_requested);
	root.add_number("started_at", snapshot.started_at);
	root.add_number("finished_at", snapshot.finished_at);
	if (snapshot.done) {
		root.add_bool("executed", snapshot.result.started);
		root.add_bool("cancelled", snapshot.result.cancelled);
		root.add_bool("timed_out", snapshot.result.timed_out);
		root.add_bool("memory_limit_exceeded",
			snapshot.result.memory_limit_exceeded);
		root.add_bool("process_limit_exceeded",
			snapshot.result.process_limit_exceeded);
		root.add_bool("output_truncated", snapshot.result.output_truncated);
		root.add_number("exit_code", snapshot.result.exit_code);
		root.add_number("signal", snapshot.result.signal);
		root.add_number("elapsed_ms",
			static_cast<long long>(snapshot.result.elapsed_ms));
		root.add_text("stdout", snapshot.result.standard_output.c_str());
		root.add_text("stderr", snapshot.result.standard_error.c_str());
		std::vector<webcool::ai::project_diagnostic_t> diagnostics;
		const std::string absolute_project = snapshot.project_path.empty()
			? user_root : user_root + "/" + snapshot.project_path;
		webcool::ai::parse_project_diagnostics(
			snapshot.result.standard_error + "\n" + snapshot.result.standard_output,
			absolute_project, snapshot.project_path, diagnostics);
		acl::json_node& diagnostic_items = json.create_array();
		root.add_child("diagnostics", diagnostic_items);
		long long error_count = 0;
		long long warning_count = 0;
		for (size_t i = 0; i < diagnostics.size(); ++i) {
			acl::json_node& item = diagnostic_items.add_child(false, true);
			item.add_text("path", diagnostics[i].path.c_str());
			item.add_number("line", diagnostics[i].line);
			item.add_number("column", diagnostics[i].column);
			item.add_text("severity", diagnostics[i].severity.c_str());
			item.add_text("message", diagnostics[i].message.c_str());
			if (diagnostics[i].severity == "error") ++error_count;
			else if (diagnostics[i].severity == "warning") ++warning_count;
		}
		root.add_number("diagnostic_count",
			static_cast<long long>(diagnostics.size()));
		root.add_number("diagnostic_error_count", error_count);
		root.add_number("diagnostic_warning_count", warning_count);
		if (!snapshot.result.error.empty()) {
			root.add_text("error", snapshot.result.error.c_str());
		}
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxRunHistoryAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	size_t limit = 20;
	const char* raw_limit = req.getParameter("limit");
	if (raw_limit != NULL && *raw_limit != '\0') {
		char* end = NULL;
		const long parsed = strtol(raw_limit, &end, 10);
		if (end == raw_limit || *end != '\0' || parsed < 1 || parsed > 100) {
			json_error(res, 400, "limit must be between 1 and 100",
				req.isKeepAlive());
			return true;
		}
		limit = static_cast<size_t>(parsed);
	}
	webcool::ai::sandbox_execution_store_t store(user_root);
	std::vector<webcool::ai::sandbox_run_history_t> runs;
	std::string err;
	if (!store.list_history(limit, runs, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("metadata_only", true);
	acl::json_node& items = json.create_array();
	root.add_child("runs", items);
	for (size_t i = 0; i < runs.size(); ++i) {
		// An unfinished persistent record normally means a service restart. If the
		// same ID is still in this process, report its live phase instead.
		sandbox_runtime_snapshot_t live;
		const bool has_live = snapshot_sandbox_runtime(user_root, runs[i].id, live);
		acl::json_node& item = items.add_child(false, true);
		item.add_text("run_id", runs[i].id.c_str());
		item.add_text("path", runs[i].project_path.c_str());
		item.add_text("command_id", runs[i].command_id.c_str());
		item.add_text("status",
			has_live ? live.status.c_str() : runs[i].status.c_str());
		item.add_number("started_at", runs[i].started_at);
		item.add_number("finished_at", runs[i].finished_at);
		item.add_number("exit_code", runs[i].exit_code);
		item.add_number("signal", runs[i].signal);
		item.add_number("elapsed_ms",
			static_cast<long long>(runs[i].elapsed_ms));
		item.add_bool("cancelled", runs[i].cancelled);
		item.add_bool("timed_out", runs[i].timed_out);
		item.add_bool("output_truncated", runs[i].output_truncated);
		if (!runs[i].error.empty()) item.add_text("error", runs[i].error.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxRunCancelAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["run_id"]);
	bool already_done = false;
	if (!cancel_sandbox_runtime(user_root, id, already_done)) {
		json_error(res, 404, "sandbox run not found", req.isKeepAlive());
		return true;
	}
	if (already_done) {
		json_error(res, 409, "sandbox run is already finished", req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("cancel_requested", true);
	root.add_text("status", "cancelling");
	return sendJson(res, 202, root, req.isKeepAlive());
}

bool AiWorkspacePatchPreviewAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(6 * 1024 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string path = json_text((*body)["path"]);
	const std::string content = json_text((*body)["content"]);
	webcool::ai::workspace_patch_store_t store(user_root);
	webcool::ai::workspace_patch_preview_t preview;
	std::string err;
	if (!store.create(path, content, preview, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("patch_id", preview.id.c_str());
	root.add_text("path", preview.path.c_str());
	root.add_text("original_sha256", preview.original_sha256.c_str());
	root.add_text("proposed_sha256", preview.proposed_sha256.c_str());
	root.add_number("added_lines", preview.added_lines);
	root.add_number("removed_lines", preview.removed_lines);
	root.add_text("diff", preview.diff.c_str());
	root.add_number("expires_in_seconds", 30 * 60);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspacePatchApplyAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string patch_id = json_text((*body)["patch_id"]);
	webcool::ai::workspace_patch_store_t store(user_root);
	webcool::ai::workspace_patch_result_t result;
	std::string err;
	if (!store.apply(patch_id, result, err)) {
		int status = 400;
		if (err == "workspace file changed after patch preview") status = 409;
		else if (err == "workspace patch plan expired") status = 410;
		else if (err == "workspace patch plan not found") status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("patch_id", result.id.c_str());
	root.add_text("path", result.path.c_str());
	root.add_text("sha256", result.sha256.c_str());
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceChangeSetPreviewAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(3 * 1024 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	acl::json_node* items = json_array((*body)["changes"]);
	if (items == NULL) {
		json_error(res, 400, "changes must be an array", req.isKeepAlive());
		return true;
	}
	std::vector<webcool::ai::workspace_change_input_t> changes;
	for (acl::json_node* item = items->first_child(); item != NULL
		&& changes.size() <= webcool::ai::kMaxAgentChanges; item = items->next_child())
	{
		acl::json_node* object = item->is_object() ? item : item->get_obj();
		if (object == NULL || !object->is_object()) continue;
		webcool::ai::workspace_change_input_t change;
		change.operation = json_text((*object)["operation"]);
		change.path = json_text((*object)["path"]);
		change.target_path = json_text((*object)["target_path"]);
		change.content = json_text((*object)["content"]);
		change.reason = json_text((*object)["reason"]);
		change.enforce_expected_current = json_bool(
			(*object)["enforce_expected_current"], false);
		change.expected_current_absent = json_bool(
			(*object)["expected_current_absent"], false);
		change.expected_current_content = json_text(
			(*object)["expected_current_content"]);
		changes.push_back(change);
	}
	webcool::ai::workspace_change_set_store_t store(user_root);
	webcool::ai::workspace_change_set_preview_t preview;
	std::string err;
	if (!store.create(changes, preview, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("change_set_id", preview.id.c_str());
	root.add_bool("requires_confirmation", true);
	root.add_number("expires_in_seconds",
		webcool::ai::workspace_change_set_store_t::lifetime_seconds());
	acl::json_node& output = json.create_array();
	root.add_child("changes", output);
	for (size_t i = 0; i < preview.items.size(); ++i) {
		acl::json_node& item = output.add_child(false, true);
		item.add_text("operation", preview.items[i].operation.c_str());
		item.add_text("path", preview.items[i].path.c_str());
		if (!preview.items[i].target_path.empty()) {
			item.add_text("target_path", preview.items[i].target_path.c_str());
		}
		item.add_text("reason", preview.items[i].reason.c_str());
		item.add_bool("creates_file", preview.items[i].creates_file);
		item.add_bool("deletes_file", preview.items[i].deletes_file);
		item.add_bool("creates_directory", preview.items[i].creates_directory);
		item.add_text("original_sha256",
			preview.items[i].original_sha256.c_str());
		item.add_text("proposed_sha256",
			preview.items[i].proposed_sha256.c_str());
		item.add_number("added_lines", preview.items[i].added_lines);
		item.add_number("removed_lines", preview.items[i].removed_lines);
		item.add_text("diff", preview.items[i].diff.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceChangeSetApplyAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_workspace(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	if (!json_bool((*body)["confirm"], false)) {
		json_error(res, 400, "explicit change-set confirmation is required",
			req.isKeepAlive());
		return true;
	}
	webcool::ai::workspace_change_set_store_t store(user_root);
	std::vector<webcool::ai::workspace_change_result_t> results;
	std::string err;
	if (!store.apply(json_text((*body)["change_set_id"]), results, err)) {
		int status = 409;
		if (err == "workspace change set expired") status = 410;
		else if (err == "invalid workspace change-set id") status = 400;
		else if (err.find("rollback failed") != std::string::npos) status = 500;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("applied", true);
	acl::json_node& output = json.create_array();
	root.add_child("changes", output);
	for (size_t i = 0; i < results.size(); ++i) {
		acl::json_node& item = output.add_child(false, true);
		item.add_text("operation", results[i].operation.c_str());
		item.add_text("path", results[i].path.c_str());
		if (!results[i].target_path.empty()) {
			item.add_text("target_path", results[i].target_path.c_str());
		}
		item.add_text("sha256", results[i].sha256.c_str());
		item.add_bool("created", results[i].created);
		item.add_bool("deleted", results[i].deleted);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action

#undef json_error
