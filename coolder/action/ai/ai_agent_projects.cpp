#include "stdafx.h"
// Project registration, plans, workflows and indexes HTTP actions.
#include "ai_agent_actions_internal.h"

namespace action
{
using namespace agent_detail;

namespace
{

void add_string_array_json(acl::json_node &parent, const char *name,
			   const std::vector<std::string> &values)
{
	acl::json_node &array = parent.get_json().create_array();
	parent.add_child(name, array);
	for (size_t i = 0; i < values.size(); ++i) {
		array.add_child(
			parent.get_json().create_array_text(values[i].c_str()));
	}
}

void add_project_json(acl::json_node &item,
		      const webcool::ai::agent_project_record_t &project,
		      bool include_plan)
{
	item.add_text("project_id", project.id.c_str());
	item.add_text("title", project.title.c_str());
	item.add_text("path", project.project_path.c_str());
	item.add_text("language", project.language.c_str());
	item.add_text("platform", project.platform.c_str());
	item.add_text("status", project.status.c_str());
	item.add_number("created_at", project.created_at);
	item.add_number("updated_at", project.updated_at);
	item.add_number("plan_version", project.plan_version);
	item.add_number("module_count",
			static_cast<long long>(project.modules.size()));
	item.add_number("task_count",
			static_cast<long long>(project.tasks.size()));
	long long completed = 0;
	for (size_t i = 0; i < project.tasks.size(); ++i) {
		if (project.tasks[i].status == "completed")
			++completed;
	}
	item.add_number("completed_task_count", completed);
	if (!include_plan)
		return;
	item.add_text("goal", project.goal.c_str());
	acl::json_node &modules = item.get_json().create_array();
	item.add_child("modules", modules);
	for (size_t i = 0; i < project.modules.size(); ++i) {
		acl::json_node &module = modules.add_child(false, true);
		module.add_text("id", project.modules[i].id.c_str());
		module.add_text("name", project.modules[i].name.c_str());
		module.add_text("layer", project.modules[i].layer.c_str());
		module.add_text("path", project.modules[i].path.c_str());
		add_string_array_json(module, "dependencies",
				      project.modules[i].dependencies);
	}
	acl::json_node &tasks = item.get_json().create_array();
	item.add_child("tasks", tasks);
	for (size_t i = 0; i < project.tasks.size(); ++i) {
		acl::json_node &task = tasks.add_child(false, true);
		task.add_text("id", project.tasks[i].id.c_str());
		task.add_text("title", project.tasks[i].title.c_str());
		task.add_text("module_id", project.tasks[i].module_id.c_str());
		task.add_text("status", project.tasks[i].status.c_str());
		task.add_bool("ready",
			      webcool::ai::agent_project_store_t::task_ready(
				      project, project.tasks[i]));
		add_string_array_json(task, "dependencies",
				      project.tasks[i].dependencies);
		add_string_array_json(task, "acceptance_criteria",
				      project.tasks[i].acceptance_criteria);
		add_string_array_json(task, "test_plan",
				      project.tasks[i].test_plan);
	}
}

void add_project_index_json(
	acl::json_node &item,
	const webcool::ai::agent_project_index_snapshot_t &snapshot)
{
	item.add_text("project_id", snapshot.project_id.c_str());
	item.add_text("path", snapshot.project_path.c_str());
	item.add_number("revision", snapshot.revision);
	item.add_number("indexed_at", snapshot.indexed_at);
	item.add_number("file_count",
			static_cast<long long>(snapshot.files.size()));
	item.add_number("directory_count", snapshot.directory_count);
	item.add_number("skipped_directory_count",
			snapshot.skipped_directory_count);
	item.add_number("changed_file_count", snapshot.changed_file_count);
	item.add_bool("truncated", snapshot.truncated);
	acl::json_node &files = item.get_json().create_array();
	item.add_child("files", files);
	// Keep browser responses bounded even when the durable index contains the
	// full 20k-file project map. The model uses a separate token-bounded summary.
	const size_t count = std::min<size_t>(snapshot.files.size(), 500);
	for (size_t i = 0; i < count; ++i) {
		const webcool::ai::agent_project_index_entry_t &indexed =
			snapshot.files[i];
		acl::json_node &file = files.add_child(false, true);
		file.add_text("path", indexed.path.c_str());
		file.add_text("module_id", indexed.module_id.c_str());
		file.add_text("kind", indexed.kind.c_str());
		file.add_text("language", indexed.language.c_str());
		file.add_number("size", indexed.size);
		file.add_number("modified_at", indexed.modified_at);
		file.add_number("symbol_count",
				static_cast<long long>(indexed.symbols.size()));
		acl::json_node &symbols = file.get_json().create_array();
		file.add_child("symbols", symbols);
		const size_t symbol_count =
			std::min<size_t>(indexed.symbols.size(), 32);
		for (size_t symbol_index = 0; symbol_index < symbol_count;
		     ++symbol_index) {
			acl::json_node &symbol = symbols.add_child(false, true);
			symbol.add_number(
				"line",
				static_cast<long long>(
					indexed.symbols[symbol_index].line));
			symbol.add_text(
				"kind",
				indexed.symbols[symbol_index].kind.c_str());
			symbol.add_text(
				"text",
				indexed.symbols[symbol_index].text.c_str());
		}
		file.add_bool("symbols_truncated",
			      indexed.symbols.size() > symbol_count);
	}
	item.add_bool("entries_truncated", snapshot.files.size() > count);
}

int project_error_status(const std::string &err)
{
	if (err == "agent project not found" ||
	    err == "agent project task not found") {
		return 404;
	}
	if (err == "agent project plan version conflict")
		return 409;
	if (err.find("cannot ") == 0 ||
	    err.find("database") != std::string::npos) {
		return 500;
	}
	return 400;
}

std::string project_title_from_path(const std::string &path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

bool AiAgentProjectListAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	webcool::ai::agent_project_store_t store(user_root);
	const std::string id =
		req.getParameter("id") ? req.getParameter("id") : "";
	const std::string path =
		req.getParameter("path") ? req.getParameter("path") : "";
	std::string err;
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	std::string location_err;
	root.add_bool("shared_project_location_allowed",
		      project_location_allowed(req, "shared", location_err));
	location_err.clear();
	root.add_bool("local_project_location_allowed",
		      project_location_allowed(req, "local", location_err));
	if (!id.empty() || !path.empty()) {
		webcool::ai::agent_project_record_t project;
		const bool found =
			!id.empty() ? store.get(id, project, err) :
				      store.find_by_path(path, project, err);
		if (!found) {
			json_error(res, project_error_status(err), err.c_str(),
				   req.isKeepAlive());
			return true;
		}
		add_project_json(root, project, true);
		return sendJson(res, 200, root, req.isKeepAlive());
	}
	std::vector<webcool::ai::agent_project_record_t> projects;
	if (!store.list(0, projects, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	acl::json_node &items = json.create_array();
	root.add_child("projects", items);
	for (size_t i = 0; i < projects.size(); ++i) {
		acl::json_node &item = items.add_child(false, true);
		add_project_json(item, projects[i], false);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectDeleteAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(32 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string project_id = json_text((*body)["project_id"]);
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project;
	std::string err;
	if (!project_store.get(project_id, project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}

	// Delete only bounded AI metadata. No workspace/file API is called here,
	// which makes retaining the user's virtual-disk project an invariant.
	webcool::ai::agent_session_store_t session_store(user_root);
	size_t removed_sessions = 0;
	if (!session_store.remove_for_project(project.project_path,
					      removed_sessions, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_project_index_store_t index_store(user_root);
	if (!index_store.remove(project.id, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	size_t removed_workflows = 0;
	webcool::ai::agent_workflow_store_t workflow_store(user_root);
	if (!workflow_store.remove_for_project(project.id, removed_workflows,
					       err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_project_record_t removed;
	if (!project_store.remove(project.id, removed, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	if (removed.project_path.compare(0, strlen("共享目录/"), "共享目录/") ==
		    0 ||
	    removed.project_path.compare(0, strlen("本地磁盘/"), "本地磁盘/") ==
		    0) {
		std::string unregister_err;
		if (!webcool::ai::agent_workspace_t::unregister_project_root(
			    user_root, removed.project_path, unregister_err)) {
			webcool::ai::ai_log_error("http.ai.project",
						  "unregister-external-root",
						  unregister_err);
		}
	}

	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("project_id", removed.id.c_str());
	root.add_text("path", removed.project_path.c_str());
	root.add_number("removed_session_count",
			static_cast<long long>(removed_sessions));
	root.add_number("removed_workflow_count",
			static_cast<long long>(removed_workflows));
	root.add_bool("workspace_retained", true);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectImportGitAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty())
		storage_scope =
			absolute_project_path(json_text((*body)["path"])) ?
				"local" :
				"personal";
	std::string permission_err;
	if (!project_location_allowed(req, storage_scope, permission_err)) {
		json_error(res,
			   permission_err == "authentication required" ? 401 :
									 403,
			   permission_err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string requested_path = json_text((*body)["path"]);
	std::string project_path;
	std::string physical_path;
	std::string err;
	if (storage_scope == "local") {
		if (!local_project_location(requested_path, project_path,
					    physical_path, err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
	} else {
		std::string scoped_path = requested_path;
		if (storage_scope == "shared") {
			const std::string prefix =
				std::string(shared_folder_name()) + "/";
			if (scoped_path.compare(0, prefix.size(), prefix) ==
			    0) {
				scoped_path = scoped_path.substr(prefix.size());
			}
		}
		std::string normalized;
		if (!webcool::ai::agent_workspace_t::normalize_path(
			    scoped_path, normalized, false, err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
		project_path = storage_scope == "shared" ?
				       std::string(shared_folder_name()) + "/" +
					       normalized :
				       normalized;
		physical_path =
			storage_scope == "shared" ?
				runtime_upload_dir_get() + "/" + project_path :
				user_root + "/" + project_path;
	}
	if (storage_scope != "personal" &&
	    !safe_git_project_directory(physical_path)) {
		json_error(
			res, 400,
			"selected project directory is not a safe Git repository",
			req.isKeepAlive());
		return true;
	}
	if (storage_scope == "local") {
		bool directory_allowed = false;
		std::string locked_path;
		const std::string password =
			json_text((*body)["local_dir_password"]);
		if (!local_dir_lock_path_allows(
			    runtime_upload_dir_get(), physical_path, password,
			    directory_allowed, locked_path, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!directory_allowed) {
			json_error(res, 403,
				   "local project directory is locked",
				   req.isKeepAlive());
			return true;
		}
	}
	if (storage_scope != "personal" &&
	    !webcool::ai::agent_workspace_t::register_project_root(
		    user_root, project_path, physical_path, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}

	// Discovery is read-only and treats .git as a protected marker. Git config,
	// hooks, credentials and object contents are never returned to the browser.
	webcool::ai::project_toolchain_catalog_t catalog(user_root);
	webcool::ai::project_toolchain_t toolchain;
	bool git_repository = false;
	if (!catalog.is_git_repository(project_path, git_repository, err) ||
	    !catalog.discover(project_path, toolchain, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (!git_repository) {
		json_error(
			res, 400,
			"selected project directory is not a safe Git repository",
			req.isKeepAlive());
		return true;
	}

	std::string language = toolchain.detected_languages.empty() ?
				       json_text((*body)["language"]) :
				       toolchain.detected_languages[0];
	if (language.empty())
		language = "unknown";
	std::string platform = json_text((*body)["platform"]);
	if (platform.empty())
		platform = "cross-platform";
	std::string title = json_text((*body)["title"]);
	if (title.empty())
		title = project_title_from_path(project_path);

	webcool::ai::agent_project_store_t store(user_root);
	webcool::ai::agent_project_record_t project;
	if (!store.create(title, project_path, language, platform, project,
			  err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	if (project.plan_version == 0) {
		std::string goal;
		std::vector<webcool::ai::agent_project_module_t> modules;
		std::vector<webcool::ai::agent_project_task_t> tasks;
		webcool::ai::build_project_plan_template(
			project_path, language, platform, goal, modules, tasks);
		webcool::ai::agent_project_record_t planned;
		if (store.save_plan(project.id, project.plan_version, goal,
				    modules, tasks, planned, err)) {
			project = planned;
		} else {
			webcool::ai::ai_log_error("http.ai.project",
						  "seed-import-plan", err);
		}
	}

	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("git_repository", true);
	root.add_bool("workspace_modified", false);
	root.add_text("storage_scope", storage_scope.c_str());
	add_project_json(root, project, true);
	add_string_array_json(root, "detected_languages",
			      toolchain.detected_languages);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectEnsureAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string requested_path = json_text((*body)["path"]);
	std::string storage_scope = json_text((*body)["storage_scope"]);
	if (storage_scope.empty())
		storage_scope = absolute_project_path(requested_path) ?
					"local" :
					"personal";
	std::string path;
	std::string physical;
	std::string err;
	if (!project_location_allowed(req, storage_scope, err)) {
		json_error(res, 403, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (storage_scope == "local") {
		if (!local_project_location(requested_path, path, physical,
					    err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
	} else if (!webcool::ai::agent_workspace_t::normalize_path(
			   requested_path, path, false, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string language = json_text((*body)["language"]);
	const std::string platform = json_text((*body)["platform"]);
	if ((!language.empty() &&
	     !webcool::ai::supported_project_language(language)) ||
	    (!platform.empty() &&
	     !webcool::ai::supported_project_platform(platform)) ||
	    (!language.empty() && !platform.empty() &&
	     !webcool::ai::project_language_supports_platform(language,
							      platform))) {
		json_error(res, 400, "unsupported project language or platform",
			   req.isKeepAlive());
		return true;
	}
	// Registration never creates or modifies source files. Require the selected
	// workspace directory to exist and pass the same symlink/reparse checks used
	// by model tools.
	if (storage_scope == "local" &&
	    !webcool::ai::agent_workspace_t::register_project_root(
		    user_root, path, physical, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_workspace_t workspace(user_root);
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!workspace.list(path, entries, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string title = json_text((*body)["title"]);
	if (title.empty())
		title = project_title_from_path(path);
	webcool::ai::agent_project_store_t store(user_root);
	webcool::ai::agent_project_record_t project;
	if (!store.create(title, path, language, platform, project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_project_json(root, project, true);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectPlanSaveAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(512 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	acl::json_node *module_items = json_array_node((*body)["modules"]);
	acl::json_node *task_items = json_array_node((*body)["tasks"]);
	if (module_items == NULL || task_items == NULL) {
		json_error(res, 400, "modules and tasks must be arrays",
			   req.isKeepAlive());
		return true;
	}
	std::string err;
	std::vector<webcool::ai::agent_project_module_t> modules;
	for (acl::json_node *item = module_items->first_child(); item != NULL;
	     item = module_items->next_child()) {
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		if (object == NULL || modules.size() >= 64) {
			json_error(res, 400, "invalid project module array",
				   req.isKeepAlive());
			return true;
		}
		webcool::ai::agent_project_module_t module;
		module.id = json_text((*object)["id"]);
		module.name = json_text((*object)["name"]);
		module.layer = json_text((*object)["layer"]);
		if (!webcool::ai::agent_workspace_t::normalize_path(
			    json_text((*object)["path"]), module.path, false,
			    err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!parse_string_array((*object)["dependencies"], 32,
					module.dependencies)) {
			json_error(res, 400,
				   "invalid project module dependencies",
				   req.isKeepAlive());
			return true;
		}
		modules.push_back(module);
	}
	std::vector<webcool::ai::agent_project_task_t> tasks;
	for (acl::json_node *item = task_items->first_child(); item != NULL;
	     item = task_items->next_child()) {
		acl::json_node *object =
			item->is_object() ? item : item->get_obj();
		if (object == NULL || tasks.size() >= 500) {
			json_error(res, 400, "invalid project task array",
				   req.isKeepAlive());
			return true;
		}
		webcool::ai::agent_project_task_t task;
		task.id = json_text((*object)["id"]);
		task.title = json_text((*object)["title"]);
		task.module_id = json_text((*object)["module_id"]);
		task.status = "pending";
		if (!parse_string_array((*object)["dependencies"], 64,
					task.dependencies) ||
		    !parse_string_array((*object)["acceptance_criteria"], 16,
					task.acceptance_criteria) ||
		    !parse_string_array((*object)["test_plan"], 16,
					task.test_plan)) {
			json_error(res, 400,
				   "invalid project task detail array",
				   req.isKeepAlive());
			return true;
		}
		tasks.push_back(task);
	}
	webcool::ai::agent_project_store_t store(user_root);
	webcool::ai::agent_project_record_t project;
	if (!store.save_plan(json_text((*body)["project_id"]),
			     json_number((*body)["plan_version"], -1),
			     json_text((*body)["goal"]), modules, tasks,
			     project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_project_json(root, project, true);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectPlanProposeAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_project_store_t store(user_root);
	webcool::ai::agent_project_record_t project;
	std::string err;
	if (!store.get(json_text((*body)["project_id"]), project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	std::string goal;
	std::vector<webcool::ai::agent_project_module_t> modules;
	std::vector<webcool::ai::agent_project_task_t> tasks;
	if (!webcool::ai::build_project_plan_proposal(
		    project.project_path, project.language, project.platform,
		    json_text((*body)["goal"]), json_text((*body)["scale"]),
		    goal, modules, tasks, err) ||
	    !webcool::ai::agent_project_store_t::validate_plan(modules, tasks,
							       err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	// Preserve stable task state in the preview so users can see exactly what a
	// subsequent versioned save will retain. Newly generated IDs stay pending.
	for (size_t i = 0; i < tasks.size(); ++i) {
		for (size_t j = 0; j < project.tasks.size(); ++j) {
			if (tasks[i].id == project.tasks[j].id) {
				tasks[i].status = project.tasks[j].status;
				break;
			}
		}
	}
	webcool::ai::agent_project_record_t proposal = project;
	proposal.goal = goal;
	proposal.modules = modules;
	proposal.tasks = tasks;
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("proposal", true);
	root.add_text("scale", json_text((*body)["scale"]).c_str());
	add_project_json(root, proposal, true);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectTaskStatusAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_project_store_t store(user_root);
	webcool::ai::agent_project_record_t project;
	std::string err;
	if (!store.update_task_status(
		    json_text((*body)["project_id"]),
		    json_text((*body)["task_id"]), json_text((*body)["status"]),
		    json_number((*body)["plan_version"], -1), project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_project_json(root, project, true);
	return sendJson(res, 200, root, req.isKeepAlive());
}

namespace
{

bool validate_workflow_owner(const std::string &user_root,
			     const std::string &project_id,
			     const std::string &session_id,
			     const std::string &task_id, std::string &err)
{
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project;
	if (!project_store.get(project_id, project, err))
		return false;
	bool task_found = false;
	for (size_t i = 0; i < project.tasks.size(); ++i) {
		if (project.tasks[i].id == task_id) {
			task_found = true;
			break;
		}
	}
	if (!task_found) {
		err = "agent project task not found";
		return false;
	}
	webcool::ai::agent_session_store_t session_store(user_root);
	webcool::ai::agent_session_record_t session;
	if (!session_store.get(session_id, session, err))
		return false;
	if (session.project_path != project.project_path) {
		err = "agent workflow session does not belong to this project";
		return false;
	}
	return true;
}

void add_workflow_json(
	acl::json_node &root,
	const webcool::ai::agent_workflow_checkpoint_t &checkpoint)
{
	root.add_text("project_id", checkpoint.project_id.c_str());
	root.add_text("session_id", checkpoint.session_id.c_str());
	root.add_text("task_id", checkpoint.task_id.c_str());
	root.add_text("failed_command_id",
		      checkpoint.failed_command_id.c_str());
	root.add_number("failed_exit_code", checkpoint.failed_exit_code);
	root.add_number("updated_at", checkpoint.updated_at);
	acl::json_node &queue = root.get_json().create_array();
	root.add_child("queue", queue);
	for (size_t i = 0; i < checkpoint.command_queue.size(); ++i) {
		queue.add_child(checkpoint.command_queue[i].c_str(), true);
	}
	acl::json_node &diagnostics = root.get_json().create_array();
	root.add_child("diagnostics", diagnostics);
	for (size_t i = 0; i < checkpoint.diagnostics.size(); ++i) {
		acl::json_node &item = diagnostics.add_child(false, true);
		item.add_text("path", checkpoint.diagnostics[i].path.c_str());
		item.add_number("line", checkpoint.diagnostics[i].line);
		item.add_number("column", checkpoint.diagnostics[i].column);
		item.add_text("severity",
			      checkpoint.diagnostics[i].severity.c_str());
	}
}

} // namespace

bool AiAgentProjectWorkflowGetAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	const std::string project_id = req.getParameter("project_id") ?
					       req.getParameter("project_id") :
					       "";
	const std::string session_id = req.getParameter("session_id") ?
					       req.getParameter("session_id") :
					       "";
	const std::string task_id =
		req.getParameter("task_id") ? req.getParameter("task_id") : "";
	std::string err;
	if (!validate_workflow_owner(user_root, project_id, session_id, task_id,
				     err)) {
		json_error(res,
			   err.find("not found") != std::string::npos ? 404 :
									400,
			   err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_workflow_store_t store(user_root);
	webcool::ai::agent_workflow_checkpoint_t checkpoint;
	bool found = false;
	if (!store.load(project_id, session_id, task_id, checkpoint, found,
			err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("found", found);
	if (found)
		add_workflow_json(root, checkpoint);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectWorkflowSaveAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(128 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_workflow_checkpoint_t checkpoint;
	checkpoint.project_id = json_text((*body)["project_id"]);
	checkpoint.session_id = json_text((*body)["session_id"]);
	checkpoint.task_id = json_text((*body)["task_id"]);
	std::string err;
	if (!validate_workflow_owner(user_root, checkpoint.project_id,
				     checkpoint.session_id, checkpoint.task_id,
				     err)) {
		json_error(res,
			   err.find("not found") != std::string::npos ? 404 :
									400,
			   err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_workflow_store_t store(user_root);
	const bool remove = json_bool((*body)["remove"], false);
	if (remove) {
		if (!store.remove(checkpoint.project_id, checkpoint.session_id,
				  checkpoint.task_id, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
	} else {
		checkpoint.failed_command_id =
			json_text((*body)["failed_command_id"]);
		checkpoint.failed_exit_code =
			json_number((*body)["failed_exit_code"], 0);
		checkpoint.updated_at = static_cast<long long>(time(NULL));
		if (!parse_string_array((*body)["queue"], 32,
					checkpoint.command_queue)) {
			json_error(res, 400, "invalid workflow command queue",
				   req.isKeepAlive());
			return true;
		}
		acl::json_node *diagnostic_items =
			json_array_node((*body)["diagnostics"]);
		for (acl::json_node *item =
			     diagnostic_items ?
				     diagnostic_items->first_child() :
				     NULL;
		     item != NULL && checkpoint.diagnostics.size() < 100;
		     item = diagnostic_items->next_child()) {
			acl::json_node *object =
				item->is_object() ? item : item->get_obj();
			if (object == NULL) {
				json_error(res, 400,
					   "invalid workflow diagnostic",
					   req.isKeepAlive());
				return true;
			}
			webcool::ai::agent_workflow_diagnostic_t diagnostic;
			diagnostic.path = json_text((*object)["path"]);
			diagnostic.line = json_number((*object)["line"], 0);
			diagnostic.column = json_number((*object)["column"], 0);
			diagnostic.severity = json_text((*object)["severity"]);
			if (diagnostic.severity.empty())
				diagnostic.severity = "note";
			checkpoint.diagnostics.push_back(diagnostic);
		}
		if (!store.save(checkpoint, err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("removed", remove);
	if (!remove)
		add_workflow_json(root, checkpoint);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentProjectIndexGetAction::run(request_t &req, response_t &res)
{
	operation_trace_t trace("project_index_get");
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	const std::string project_id =
		req.getParameter("id") ? req.getParameter("id") : "";
	trace.phase("load_project");
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project;
	std::string err;
	if (!project_store.get(project_id, project, err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	trace.project_id(project.id);
	trace.phase("index_load");
	webcool::ai::agent_project_index_store_t index_store(user_root);
	webcool::ai::agent_project_index_snapshot_t snapshot;
	if (!index_store.load(project, snapshot, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	trace.phase("serialize_index");
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_project_index_json(root, snapshot);
	trace.phase("send_response");
	const bool sent = sendJson(res, 200, root, req.isKeepAlive());
	if (sent)
		trace.complete();
	return sent;
}

bool AiAgentProjectIndexRefreshAction::run(request_t &req, response_t &res)
{
	operation_trace_t trace("project_index_refresh");
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	trace.phase("load_project");
	webcool::ai::agent_project_store_t project_store(user_root);
	webcool::ai::agent_project_record_t project;
	std::string err;
	if (!project_store.get(json_text((*body)["project_id"]), project,
			       err)) {
		json_error(res, project_error_status(err), err.c_str(),
			   req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_workspace_t workspace(user_root);
	trace.project_id(project.id);
	trace.phase("index_load");
	webcool::ai::agent_project_index_store_t index_store(user_root);
	webcool::ai::agent_project_index_snapshot_t snapshot;
	if (!index_store.refresh(
		    project, workspace, snapshot, err,
		    [&trace](const char *phase) { trace.phase(phase); })) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	trace.phase("serialize_index");
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_project_index_json(root, snapshot);
	trace.phase("send_response");
	const bool sent = sendJson(res, 200, root, req.isKeepAlive());
	if (sent)
		trace.complete();
	return sent;
}

} // namespace action
