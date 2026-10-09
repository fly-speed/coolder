#pragma once
#include "libai/runtime/coding_runtime.h"
#include "ai_agent_actions.h"
#include "action/actions.h"
#include "action/action_util.h"
namespace action
{
namespace agent_detail
{
bool absolute_project_path(const std::string &path);
bool project_location_allowed(request_t &req, const std::string &scope,
			      std::string &err);
void ai_agent_json_error(response_t &res, int status, const char *message,
			 bool keep_alive, const char *file, int line,
			 const char *function);
bool current_user_root(request_t &req, response_t &res, std::string &user_root,
		       std::string *username_out = NULL);
}
}

#undef json_error
#define json_error(res, status, message, keep_alive)                      \
	::action::agent_detail::ai_agent_json_error(res, status, message, \
						    keep_alive, __FILE__, \
						    __LINE__, __FUNCTION__)
