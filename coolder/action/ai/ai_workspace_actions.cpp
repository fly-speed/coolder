#include "stdafx.h"
#include "ai_workspace_actions_internal.h"
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
    bool keep_alive, const char *file, int line, const char *function)
{
	webcool::ai::ai_log_error("http.ai.workspace", "response",
	    message ? message : "unspecified workspace action error");
	action::json_error_at(
	    res, status, message, keep_alive, file, line, function);
}

#define json_error(res, status, message, keep_alive)                        \
	ai_workspace_json_error(res, status, message, keep_alive, __FILE__, \
	    __LINE__, __FUNCTION__)

bool current_workspace(request_t &req, response_t &res, std::string &user_root)
{
	const std::string upload_root = runtime_upload_dir_get();
	std::string username;
	bool admin = false;
	if (!auth_current_user(req, upload_root, username, admin)) {
		auth_send_required(req, res);
		return false;
	}
	std::string err;
	if (authenticated_user_upload_dir(req, upload_root, user_root, err))
		return true;
	json_error(res, err == "authentication required" ? 401 : 500,
	    err.c_str(), req.isKeepAlive());
	return false;
}

std::string request_text(request_t &req, const char *name)
{
	const char *value = req.getParameter(name);
	return value ? value : "";
}

std::string json_text(acl::json_node *node)
{
	if (node == NULL)
		return "";
	const char *value = node->get_string();
	if (!(value == NULL))
		return value ? value : "";
	value = node->get_text();
	return value ? value : "";
}

bool json_bool(acl::json_node *node, bool fallback)
{
	if (node == NULL)
		return fallback;
	const std::string value = json_text(node);
	if (value == "true" || value == "1")
		return true;
	if (!(value == "false" || value == "0"))
		return fallback;
	return false;
}

bool split_local_project_path(const std::string &input, std::string &parent,
    std::string &name, std::string &logical, std::string &err)
{
	std::string raw = input;
	while (raw.size() > 1 &&
	    (raw[raw.size() - 1] == '/' || raw[raw.size() - 1] == '\\'))
		raw.resize(raw.size() - 1);
#ifdef _WIN32
	const bool absolute = (raw.size() >= 3 && raw[1] == ':' &&
	                          (raw[2] == '/' || raw[2] == '\\')) ||
	    (raw.size() >= 2 && raw[0] == '\\' && raw[1] == '\\');
#else
	const bool absolute = !raw.empty() && raw[0] == '/';
#endif
	const size_t slash = raw.find_last_of("/\\");
	if (!absolute || slash == std::string::npos ||
	    slash + 1 >= raw.size()) {
		err = "local project path must be an absolute directory path";
		return false;
	}
	parent = raw.substr(0, slash);
	if (parent.empty())
		parent = "/";
#ifdef _WIN32
	if (parent.size() == 2 && parent[1] == ':')
		parent += "/";
#endif
	name = raw.substr(slash + 1);
	if (name == "." || name == "..") {
		err = "invalid local project directory name";
		return false;
	}
	std::string normalized;
	if (!webcool::ai::agent_workspace_t::normalize_path(
	        raw, normalized, false, err))
		return false;
	logical = std::string("本地磁盘/") + normalized;
	return true;
}

acl::json_node *json_array(acl::json_node *node)
{
	if (node == NULL)
		return NULL;
	if (node->is_array())
		return node;
	acl::json_node *value = node->get_obj();
	return value != NULL && value->is_array() ? value : NULL;
}

// Live command results are transient just like model replies. Persistent audit
// remains metadata-only; stdout/stderr disappear on expiry or service restart.

}
using namespace workspace_action_detail;
// namespace

