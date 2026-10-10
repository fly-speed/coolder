#include "stdafx.h"
#include "ai_agent_actions_internal.h"
namespace action
{
using namespace agent_detail;

// Read the authenticated run's bounded journal after releasing runtime locks.
// The same durable source supports live snapshots and expired runtime entries.
static void add_model_interactions_json(acl::json_node &root,
    const std::string &user_root, const webcool::ai::agent_run_record_t &record,
    const std::shared_ptr<agent_runtime_task_t> &task)
{
	std::string log, err;
	if (task) {
		std::lock_guard<webcool::mutex> guard(task->operation_log_mutex);
		log = task->operation_log;
	} else {
		webcool::ai::agent_request_store_t store(
		    user_root, record.project_path, record.id);
		store.operation_log(log, false, err);
	}
	std::istringstream lines(log);
	std::string line, entries = "[";
	bool truncated = false, cache_available = record.cached_input_tokens > 0;
	while (std::getline(lines, line)) {
		acl::json event(line.c_str());
		if (!event.finish()) continue;
		const std::string name = json_text(event["event"]);
		const auto *cache = event["cache_usage_available"];
		if (cache && cache->get_bool() && *cache->get_bool()) cache_available = true;
		if (name == "operation_log_truncated") truncated = true;
		if (name != "model_request_started" &&
		    name != "model_response_completed" &&
		    name != "model_request_failed" &&
		    name != "tool_arguments_recovery" &&
		    name != "model_stream_interrupted_before_retry") continue;
		if (entries.size() > 1) entries += ",";
		entries += line;
	}
	entries += "]";
	root.add_text("model_interactions_json", entries.c_str());
	root.add_bool("model_interactions_truncated", truncated);
	root.add_bool("cache_usage_available", cache_available);
}

class runtime_subscription_guard_t {
public:
	explicit runtime_subscription_guard_t(
	    const std::shared_ptr<agent_runtime_task_t> &task)
	        : task_(task)
	        , active_(task && begin_runtime_subscription(task))
	{
	}

	~runtime_subscription_guard_t()
	{
		if (!active_)
			return;
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (task_->event_subscribers > 0)
			--task_->event_subscribers;
	}

