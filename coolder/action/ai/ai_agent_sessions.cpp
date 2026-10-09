#include "stdafx.h"
#include "libai/storage/agent_run_store.h"
#include "ai_agent_actions_internal.h"
namespace action
{
using namespace agent_detail;
bool AiAgentSessionListAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	std::string requested_path;
	std::string path_err;
	const char *raw_project_id = req.getParameter("project_id");
	const char *raw_path = req.getParameter("path");
	const char *raw_detail_session_id = req.getParameter("session_id");
	if (raw_project_id != NULL && *raw_project_id != '\0') {
		webcool::ai::agent_project_store_t project_store(user_root);
		webcool::ai::agent_project_record_t project;
		if (!project_store.get(raw_project_id, project, path_err)) {
			json_error(res,
				   path_err == "agent project not found" ? 404 :
									   500,
				   path_err.c_str(), req.isKeepAlive());
			return true;
		}
		requested_path = project.project_path;
	} else if (raw_path != NULL &&
		   !webcool::ai::agent_workspace_t::normalize_path(
			   raw_path, requested_path, false, path_err)) {
		json_error(res, 400, path_err.c_str(), req.isKeepAlive());
		return true;
	}
	webcool::ai::agent_session_store_t store(user_root);
	std::vector<webcool::ai::agent_session_record_t> sessions;
	std::string err;
	if (!store.list(50, sessions, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	// Session messages are persisted at completion. Use the run audit for the
	// user's submission time rather than presenting completion as submission.
	webcool::ai::agent_run_store_t run_store(user_root);
	std::vector<webcool::ai::agent_run_record_t> runs;
	std::string run_err;
	run_store.list(100, runs, run_err);
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node &items = json.create_array();
	root.add_child("sessions", items);
	for (size_t i = 0; i < sessions.size(); ++i) {
		// A conversation belongs to exactly one normalized project workspace.
		// Supplying a project path prevents unrelated conversations from crossing
		// the project boundary in the browser response.
		if ((raw_project_id != NULL || raw_path != NULL) &&
		    sessions[i].project_path != requested_path)
			continue;
		acl::json_node &item = items.add_child(false, true);
		item.add_text("session_id", sessions[i].id.c_str());
		item.add_text("title", sessions[i].title.c_str());
		item.add_text("agent_id", sessions[i].agent_id.c_str());
		item.add_text("provider_id", sessions[i].provider_id.c_str());
		item.add_text("path", sessions[i].project_path.c_str());
		if (!sessions[i].last_run_id.empty()) {
			item.add_text("last_run_id",
				      sessions[i].last_run_id.c_str());
		}
		item.add_number("created_at", sessions[i].created_at);
		item.add_number("updated_at", sessions[i].updated_at);
		item.add_number("turn_count", sessions[i].turn_count);
		bool recovery_available = false;
		std::string recovery_err;
		webcool::ai::agent_progress_store_t progress_store(
			user_root, sessions[i].project_path, sessions[i].id);
		if (!progress_store.exists(recovery_available, recovery_err)) {
			// Conversation history is still useful when one malformed checkpoint
			// cannot be inspected; log the precise problem and expose no resume action.
			webcool::ai::ai_log_error("agent.session",
						  "probe-progress",
						  recovery_err);
			recovery_available = false;
		}
		item.add_bool("recovery_available", recovery_available);
		item.add_bool("has_summary", !sessions[i].summary.empty());
		// Summary and bounded message history have different jobs: the former is
		// compact model memory, while the latter restores the user's visible chat.
		const bool include_session_detail =
			raw_detail_session_id == NULL ||
			sessions[i].id == raw_detail_session_id;
		if (include_session_detail && !sessions[i].summary.empty()) {
			item.add_text("summary", sessions[i].summary.c_str());
		}
		acl::json_node &messages = json.create_array();
		item.add_child("messages", messages);
		for (size_t j = 0;
		     include_session_detail && j < sessions[i].messages.size();
		     ++j) {
			const webcool::ai::agent_session_message_t &saved =
				sessions[i].messages[j];
			acl::json_node &message =
				messages.add_child(false, true);
			message.add_text(
				"message_id",
				(saved.run_id + ":" + saved.role).c_str());
			message.add_text("run_id", saved.run_id.c_str());
			message.add_text("role", saved.role.c_str());
			message.add_text("text", saved.text.c_str());
			message.add_text("state", saved.state.c_str());
			message.add_number("created_at", saved.created_at);
			if (saved.role == "user") {
				long long sent_at = 0;
				for (const auto &run : runs) {
					if (run.id == saved.run_id &&
					    run.project_path ==
						    sessions[i].project_path) {
						sent_at = run.started_at;
						break;
					}
				}
				message.add_number("sent_at", sent_at);
			}
			if (!saved.reasoning.empty()) {
				message.add_text("reasoning",
						 saved.reasoning.c_str());
			}
			if (!saved.completion_summary.empty()) {
				message.add_text(
					"completion_summary",
					saved.completion_summary.c_str());
			}
			message.add_number("duration_ms", saved.duration_ms);
			message.add_number("input_tokens", saved.input_tokens);
			message.add_number("cached_input_tokens",
					   saved.cached_input_tokens);
			message.add_number("output_tokens",
					   saved.output_tokens);
			message.add_number("reasoning_tokens",
					   saved.reasoning_tokens);
			// The per-run artifact is the durable source of file history, including
			// accepted/rejected revisions and runs evicted from the audit list.
			// Return metadata only; source snapshots are fetched when opening review.
			acl::json_node &files = json.create_array();
			message.add_child("changes", files);
			if (saved.role == "assistant") {
				webcool::ai::agent_result_store_t results(
					user_root, sessions[i].project_path);
				webcool::ai::agent_result_t result;
				bool found = false;
				std::string result_err;
				if (!results.load(saved.run_id, result, found,
						  result_err)) {
					webcool::ai::ai_log_error(
						"agent.session",
						"load-message-files",
						result_err);
				} else if (found && result.session_id ==
							    sessions[i].id) {
					for (size_t k = 0;
					     k < result.changes.size(); ++k) {
						const webcool::ai::agent_change_proposal_t
							&change = result.changes
									  [k];
						acl::json_node &file =
							files.add_child(false,
									true);
						file.add_text("operation",
							      change.operation
								      .c_str());
						file.add_text(
							"path",
							change.path.c_str());
						file.add_text("target_path",
							      change.target_path
								      .c_str());
						file.add_text(
							"review_status",
							change.review_status
								.c_str());
						file.add_number(
							"added_lines",
							change.added_lines);
						file.add_number(
							"removed_lines",
							change.removed_lines);
					}
				}
			}
		}
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentSessionSaveAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(32 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["session_id"]);
	webcool::ai::agent_session_store_t store(user_root);
	webcool::ai::agent_session_record_t session;
	std::string err;
	if (!store.get(id, session, err)) {
		int status = 500;
		if (err == "invalid agent session id")
			status = 400;
		else if (err == "agent session not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}

	// Export only the already bounded session record. Source code, tool output,
	// request JSON and API credentials are intentionally absent from this file.
	std::ostringstream text;
	text << "coolder AI coding session\n"
	     << "Title: " << session.title << "\n"
	     << "Session: " << session.id << "\n"
	     << "Project: "
	     << (session.project_path.empty() ? "." : session.project_path)
	     << "\n"
	     << "Provider: " << session.provider_id << "\n"
	     << "Turns: " << session.turn_count << "\n"
	     << "Created: " << session.created_at << "\n"
	     << "Updated: " << session.updated_at << "\n\n";
	for (size_t i = 0; i < session.messages.size(); ++i) {
		const webcool::ai::agent_session_message_t &message =
			session.messages[i];
		text << "[" << message.role << "]";
		if (message.created_at > 0)
			text << " " << message.created_at;
		if (message.duration_ms > 0) {
			text << " duration_ms=" << message.duration_ms;
		}
		text << "\n" << message.text << "\n\n";
		if (!message.completion_summary.empty()) {
			text << "[completion summary]\n"
			     << message.completion_summary << "\n\n";
		}
		if (!message.reasoning.empty()) {
			text << "[reasoning]\n" << message.reasoning << "\n\n";
		}
	}
	if (!session.summary.empty()) {
		text << "[memory summary]\n" << session.summary << "\n";
	}
	const std::string content = text.str();
	const std::string filename = "ai-session-" + session.id + ".txt";
	const std::string relative_path =
		session.project_path.empty() ?
			filename :
			session.project_path + "/" + filename;
	webcool::ai::agent_workspace_t workspace(user_root);
	if (!workspace.save_generated_text(relative_path, content, err)) {
		webcool::ai::ai_log_error("agent.session", "save-transcript",
					  err);
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("session_id", id.c_str());
	root.add_text("path", relative_path.c_str());
	root.add_number("bytes", static_cast<long long>(content.size()));
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentSessionDeleteAction::run(request_t &req, response_t &res)
{
	std::string user_root;
	if (!current_user_root(req, res, user_root))
		return true;
	acl::json *body = req.getJson(32 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["session_id"]);
	webcool::ai::agent_session_store_t store(user_root);
	std::string err;
	if (!store.remove(id, err)) {
		int status = 500;
		if (err == "invalid agent session id")
			status = 400;
		else if (err == "agent session not found")
			status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	size_t removed_workflows = 0;
	webcool::ai::agent_workflow_store_t workflow_store(user_root);
	if (!workflow_store.remove_for_session(id, removed_workflows, err)) {
		// The requested conversation is already gone. Do not falsely report the
		// delete as failed, but leave an operator-visible cleanup error.
		webcool::ai::ai_log_error("http.ai.session", "remove-workflows",
					  err);
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("session_id", id.c_str());
	root.add_number("removed_workflow_count",
			static_cast<long long>(removed_workflows));
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action
