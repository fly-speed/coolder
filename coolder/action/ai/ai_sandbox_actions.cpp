#include "stdafx.h"
#include "ai_workspace_actions_internal.h"
namespace action
{
namespace workspace_action_detail
{
webcool::mutex g_sandbox_runtime_mutex;
std::map<std::string, std::shared_ptr<sandbox_runtime_task_t>>
    g_sandbox_runtime_tasks;
const size_t kMaxActiveSandboxRuns = 4;
const size_t kMaxActiveSandboxRunsPerUser = 1;
const size_t kMaxCompletedSandboxResults = 20;
const long long kSandboxResultLifetimeSeconds = 60 * 60;

std::string sandbox_runtime_key(
    const std::string &user_root, const std::string &id)
{
	return user_root + "\n" + id;
}

void cleanup_sandbox_runtime_locked(long long now)
{
	for (std::map<std::string,
	         std::shared_ptr<sandbox_runtime_task_t>>::iterator it =
	         g_sandbox_runtime_tasks.begin();
	     it != g_sandbox_runtime_tasks.end();) {
		if (it->second->done && it->second->finished_at > 0 &&
		    now - it->second->finished_at >
		        kSandboxResultLifetimeSeconds) {
			it = g_sandbox_runtime_tasks.erase(it);
		} else
			++it;
	}
	for (;;) {
		size_t completed = 0;
		std::map<std::string,
		    std::shared_ptr<sandbox_runtime_task_t>>::iterator oldest =
		    g_sandbox_runtime_tasks.end();
		for (std::map<std::string,
		         std::shared_ptr<sandbox_runtime_task_t>>::iterator it =
		         g_sandbox_runtime_tasks.begin();
		     it != g_sandbox_runtime_tasks.end(); ++it) {
			if (!it->second->done)
				continue;
			++completed;
			if (!(oldest == g_sandbox_runtime_tasks.end() ||
			        it->second->finished_at <
			            oldest->second->finished_at))
				continue;
			oldest = it;
		}
		if (completed <= kMaxCompletedSandboxResults ||
		    oldest == g_sandbox_runtime_tasks.end())
			break;
		g_sandbox_runtime_tasks.erase(oldest);
	}
}

bool reserve_sandbox_runtime(
    const std::shared_ptr<sandbox_runtime_task_t> &task, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	cleanup_sandbox_runtime_locked(static_cast<long long>(time(NULL)));
	size_t active = 0;
	size_t user_active = 0;
	for (std::map<std::string,
	         std::shared_ptr<sandbox_runtime_task_t>>::iterator it =
	         g_sandbox_runtime_tasks.begin();
	     it != g_sandbox_runtime_tasks.end(); ++it) {
		if (it->second->done)
			continue;
		++active;
		if (!(it->second->user_root == task->user_root))
			continue;
		++user_active;
	}
	if (active >= kMaxActiveSandboxRuns ||
	    user_active >= kMaxActiveSandboxRunsPerUser) {
		err = "sandbox execution concurrency limit reached";
		return false;
	}
	const std::string key = sandbox_runtime_key(task->user_root, task->id);
	if (g_sandbox_runtime_tasks.find(key) !=
	    g_sandbox_runtime_tasks.end()) {
		err = "sandbox execution plan is already running";
		return false;
	}
	g_sandbox_runtime_tasks[key] = task;
	return true;
}

void remove_sandbox_runtime(const std::string &user_root, const std::string &id)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	g_sandbox_runtime_tasks.erase(sandbox_runtime_key(user_root, id));
}

bool snapshot_sandbox_runtime(const std::string &user_root,
    const std::string &id, sandbox_runtime_snapshot_t &snapshot)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	cleanup_sandbox_runtime_locked(static_cast<long long>(time(NULL)));
	const std::map<std::string,
	    std::shared_ptr<sandbox_runtime_task_t>>::const_iterator it =
	    g_sandbox_runtime_tasks.find(sandbox_runtime_key(user_root, id));
	if (it == g_sandbox_runtime_tasks.end())
		return false;
	const std::shared_ptr<sandbox_runtime_task_t> &task = it->second;
	snapshot.id = task->id;
	snapshot.project_path = task->plan.project_path;
	snapshot.command_id = task->plan.command.id;
	snapshot.status = task->status;
	snapshot.cancel_requested =
	    task->cancel_requested.load(std::memory_order_relaxed);
	snapshot.done = task->done;
	snapshot.started_at = task->started_at;
	snapshot.finished_at = task->finished_at;
	snapshot.result = task->result;
	return true;
}