	bool acquired() const
	{
		return !task_ || active_;
	}

private:
	std::shared_ptr<agent_runtime_task_t> task_;
	bool active_;
};
// Durable review artifacts outlive the bounded run audit log. Resolve an
// archived run only through an authenticated user's session and project.
bool AiAgentRunListAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	const char *pending_flag = req.getParameter("pending_review");
	const bool pending_only =
	    pending_flag && std::string(pending_flag) == "1";
	webcool::ai::agent_session_record_t review_session;
	if (pending_only) {
		const char *session_id = req.getParameter("session_id");
		webcool::ai::agent_session_store_t sessions(user_root);
		std::string session_err;
		if (!session_id ||
		    !sessions.get(session_id, review_session, session_err)) {
			json_error(res, 400, "invalid review session",
			    req.isKeepAlive());
			return true;
		}
	}
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
	webcool::ai::agent_run_store_t store(user_root);
	std::vector<webcool::ai::agent_run_record_t> records;
	std::string err;
	if (!store.list(pending_only ? 100 : limit, records, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node &items = json.create_array();
	root.add_child("runs", items);
	if (pending_only) {
		webcool::ai::agent_result_store_t results(
		    user_root, review_session.project_path);
		std::vector<webcool::ai::agent_pending_result_t> pending;
		if (!results.list_pending(review_session.id, pending, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		for (const auto &saved : pending) {
			acl::json_node &item = items.add_child(false, true);
			item.add_text("run_id", saved.run_id.c_str());
			item.add_number("started_at", saved.saved_at);
			item.add_number(
			    "pending_review_count", saved.pending_count);
			const auto record =
			    std::find_if(records.begin(), records.end(),
			        [&saved](const webcool::ai::agent_run_record_t
			                &candidate) {
				return candidate.id == saved.run_id;
			});
			item.add_text("model",
			    record != records.end() ? record->model.c_str() :
			                              "");
		}
	} else {
		for (size_t i = 0; i < records.size(); ++i) {
			acl::json_node &item = items.add_child(false, true);
			add_run_record_json(item, records[i]);
			// Running tasks have an exact session owner; project/provider matches
			// cannot distinguish two conversations in different browser windows.
			std::lock_guard<webcool::mutex> guard(
			    g_agent_runtime_mutex);
			const auto task = g_agent_runtime_tasks.find(
			    runtime_task_key(user_root, records[i].id));
			if (!(task != g_agent_runtime_tasks.end()))
				continue;
			item.add_text(
			    "session_id", task->second->session_id.c_str());
		}
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunStatusAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username))
		return true;
	const char *raw_id = req.getParameter("id");
	const std::string id = raw_id ? raw_id : "";
	const char *raw_session_id = req.getParameter("session_id");
	const std::string session_hint = raw_session_id ? raw_session_id : "";
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!load_review_run_record(user_root, id, session_hint, record, err)) {
		int status = 500;
		if (err == "invalid agent run id")
			status = 400;
		else if (err == "agent run not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(user_root, id);
	if (record.status == "running" && !runtime_task) {
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
		    runtime_upload_dir_get(), user_root, username);
		if (checkpoint_store.exists(id)) {
			// Recovery is lazy: the first status or SSE request after startup
			// recreates the provider fiber from the encrypted checkpoint.
			runtime_task =
			    recover_runtime_task(runtime_upload_dir_get(),
			        user_root, username, record, err);
			if (!runtime_task) {
				json_error(res, 503,
				    err.empty() ?
				        "agent restart recovery is temporarily unavailable" :
				        err.c_str(),
				    req.isKeepAlive());
				return true;
			}
		} else {
			std::string update_err;
			if (!store.fail(id,
			        "agent run interrupted by service restart",
			        update_err) ||
			    !store.get(id, record, update_err)) {
				json_error(res, 500, update_err.c_str(),
				    req.isKeepAlive());
				return true;
			}
		}
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	add_run_record_json(root, record);
	if (runtime_task) {
		// Terminal tasks remain cached for traces and usage. Their review state
		// may have been superseded by another run; use the same durable view as
		// the pending list and review endpoint, without discarding runtime data.
		webcool::ai::agent_result_t persisted;
		bool found = false;
		if (record.status != "running" &&
		    reviewable_agent_run_status(record.status)) {
			webcool::ai::agent_result_store_t result_store(
			    user_root, record.project_path);
			if (!result_store.load(
			        record.id, persisted, found, err)) {
				json_error(
				    res, 500, err.c_str(), req.isKeepAlive());
				return true;
			}
		}
		add_runtime_result_json(
		    root, runtime_task, found ? &persisted : NULL);
	} else if (reviewable_agent_run_status(record.status)) {
		webcool::ai::agent_result_store_t result_store(
		    user_root, record.project_path);
		webcool::ai::agent_result_t persisted;
		bool found = false;
		if (!result_store.load(record.id, persisted, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (found) {
			add_persisted_result_json(root, persisted,
			    result_store.relative_path(record.id));
		} else {
			root.add_bool("result_available", false);
			root.add_bool("result_persisted", false);
		}
		if (record.status == "failed" || record.status == "cancelled") {
			add_durable_recovery_json(
			    root, user_root, record, session_hint);
		}
	} else {
		add_runtime_result_json(root, runtime_task);
		add_durable_recovery_json(
		    root, user_root, record, session_hint);
	}
	add_model_interactions_json(root, user_root, record, runtime_task);
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunReasoningSaveAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string run_id = json_text((*body)["run_id"]);
	const std::string session_id = json_text((*body)["session_id"]);
	webcool::ai::agent_run_store_t run_store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!run_store.get(run_id, record, err)) {
		json_error(res, err == "agent run not found" ? 404 : 400,
		    err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string reasoning;
	const std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(user_root, run_id);
	if (runtime_task)
		reasoning = runtime_reasoning_for_save(runtime_task);
	if (reasoning.empty() && record.status == "completed") {
		// Completed runtime entries expire after a bounded retention period. Load
		// the durable project result so saving still works after service restart.
		webcool::ai::agent_result_store_t result_store(
		    user_root, record.project_path);
		webcool::ai::agent_result_t result;
		bool found = false;
		if (!result_store.load(run_id, result, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (found)
			reasoning = result.reasoning;
	}
	if (reasoning.empty() && !session_id.empty()) {
		// Failed/cancelled runtimes may have no result artifact, but their bounded
		// reasoning is retained with the private session transcript. Require both
		// the run id and project path to match before using that fallback.
		webcool::ai::agent_session_store_t session_store(user_root);
		webcool::ai::agent_session_record_t session;
		if (!session_store.get(session_id, session, err)) {
			json_error(res,
			    err == "agent session not found" ? 404 : 400,
			    err.c_str(), req.isKeepAlive());
			return true;
		}
		if (session.project_path != record.project_path) {
			err =
			    "AI run does not belong to the selected session project";
			webcool::ai::ai_log_error(
			    "agent.reasoning", "validate-session-project", err);
			json_error(res, 409, err.c_str(), req.isKeepAlive());
			return true;
		}
		for (size_t i = 0; i < session.messages.size(); ++i) {
			if (!(session.messages[i].role == "assistant" &&
			        session.messages[i].run_id == run_id))
				continue;
			reasoning = session.messages[i].reasoning;
			break;
		}
	}
	if (reasoning.empty()) {
		err = "this AI run has no reasoning text to save";
		webcool::ai::ai_log_error("agent.reasoning", "save-empty", err);
		json_error(res, 409, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string filename = "ai-reasoning-" + run_id + ".txt";
	const std::string relative_path = record.project_path.empty() ?
	    filename :
	    record.project_path + "/" + filename;
	webcool::ai::agent_workspace_t workspace(user_root);
	if (!workspace.save_generated_text(relative_path, reasoning, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (runtime_task) {
		acl::json saved_json;
		acl::json_node &saved_event = saved_json.create_node();
		saved_event.add_text("event", "reasoning_log_saved");
		saved_event.add_text("path", relative_path.c_str());
		saved_event.add_number(
		    "bytes", static_cast<long long>(reasoning.size()));
		append_runtime_operation_event(runtime_task, saved_event);
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", run_id.c_str());
	root.add_text("path", relative_path.c_str());
	root.add_text("operation_log_path",
	    (record.project_path.empty() ?
	            ".webcool_agent/ai-operations-" + run_id + ".jsonl" :
	            record.project_path + "/.webcool_agent/ai-operations-" +
	                run_id + ".jsonl")
	        .c_str());
	root.add_number("bytes", static_cast<long long>(reasoning.size()));
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunEventsAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username))
		return true;
	const char *raw_id = req.getParameter("id");
	const std::string id = raw_id ? raw_id : "";
	const char *raw_session_id = req.getParameter("session_id");
	const std::string session_hint = raw_session_id ? raw_session_id : "";
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		int status = 500;
		if (err == "invalid agent run id")
			status = 400;
		else if (err == "agent run not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(user_root, id);
	if (record.status == "running" && !runtime_task) {
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
		    runtime_upload_dir_get(), user_root, username);
		if (checkpoint_store.exists(id)) {
			runtime_task =
			    recover_runtime_task(runtime_upload_dir_get(),
			        user_root, username, record, err);
			if (!runtime_task) {
				json_error(res, 503,
				    err.empty() ?
				        "agent restart recovery is temporarily unavailable" :
				        err.c_str(),
				    req.isKeepAlive());
				return true;
			}
		} else {
			std::string update_err;
			if (!store.fail(id,
			        "agent run interrupted by service restart",
			        update_err) ||
			    !store.get(id, record, update_err)) {
				json_error(res, 500, update_err.c_str(),
				    req.isKeepAlive());
				return true;
			}
		}
	}
	runtime_subscription_guard_t subscription(runtime_task);
	if (!subscription.acquired()) {
		json_error(res, 429,
		    "too many event subscribers for this agent run",
		    req.isKeepAlive());
		return true;
	}

	res.setStatus(200);
	res.setContentType("text/event-stream; charset=utf-8");
	res.setHeader("Cache-Control", "no-cache, no-store");
	res.setHeader("X-Accel-Buffering", "no");
	res.setChunkedTransferEncoding(true);
	res.setKeepAlive(false);
	const char *retry = "retry: 2000\n\n";
	if (!res.write(retry, strlen(retry)))
		return true;

	unsigned long long last_version = static_cast<unsigned long long>(-1);
	unsigned long long last_staged_version =
	    static_cast<unsigned long long>(-1);
	size_t heartbeat_ticks = 0;
	for (;;) {
		const unsigned long long version =
		    runtime_event_version(runtime_task);
		if (version != last_version) {
			if (!store.get(id, record, err))
				break;
			const unsigned long long staged_version =
			    runtime_staged_change_version(runtime_task);
			acl::json json;
			acl::json_node &root = json.create_node();
			root.add_bool("ok", true);
			add_run_record_json(root, record);
			// File bodies, originals and diffs can total several megabytes. They
			// change only with staged_change_version, so do not resend them for
			// every reasoning/text token snapshot.
			add_runtime_result_json(root, runtime_task, NULL,
			    record.status != "running" ||
			        staged_version != last_staged_version);
			if (!runtime_task) {
				add_durable_recovery_json(
				    root, user_root, record, session_hint);
			}
			add_model_interactions_json(root, user_root, record, runtime_task);
			const std::string data = serialize_json(root);
			std::ostringstream frame;
			frame << "id: " << version << "\n"
			      << "event: "
			      << (record.status == "running" ? "snapshot" :
			                                       "complete")
			      << "\ndata: " << data << "\n\n";
			const std::string payload = frame.str();
			if (!res.write(payload.data(), payload.size()))
				return true;
			last_version = version;
			last_staged_version = staged_version;
			heartbeat_ticks = 0;
			if (record.status != "running")
				break;
		}
		acl::fiber::delay(250);
		if (!(++heartbeat_ticks >= 60))
			continue;
		const char *heartbeat = ": heartbeat\n\n";
		if (!res.write(heartbeat, strlen(heartbeat)))
			return true;
		heartbeat_ticks = 0;
	}
	(void)res.write(NULL, 0);
	return true;
}

static void clear_session_recovery(const std::string &user_root,
    const webcool::ai::agent_session_record_t &session)
{
	std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
	for (auto &item : g_agent_runtime_tasks) {
		if (!(item.second->user_root == user_root &&
		        item.second->session_id == session.id))
			continue;
		item.second->recovery_available = false;
	}
}

bool AiAgentRunCancelAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	// Explicitly abandon a suspended session without discarding its review ledger.
	const std::string recovery_session =
	    json_text((*body)["cancel_recovery_session_id"]);
	if (!recovery_session.empty()) {
		std::string err;
		webcool::ai::agent_session_record_t session;
		if (!webcool::ai::agent_session_store_t(user_root).get(
		        recovery_session, session, err)) {
			json_error(res, 404, err.c_str(), req.isKeepAlive());
			return true;
		}
		std::vector<webcool::ai::agent_run_record_t> runs;
		if (!webcool::ai::agent_run_store_t(user_root).list(
		        0, runs, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		for (const auto &run : runs) {
			if (!(run.project_path == session.project_path &&
			        run.status == "running"))
				continue;

			json_error(res, 409,
			    "project has a running task; stop it before cancelling recovery",
			    req.isKeepAlive());
			return true;
		}
		webcool::ai::agent_progress_store_t progress_store(
		    user_root, session.project_path, session.id);
		webcool::ai::agent_progress_t progress;
		bool found = false;
		if (!progress_store.load(progress, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		webcool::ai::agent_checkpoint_store_t checkpoints(
		    runtime_upload_dir_get(), user_root, username);
		for (const auto &run_id :
		    { session.last_run_id, progress.source_run_id }) {
			if (run_id.empty())
				continue;
			if (checkpoints.remove(run_id, err))
				continue;
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!progress_store.remove(err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		{
			clear_session_recovery(user_root, session);
		}
		acl::json json;
		auto &root = json.create_node();
		root.add_bool("ok", true);
		root.add_bool("recovery_available", false);
		root.add_text("session_id", session.id.c_str());
		return sendJson(res, 200, root, req.isKeepAlive());
	}
	const std::string id = json_text((*body)["run_id"]);
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		int status = 500;
		if (err == "invalid agent run id")
			status = 400;
		else if (err == "agent run not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (record.status != "running") {
		json_error(res, 409, "agent run is already finished",
		    req.isKeepAlive());
		return true;
	}
	bool already_done = false;
	const bool in_memory =
	    request_runtime_cancel(user_root, id, already_done);
	if (already_done) {
		json_error(res, 409, "agent run is already finished",
		    req.isKeepAlive());
		return true;
	}
	if (!in_memory) {
		// A running record without an in-memory task came from an interrupted
		// service process. Remove its encrypted recovery material before closing
		// the audit record so cancellation cannot restart it later.
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
		    runtime_upload_dir_get(), user_root, username);
		if (!checkpoint_store.remove(id, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!store.cancel(id, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
	}
	if (in_memory) {
		const std::shared_ptr<agent_runtime_task_t> runtime_task =
		    find_runtime_task(user_root, id);
		append_simple_operation_event(runtime_task, "cancel_requested",
		    "cancelling", "",
		    runtime_completed_tool_count(runtime_task));
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("cancel_requested", in_memory);
	root.add_text("status", in_memory ? "running" : "cancelled");
	return sendJson(res, in_memory ? 202 : 200, root, req.isKeepAlive());
}

bool AiAgentRunPauseAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["run_id"]);
	const acl::json_node *paused_node = (*body)["paused"];
	const bool *paused_value =
	    paused_node == NULL ? NULL : paused_node->get_bool();
	if (paused_value == NULL) {
		json_error(
		    res, 400, "paused must be a boolean", req.isKeepAlive());
		return true;
	}
	const bool paused = *paused_value;
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		const int status = err == "invalid agent run id" ? 400 :
		    err == "agent run not found"                 ? 404 :
		                                                   500;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (record.status != "running") {
		json_error(res, 409, "agent run is already finished",
		    req.isKeepAlive());
		return true;
	}
	bool already_done = false;
	bool pause_requested = false;
	if (!request_runtime_pause(
	        user_root, id, paused, already_done, pause_requested)) {
		err =
		    "agent run cannot be paused because its live worker is unavailable";
		webcool::ai::ai_log_error(
		    "agent.runtime", "pause-worker-missing", err);
		json_error(res, 409, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (already_done) {
		json_error(res, 409, "agent run is already finished",
		    req.isKeepAlive());
		return true;
	}
	const std::shared_ptr<agent_runtime_task_t> runtime_task =
	    find_runtime_task(user_root, id);
	append_simple_operation_event(runtime_task,
	    paused ? "pause_requested" : "resume_requested",
	    pause_requested ? "pausing" : "resuming", "",
	    runtime_completed_tool_count(runtime_task));
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("pause_requested", pause_requested);
	root.add_text("phase", pause_requested ? "pausing" : "resuming");
	return sendJson(res, 202, root, req.isKeepAlive());
}

} // namespace action
