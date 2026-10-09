#include "stdafx.h"
// Shared HTTP authentication, JSON helpers and persisted run lookup.
#include "ai_agent_actions_internal.h"

namespace action
{
namespace agent_detail
{

// HTTP validation/storage failures are logged here without recording prompts,
// model replies, tool results or proposed file contents.

void ai_agent_json_error(response_t &res, int status, const char *message,
			 bool keep_alive, const char *file, int line,
			 const char *function)
{
	webcool::ai::ai_log_error("http.ai.agent", "response",
				  message ? message :
					    "unspecified agent action error");
	action::json_error_at(res, status, message, keep_alive, file, line,
			      function);
}

// A provider/protocol failure may happen after the agent has already produced
// useful, durable file generations. Those generations must remain reviewable:
// the run status describes model execution, not whether a generation can be
// accepted. Running generations are immutable by generation/hash and are safe to
// review; a later model edit receives a new pending generation.
bool absolute_project_path(const std::string &path)
{
#ifdef _WIN32
	return (path.size() >= 3 && path[1] == ':' &&
		(path[2] == '/' || path[2] == '\\')) ||
	       (path.size() >= 2 && path[0] == '\\' && path[1] == '\\');
#else
	return !path.empty() && path[0] == '/';
#endif
}

bool project_location_allowed(request_t &req, const std::string &scope,
			      std::string &err)
{
	if (scope == "shared") {
		err = "shared scope is disabled in coolder";
		return false;
	}

	if (scope == "personal")
		return true;
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
	if (!admin &&
	    ((scope == "shared" && !policy.allow_users_shared_projects) ||
	     (scope == "local" && !policy.allow_users_local_projects))) {
		err = scope == "shared" ?
			      "administrator has disabled shared-directory projects" :
			      "administrator has disabled local-disk projects";
		return false;
	}
	if (scope == "local" &&
	    !local_disk_access_allowed(upload_root, admin, err)) {
		if (err.empty())
			err = "local disk access is disabled";
		return false;
	}
	return true;
}

bool current_user_root(request_t &req, response_t &res, std::string &user_root,
		       std::string *username_out)
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
	if (username_out != NULL)
		*username_out = username;
	return true;
}

} // namespace agent_detail
} // namespace action