bool cancel_sandbox_runtime(
    const std::string &user_root, const std::string &id, bool &already_done)
{
	std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
	const std::map<std::string,
	    std::shared_ptr<sandbox_runtime_task_t>>::iterator it =
	    g_sandbox_runtime_tasks.find(sandbox_runtime_key(user_root, id));
	if (it == g_sandbox_runtime_tasks.end())
		return false;
	already_done = it->second->done;
	if (already_done)
		return true;
	it->second->cancel_requested.store(true, std::memory_order_relaxed);
	it->second->status = "cancelling";

	return true;
}

void run_async_sandbox_task(const std::shared_ptr<sandbox_runtime_task_t> &task)
{
	{
		std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
		task->status = "running";
		task->started_at = static_cast<long long>(time(NULL));
	}
	webcool::ai::sandbox_result_t result;
	bool dispatched = false;
	try {
		std::vector<webcool::ai::sandbox_command_t> commands(
		    1, task->plan.command);
		webcool::ai::program_sandbox_t sandbox(task->user_root,
		    task->plan.project_path, commands, task->plan.limits);
		webcool::ai::sandbox_request_t request;
		request.command_id = task->plan.command.id;
		dispatched =
		    sandbox.execute(request, result, &task->cancel_requested);
	} catch (const std::exception &exception) {
		result.error = exception.what();
		webcool::ai::ai_log_error(
		    "sandbox.runtime", "exception", result.error);
	} catch (...) {
		result.error = "unexpected sandbox runtime exception";
		webcool::ai::ai_log_error(
		    "sandbox.runtime", "unknown-exception", result.error);
	}
	webcool::ai::sandbox_execution_store_t store(task->user_root);
	std::string audit_error;
	(void)store.audit_finished(task->plan, result, audit_error);
	{
		std::lock_guard<webcool::mutex> guard(g_sandbox_runtime_mutex);
		task->result = result;
		task->done = true;
		task->finished_at = static_cast<long long>(time(NULL));
		if (result.cancelled)
			task->status = "cancelled";
		else if (!dispatched || result.timed_out ||
		    result.output_truncated || !result.error.empty())
			task->status = "failed";
		else
			task->status = "completed";
	}
}