bool AiWorkspaceListAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	std::string storage_scope = request_text(req, "storage_scope");
	if (storage_scope.empty())
		storage_scope = "personal";
	std::string err;
	if (storage_scope != "personal" && storage_scope != "shared") {
		json_error(res, 400, "unsupported workspace directory scope",
		    req.isKeepAlive());
		return true;
	}
	if (!agent_detail::project_location_allowed(req, storage_scope, err)) {
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
		workspace_root =
		    runtime_upload_dir_get() + "/" + shared_folder_name();
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!workspace.list(request_text(req, "path"), entries, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("storage_scope", storage_scope.c_str());
	acl::json_node &items = json.create_array();
	root.add_child("entries", items);
	for (size_t i = 0; i < entries.size(); ++i) {
		acl::json_node &item = items.add_child(false, true);
		item.add_text("path", entries[i].path.c_str());
		item.add_bool("directory", entries[i].directory);
		item.add_number("size", entries[i].size);
		item.add_number("modified_at", entries[i].modified_at);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceReadAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
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
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", path.c_str());
	root.add_text("content", content.c_str());
	root.add_bool("truncated", truncated);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceSearchAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	webcool::ai::agent_workspace_t workspace(user_root);
	std::vector<webcool::ai::workspace_match_t> matches;
	std::string err;
	bool truncated = false;
	if (!workspace.search(request_text(req, "path"), request_text(req, "q"),
	        matches, truncated, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("truncated", truncated);
	acl::json_node &items = json.create_array();
	root.add_child("matches", items);
	for (size_t i = 0; i < matches.size(); ++i) {
		acl::json_node &item = items.add_child(false, true);
		item.add_text("path", matches[i].path.c_str());
		item.add_number(
		    "line", static_cast<long long>(matches[i].line));
		item.add_text("text", matches[i].text.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiProjectDirectoriesAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	std::string err;
	if (!agent_detail::project_location_allowed(req, "local", err)) {
		json_error(res, 403, err.c_str(), req.isKeepAlive());
		return true;
	}
	namespace fs = std::filesystem;
	const std::string input = request_text(req, "path");
	if (!input.empty() &&
	    (!agent_detail::absolute_project_path(input) ||
	        input.find('\0') != std::string::npos)) {
		json_error(res, 400, "directory path must be absolute",
		    req.isKeepAlive());
		return true;
	}
	std::error_code ec;
	const fs::path path =
	    fs::canonical(fs::u8path(input.empty() ? user_root : input), ec);
	if (ec || !fs::is_directory(path, ec)) {
		json_error(res, 400,
		    "directory does not exist or is inaccessible",
		    req.isKeepAlive());
		return true;
	}
	fs::directory_iterator it(path, ec), end;
	if (ec) {
		json_error(res, 403, "cannot browse this directory",
		    req.isKeepAlive());
		return true;
	}
	std::vector<std::string> names;
	size_t inspected = 0;
	for (; it != end && inspected < 10000; it.increment(ec), ++inspected) {
		if (ec)
			break;
		std::error_code entry_error;
		if (!(it->is_directory(entry_error) && !entry_error))
			continue;
		names.push_back(it->path().filename().u8string());
	}
	if (ec) {
		json_error(res, 403, "cannot browse this directory",
		    req.isKeepAlive());
		return true;
	}
	std::sort(names.begin(), names.end());
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", path.generic_u8string().c_str());
	root.add_text("parent", path.parent_path().generic_u8string().c_str());
	root.add_bool("truncated", it != end);
	acl::json_node &entries = json.create_array();
	root.add_child("entries", entries);
	for (const auto &name : names) {
		acl::json_node &entry = entries.add_child(false, true);
		entry.add_text("name", name.c_str());
		entry.add_text("path",
		    (path / fs::u8path(name)).generic_u8string().c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceProjectCreateAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string path = json_text((*body)["path"]);
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty())
		storage_scope = agent_detail::absolute_project_path(path) ?
		    "local" :
		    "personal";
	const std::string requested_language = json_text((*body)["language"]);
	const std::string requested_platform = json_text((*body)["platform"]);
	// Defaults preserve compatibility with clients released before language and
	// target-platform selection was added.
	const std::string language =
	    requested_language.empty() ? "cpp" : requested_language;
	const std::string platform =
	    requested_platform.empty() ? "cross-platform" : requested_platform;
	if (!json_bool((*body)["confirm"], false)) {
		json_error(res, 400,
		    "project workspace creation requires confirmation",
		    req.isKeepAlive());
		return true;
	}

	std::string permission_err;
	if (!agent_detail::project_location_allowed(
	        req, storage_scope, permission_err)) {
		json_error(res,
		    permission_err == "authentication required" ? 401 : 403,
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
			json_error(
			    res, 500, shared_err.c_str(), req.isKeepAlive());
			return true;
		}
		workspace_root =
		    runtime_upload_dir_get() + "/" + shared_folder_name();
		const std::string prefix =
		    std::string(shared_folder_name()) + "/";
		if (scaffold_path.compare(0, prefix.size(), prefix) == 0) {
			scaffold_path = scaffold_path.substr(prefix.size());
		}
	} else if (storage_scope == "local") {
		std::string name;
		if (!split_local_project_path(path, workspace_root, name,
		        logical_path, permission_err)) {
			json_error(res, 400, permission_err.c_str(),
			    req.isKeepAlive());
			return true;
		}
		scaffold_path = name;
		bool directory_allowed = false;
		std::string locked_path;
		const std::string password =
		    json_text((*body)["local_dir_password"]);
		if (!local_dir_lock_path_allows(runtime_upload_dir_get(),
		        workspace_root, password, directory_allowed,
		        locked_path, permission_err)) {
			json_error(res, 500, permission_err.c_str(),
			    req.isKeepAlive());
			return true;
		}
		if (!directory_allowed) {
			json_error(res, 403,
			    "local project parent directory is locked",
			    req.isKeepAlive());
			return true;
		}
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	std::string normalized;
	std::string err;
	webcool::ai::project_scaffold_result_t scaffold;
	if (!webcool::ai::agent_workspace_t::normalize_path(
	        scaffold_path, normalized, false, err) ||
	    !webcool::ai::create_project_scaffold(
	        workspace, normalized, language, platform, scaffold, err)) {
		const int status = err == "workspace path already exists" ||
		        err == "workspace project directory is not empty" ?
		    409 :
		    400;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string physical_project = workspace_root + "/" + normalized;
	if (storage_scope == "shared") {
		logical_path =
		    std::string(shared_folder_name()) + "/" + normalized;
	} else if (storage_scope == "personal") {
		logical_path = normalized;
	}
	if (storage_scope != "personal" &&
	    !webcool::ai::agent_workspace_t::register_project_root(
	        user_root, logical_path, physical_project, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}

	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", logical_path.c_str());
	root.add_text("storage_scope", storage_scope.c_str());
	root.add_text("workspace_base",
	    storage_scope == "personal" ?
	        "virtual_disk_root" :
	        (storage_scope == "shared" ? "shared_virtual_disk" :
	                                     "local_disk"));
	root.add_text("language", scaffold.language.c_str());
	root.add_text("platform", scaffold.platform.c_str());
	root.add_bool(
	    "reused_empty_directory", scaffold.reused_empty_directory);
	// Register the new workspace as a durable large-project manifest. Project
	// creation remains successful if metadata persistence is temporarily
	// unavailable; the UI can retry registration without recreating source files.
	const size_t slash = logical_path.rfind('/');
	const std::string title = slash == std::string::npos ?
	    logical_path :
	    logical_path.substr(slash + 1);
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project_record;
	std::string project_err;
	const bool planning_available = project_store.create(title,
	    logical_path, language, platform, project_record, project_err);
	root.add_bool("planning_available", planning_available);
	if (planning_available) {
		// A fresh scaffold receives a deterministic first plan. This gives users
		// visible module boundaries and a ready first task before any model call,
		// while remaining fully editable through versioned plan APIs.
		std::string goal;
		std::vector<webcool::ai::agent_project_module_t> modules;
		std::vector<webcool::ai::agent_project_task_t> tasks;
		webcool::ai::build_project_plan_template(
		    logical_path, language, platform, goal, modules, tasks);
		webcool::ai::agent_project_record_t planned_record;
		const bool plan_seeded = project_record.plan_version != 0 ||
		    project_store.save_plan(project_record.id,
		        project_record.plan_version, goal, modules, tasks,
		        planned_record, project_err);
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
	acl::json_node &files = json.create_array();
	root.add_child("files", files);
	for (size_t i = 0; i < scaffold.files.size(); ++i) {
		files.add_child(
		    json.create_array_text(scaffold.files[i].c_str()));
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceDirectoryCreateAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string parent_input = json_text((*body)["parent_path"]);
	const std::string name = json_text((*body)["name"]);
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty())
		storage_scope = "personal";
	// A child name is deliberately one component. Rejecting both separator
	// styles keeps the request identical on Windows, Linux and macOS.
	if (name.empty() || name.size() > 255 || name == "." || name == ".." ||
	    name.find('/') != std::string::npos ||
	    name.find('\\') != std::string::npos ||
	    name.find_first_of("\r\n\t\0") != std::string::npos) {
		json_error(res, 400, "invalid child directory name",
		    req.isKeepAlive());
		return true;
	}

	std::string err;
	if (storage_scope != "personal" && storage_scope != "shared") {
		json_error(res, 400, "unsupported workspace directory scope",
		    req.isKeepAlive());
		return true;
	}
	if (!agent_detail::project_location_allowed(req, storage_scope, err)) {
		json_error(res, err == "authentication required" ? 401 : 403,
		    err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string parent;
	if (!webcool::ai::agent_workspace_t::normalize_path(
	        parent_input, parent, true, err)) {
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
		workspace_root =
		    runtime_upload_dir_get() + "/" + shared_folder_name();
	}
	webcool::ai::agent_workspace_t workspace(workspace_root);
	if (!workspace.create_directory_if_absent(child, err)) {
		const int status =
		    err == "workspace path already exists" ? 409 : 400;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}

	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", child.c_str());
	root.add_text("parent_path", parent.c_str());
	root.add_text("storage_scope", storage_scope.c_str());
	root.add_text("workspace_base",
	    storage_scope == "shared" ? "shared_virtual_disk" :
	                                "virtual_disk_root");
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspacePatchPreviewAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(6 * 1024 * 1024);
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
	acl::json_node &root = json.create_node();
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

bool AiWorkspacePatchApplyAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
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
		if (err == "workspace file changed after patch preview")
			status = 409;
		else if (err == "workspace patch plan expired")
			status = 410;
		else if (err == "workspace patch plan not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("patch_id", result.id.c_str());
	root.add_text("path", result.path.c_str());
	root.add_text("sha256", result.sha256.c_str());
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceChangeSetPreviewAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(3 * 1024 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	acl::json_node *items = json_array((*body)["changes"]);
	if (items == NULL) {
		json_error(
		    res, 400, "changes must be an array", req.isKeepAlive());
		return true;
	}
	std::vector<webcool::ai::workspace_change_input_t> changes;
	for (acl::json_node *item = items->first_child();
	     item != NULL && changes.size() <= webcool::ai::kMaxAgentChanges;
	     item = items->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL || !object->is_object())
			continue;
		webcool::ai::workspace_change_input_t change;
		change.operation = json_text((*object)["operation"]);
		change.path = json_text((*object)["path"]);
		change.target_path = json_text((*object)["target_path"]);
		change.content = json_text((*object)["content"]);
		change.reason = json_text((*object)["reason"]);
		change.enforce_expected_current =
		    json_bool((*object)["enforce_expected_current"], false);
		change.expected_current_absent =
		    json_bool((*object)["expected_current_absent"], false);
		change.expected_current_content =
		    json_text((*object)["expected_current_content"]);
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
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("change_set_id", preview.id.c_str());
	root.add_bool("requires_confirmation", true);
	root.add_number("expires_in_seconds",
	    webcool::ai::workspace_change_set_store_t::lifetime_seconds());
	acl::json_node &output = json.create_array();
	root.add_child("changes", output);
	for (size_t i = 0; i < preview.items.size(); ++i) {
		acl::json_node &item = output.add_child(false, true);
		item.add_text("operation", preview.items[i].operation.c_str());
		item.add_text("path", preview.items[i].path.c_str());
		if (!preview.items[i].target_path.empty()) {
			item.add_text("target_path",
			    preview.items[i].target_path.c_str());
		}
		item.add_text("reason", preview.items[i].reason.c_str());
		item.add_bool("creates_file", preview.items[i].creates_file);
		item.add_bool("deletes_file", preview.items[i].deletes_file);
		item.add_bool(
		    "creates_directory", preview.items[i].creates_directory);
		item.add_text("original_sha256",
		    preview.items[i].original_sha256.c_str());
		item.add_text("proposed_sha256",
		    preview.items[i].proposed_sha256.c_str());
		item.add_number("added_lines", preview.items[i].added_lines);
		item.add_number(
		    "removed_lines", preview.items[i].removed_lines);
		item.add_text("diff", preview.items[i].diff.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiWorkspaceChangeSetApplyAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	if (!json_bool((*body)["confirm"], false)) {
		json_error(res, 400,
		    "explicit change-set confirmation is required",
		    req.isKeepAlive());
		return true;
	}
	webcool::ai::workspace_change_set_store_t store(user_root);
	std::vector<webcool::ai::workspace_change_result_t> results;
	std::string err;
	if (!store.apply(json_text((*body)["change_set_id"]), results, err)) {
		int status = 409;
		if (err == "workspace change set expired")
			status = 410;
		else if (err == "invalid workspace change-set id")
			status = 400;
		else if (err.find("rollback failed") != std::string::npos)
			status = 500;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("applied", true);
	acl::json_node &output = json.create_array();
	root.add_child("changes", output);
	for (size_t i = 0; i < results.size(); ++i) {
		acl::json_node &item = output.add_child(false, true);
		item.add_text("operation", results[i].operation.c_str());
		item.add_text("path", results[i].path.c_str());
		if (!results[i].target_path.empty()) {
			item.add_text(
			    "target_path", results[i].target_path.c_str());
		}
		item.add_text("sha256", results[i].sha256.c_str());
		item.add_bool("created", results[i].created);
		item.add_bool("deleted", results[i].deleted);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action

#undef json_error
