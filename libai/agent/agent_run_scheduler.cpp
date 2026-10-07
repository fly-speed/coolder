#include "stdafx.h"
#include "agent_run_scheduler.h"

#include <map>

namespace webcool {
namespace ai {

std::string normalize_agent_resource_path(const std::string& path)
{
	std::string value = path;
	for (size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '\\') value[i] = '/';
#ifdef _WIN32
		if (value[i] >= 'A' && value[i] <= 'Z') value[i] += 'a' - 'A';
#endif
	}
	std::vector<std::string> parts;
	size_t begin = 0;
	for (size_t i = 0; i <= value.size(); ++i) {
		if (i != value.size() && value[i] != '/') continue;
		const std::string part = value.substr(begin, i - begin);
		begin = i + 1;
		if (part.empty() || part == ".") continue;
		if (part == "..") { if (!parts.empty()) parts.pop_back(); }
		else parts.push_back(part);
	}
	std::string result = !value.empty() && value[0] == '/' ? "/" : "";
	for (size_t i = 0; i < parts.size(); ++i) {
		if (!result.empty() && result[result.size() - 1] != '/') result += '/';
		result += parts[i];
	}
	return result;
}

namespace {
bool within_project(const std::string& path, const std::string& project)
{
	return project.empty() || path == project
		|| (path.size() > project.size() && path.compare(0, project.size(), project) == 0
			&& (project[project.size() - 1] == '/' || path[project.size()] == '/'));
}
}

bool agent_run_scopes_conflict(const agent_run_scope_t& left,
	const agent_run_scope_t& right)
{
	if (left.user_root != right.user_root) return false;
	const bool left_document = left.tool_free && !left.document_path.empty();
	const bool right_document = right.tool_free && !right.document_path.empty();
	const bool left_chat = left.tool_free && !left_document;
	const bool right_chat = right.tool_free && !right_document;
	if (left_chat || right_chat) {
		return left_chat && right_chat && !left.conversation_id.empty()
			&& left.conversation_id == right.conversation_id;
	}
	if (left_document && right_document) return left.document_path == right.document_path;
	if (left_document) return within_project(left.document_path, right.project_path);
	if (right_document) return within_project(right.document_path, left.project_path);
	return within_project(left.project_path, right.project_path)
		|| within_project(right.project_path, left.project_path)
		|| (!left.session_id.empty() && left.session_id == right.session_id);
}

std::string select_next_agent_run(const std::vector<agent_run_slot_t>& runs,
	size_t global_limit, size_t per_user_limit)
{
	if (global_limit == 0 || per_user_limit == 0) return "";
	size_t active = 0;
	std::map<std::string, size_t> active_by_user;
	for (size_t i = 0; i < runs.size(); ++i) {
		if (runs[i].done || !runs[i].admitted) continue;
		++active;
		++active_by_user[runs[i].user_scope];
	}
	if (active >= global_limit) return "";
	const agent_run_slot_t* selected = NULL;
	for (size_t i = 0; i < runs.size(); ++i) {
		const agent_run_slot_t& run = runs[i];
		if (run.done || run.admitted || run.cancelled || run.paused
			|| active_by_user[run.user_scope] >= per_user_limit) continue;
		if (selected == NULL || run.sequence < selected->sequence) selected = &run;
	}
	return selected == NULL ? "" : selected->key;
}

} // namespace ai
} // namespace webcool