void add_limits(acl::json &json, acl::json_node &root,
    const webcool::ai::sandbox_limits_t &limits)
{
	acl::json_node &value = json.create_node();
	root.add_child("limits", value);
	value.add_number(
	    "timeout_ms", static_cast<long long>(limits.timeout_ms));
	value.add_number(
	    "cpu_seconds", static_cast<long long>(limits.cpu_seconds));
	value.add_number(
	    "memory_bytes", static_cast<long long>(limits.memory_bytes));
	value.add_number(
	    "process_count", static_cast<long long>(limits.process_count));
	value.add_number(
	    "open_files", static_cast<long long>(limits.open_files));
	value.add_number(
	    "file_size_bytes", static_cast<long long>(limits.file_size_bytes));
	value.add_number(
	    "output_bytes", static_cast<long long>(limits.output_bytes));
}

}
using namespace workspace_action_detail;
bool AiSandboxCapabilitiesAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
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
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", path.c_str());
	root.add_bool("backend_available", backend_available);
	root.add_bool("execution_enabled", backend_available);
	root.add_bool(
	    "browser_acceptance_enabled", toolchain.browser_acceptance_enabled);
	root.add_text("browser_unavailable_reason",
	    toolchain.browser_unavailable_reason.c_str());
	root.add_bool("requires_confirmation", true);
	if (!backend_reason.empty()) {
		root.add_text("backend_reason", backend_reason.c_str());
	}
	acl::json_node &languages = json.create_array();
	root.add_child("detected_languages", languages);
	for (size_t i = 0; i < toolchain.detected_languages.size(); ++i) {
		languages.add_child(json.create_array_text(
		    toolchain.detected_languages[i].c_str()));
	}
	acl::json_node &commands = json.create_array();
	root.add_child("commands", commands);
	for (size_t i = 0; i < toolchain.commands.size(); ++i) {
		acl::json_node &item = commands.add_child(false, true);
		item.add_text("id", toolchain.commands[i].id.c_str());
		item.add_bool("dynamic_arguments", false);
		item.add_bool("requires_confirmation", true);
		acl::json_node &arguments = json.create_array();
		item.add_child("fixed_arguments", arguments);
		for (size_t j = 0;
		     j < toolchain.commands[i].fixed_arguments.size(); ++j) {
			arguments.add_child(json.create_array_text(
			    toolchain.commands[i].fixed_arguments[j].c_str()));
		}
	}
	acl::json_node &unavailable = json.create_array();
	root.add_child("unavailable_tools", unavailable);
	for (size_t i = 0; i < toolchain.unavailable_tools.size(); ++i) {
		unavailable.add_child(json.create_array_text(
		    toolchain.unavailable_tools[i].c_str()));
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxPlanAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	std::string backend_reason;
	if (!webcool::ai::program_sandbox_t::backend_available(
	        backend_reason)) {
		json_error(res, 503, backend_reason.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	webcool::ai::sandbox_execution_store_t store(user_root);
	webcool::ai::sandbox_execution_plan_t plan;
	std::string err;
	if (!store.create(json_text((*body)["path"]),
	        json_text((*body)["command_id"]), plan, err)) {
		json_error(res, 400, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("plan_id", plan.id.c_str());
	root.add_text("path", plan.project_path.c_str());
	root.add_text("command_id", plan.command.id.c_str());
	root.add_text("executable", plan.command.executable.c_str());
	root.add_bool("requires_confirmation", true);
	root.add_number("expires_in_seconds",
	    webcool::ai::sandbox_execution_store_t::lifetime_seconds());
	acl::json_node &arguments = json.create_array();
	root.add_child("fixed_arguments", arguments);
	for (size_t i = 0; i < plan.command.fixed_arguments.size(); ++i) {
		arguments.add_child(json.create_array_text(
		    plan.command.fixed_arguments[i].c_str()));
	}
	add_limits(json, root, plan.limits);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxExecuteAction::run(request_t &req, response_t &res)
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
		    "explicit sandbox confirmation is required",
		    req.isKeepAlive());
		return true;
	}
	const std::string plan_id = json_text((*body)["plan_id"]);
	std::shared_ptr<sandbox_runtime_task_t> task(
	    new sandbox_runtime_task_t());
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
		if (err == "sandbox execution plan not found")
			status = 404;
		else if (err == "sandbox execution plan expired")
			status = 410;
		else if (err == "sandbox execution plan was already consumed")
			status = 409;
		else if (err == "sandbox toolchain changed after plan preview")
			status = 409;
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
	acl::json_node &root = json.create_node();
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

bool AiSandboxRunStatusAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	sandbox_runtime_snapshot_t snapshot;
	if (!snapshot_sandbox_runtime(
	        user_root, request_text(req, "id"), snapshot)) {
		json_error(
		    res, 404, "sandbox run not found", req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", snapshot.id.c_str());
	root.add_text("path", snapshot.project_path.c_str());
	root.add_text("command_id", snapshot.command_id.c_str());
	root.add_text("status", snapshot.status.c_str());
	root.add_bool("done", snapshot.done);
	root.add_bool("cancel_requested", snapshot.cancel_requested);
	root.add_number("started_at", snapshot.started_at);
	root.add_number("finished_at", snapshot.finished_at);
	if (!snapshot.done)
		return sendJson(res, 200, root, req.isKeepAlive());
	root.add_bool("executed", snapshot.result.started);
	root.add_bool("cancelled", snapshot.result.cancelled);
	root.add_bool("timed_out", snapshot.result.timed_out);
	root.add_bool(
	    "memory_limit_exceeded", snapshot.result.memory_limit_exceeded);
	root.add_bool(
	    "process_limit_exceeded", snapshot.result.process_limit_exceeded);
	root.add_bool("output_truncated", snapshot.result.output_truncated);
	root.add_number("exit_code", snapshot.result.exit_code);
	root.add_number("signal", snapshot.result.signal);
	root.add_number(
	    "elapsed_ms", static_cast<long long>(snapshot.result.elapsed_ms));
	root.add_text("stdout", snapshot.result.standard_output.c_str());
	root.add_text("stderr", snapshot.result.standard_error.c_str());
	std::vector<webcool::ai::project_diagnostic_t> diagnostics;
	const std::string absolute_project = snapshot.project_path.empty() ?
	    user_root :
	    user_root + "/" + snapshot.project_path;
	webcool::ai::parse_project_diagnostics(snapshot.result.standard_error +
	        "\n" + snapshot.result.standard_output,
	    absolute_project, snapshot.project_path, diagnostics);
	acl::json_node &diagnostic_items = json.create_array();
	root.add_child("diagnostics", diagnostic_items);
	long long error_count = 0;
	long long warning_count = 0;
	for (size_t i = 0; i < diagnostics.size(); ++i) {
		acl::json_node &item = diagnostic_items.add_child(false, true);
		item.add_text("path", diagnostics[i].path.c_str());
		item.add_number("line", diagnostics[i].line);
		item.add_number("column", diagnostics[i].column);
		item.add_text("severity", diagnostics[i].severity.c_str());
		item.add_text("message", diagnostics[i].message.c_str());
		if (diagnostics[i].severity == "error")
			++error_count;
		else if (diagnostics[i].severity == "warning")
			++warning_count;
	}
	root.add_number(
	    "diagnostic_count", static_cast<long long>(diagnostics.size()));
	root.add_number("diagnostic_error_count", error_count);
	root.add_number("diagnostic_warning_count", warning_count);
	if (snapshot.result.error.empty())
		return sendJson(res, 200, root, req.isKeepAlive());
	root.add_text("error", snapshot.result.error.c_str());

	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxRunHistoryAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	size_t limit = 20;
	const char *raw_limit = req.getParameter("limit");
	if (raw_limit != NULL && *raw_limit != '\0') {
		char *end = NULL;
		const long parsed = strtol(raw_limit, &end, 10);
		if (end == raw_limit || *end != '\0' || parsed < 1 ||
		    parsed > 100) {
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
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("metadata_only", true);
	acl::json_node &items = json.create_array();
	root.add_child("runs", items);
	for (size_t i = 0; i < runs.size(); ++i) {
		// An unfinished persistent record normally means a service restart. If the
		// same ID is still in this process, report its live phase instead.
		sandbox_runtime_snapshot_t live;
		const bool has_live =
		    snapshot_sandbox_runtime(user_root, runs[i].id, live);
		acl::json_node &item = items.add_child(false, true);
		item.add_text("run_id", runs[i].id.c_str());
		item.add_text("path", runs[i].project_path.c_str());
		item.add_text("command_id", runs[i].command_id.c_str());
		item.add_text("status",
		    has_live ? live.status.c_str() : runs[i].status.c_str());
		item.add_number("started_at", runs[i].started_at);
		item.add_number("finished_at", runs[i].finished_at);
		item.add_number("exit_code", runs[i].exit_code);
		item.add_number("signal", runs[i].signal);
		item.add_number(
		    "elapsed_ms", static_cast<long long>(runs[i].elapsed_ms));
		item.add_bool("cancelled", runs[i].cancelled);
		item.add_bool("timed_out", runs[i].timed_out);
		item.add_bool("output_truncated", runs[i].output_truncated);
		if (runs[i].error.empty())
			continue;
		item.add_text("error", runs[i].error.c_str());
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiSandboxRunCancelAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_workspace(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["run_id"]);
	bool already_done = false;
	if (!cancel_sandbox_runtime(user_root, id, already_done)) {
		json_error(
		    res, 404, "sandbox run not found", req.isKeepAlive());
		return true;
	}
	if (already_done) {
		json_error(res, 409, "sandbox run is already finished",
		    req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("cancel_requested", true);
	root.add_text("status", "cancelling");
	return sendJson(res, 202, root, req.isKeepAlive());
}

}
