#include "stdafx.h"
#include "project_sharing.h"
#include "host.h"
#include "action/action_util.h"
#include "libai/project/agent_project_store.h"
#include "libai/workspace/agent_workspace.h"
#include "libai/common/file_ops.h"
#include "libai/common/webcool_mutex.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <tuple>

namespace coolder
{
namespace
{
using webcool::ai::agent_project_record_t;
using webcool::ai::agent_project_store_t;
using webcool::ai::agent_workspace_t;
using grant_key = std::tuple<std::string, std::string, std::string>;
using grants_t = std::map<grant_key, std::string>;
// Serialize membership changes and collaborative writes through their commit.
webcool::mutex sharing_mutex;

std::string text(acl::json *body, const char *key)
{
	auto *node = body ? (*body)[key] : nullptr;
	return node && node->get_string() ? node->get_string() : "";
}

bool error(response_t &res, int status, const std::string &message)
{
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", message.c_str());
	return action::sendJson(res, status, root, false);
}

grants_t load_grants()
{
	grants_t grants;
	const auto path = data_dir + "/auth/project-members.v1";
	std::ifstream in(path);
	if (!in) {
		if (webcool::ai::file_ops::path_entry_exists(path))
			throw std::runtime_error("cannot read memberships");
		return grants;
	}
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream row(line);
		std::string owner, project, member, permission, extra;
		if (!(row >> std::quoted(owner) >> std::quoted(project) >>
		        std::quoted(member) >> std::quoted(permission)) ||
		    (row >> extra) ||
		    (permission != "read" && permission != "write"))
			throw std::runtime_error("invalid memberships");
		grants[{ owner, project, member }] = permission;
	}
	if (in.bad())
		throw std::runtime_error("cannot read memberships");
	return grants;
}

void save_grants(const grants_t &grants)
{
	const auto path = data_dir + "/auth/project-members.v1";
	std::ofstream out(path + ".tmp", std::ios::trunc);
	for (const auto &grant : grants) {
		out << std::quoted(std::get<0>(grant.first)) << ' '
		    << std::quoted(std::get<1>(grant.first)) << ' '
		    << std::quoted(std::get<2>(grant.first)) << ' '
		    << std::quoted(grant.second) << '\n';
	}
	out.close();
	if (!out || !webcool::ai::file_ops::replace_file(path + ".tmp", path))
		throw std::runtime_error("cannot save memberships");
}

std::string permission_for(const grants_t &grants, const account_t &owner,
    const account_t &actor, const std::string &project)
{
	if (owner.id == actor.id)
		return "owner";
	auto found = grants.find({ owner.id, project, actor.id });
	return found == grants.end() ? "" : found->second;
}

void project_json(acl::json_node &item, const agent_project_record_t &project,
    const account_t &owner, const std::string &permission)
{
	item.add_text("project_id", project.id.c_str());
	item.add_text("title", project.title.c_str());
	item.add_text("path", project.project_path.c_str());
	item.add_text("owner", owner.username.c_str());
	item.add_text("owner_id", owner.id.c_str());
	item.add_text("permission", permission.c_str());
	item.add_bool("shared", permission != "owner");
}

bool list_projects(response_t &res, const account_t &actor,
    const std::vector<account_t> &accounts, const grants_t &grants)
{
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", true);
	auto &items = json.create_array();
	root.add_child("projects", items);
	for (const auto &owner : accounts) {
		if (!owner.enabled)
			continue;
		// Do not inspect unrelated users' project stores.
		bool relevant = owner.id == actor.id;
		for (const auto &grant : grants)
			relevant = relevant ||
			    (std::get<0>(grant.first) == owner.id &&
			        std::get<2>(grant.first) == actor.id);
		if (!relevant)
			continue;
		std::vector<agent_project_record_t> projects;
		std::string err;
		if (!agent_project_store_t(account_workspace(owner))
		        .list(0, projects, err))
			return error(res, 500, err);
		for (const auto &project : projects) {
			const auto permission =
			    permission_for(grants, owner, actor, project.id);
			if (!permission.empty())
				project_json(items.add_child(false, true),
				    project, owner, permission);
		}
	}
	return action::sendJson(res, 200, root, false);
}

bool settings(response_t &res, acl::json *body, const std::string &method,
    const account_t &actor, const account_t &owner,
    const agent_project_record_t &project, const std::string &permission,
    const std::vector<account_t> &accounts, grants_t &grants)
{
	if (method == "POST") {
		if (actor.id != owner.id)
			return error(res, 403, "只有项目拥有者可以管理参与者");
		const auto requested = text(body, "permission");
		if (requested != "read" && requested != "write" &&
		    requested != "remove")
			return error(res, 400, "无效的项目权限");
		const auto username = text(body, "username");
		auto target = std::find_if(accounts.begin(), accounts.end(),
		    [&](const account_t &a) { return a.username == username; });
		if (target == accounts.end() ||
		    (!target->enabled && requested != "remove"))
			return error(res, 404, "目标用户不存在或已停用");
		if (target->id == owner.id)
			return error(res, 400, "不能修改项目拥有者的权限");
		const grant_key key{ owner.id, project.id, target->id };
		if (requested == "remove")
			grants.erase(key);
		else
			grants[key] = requested;
		save_grants(grants);
	}
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", true);
	project_json(root, project, owner, permission);
	auto &members = json.create_array();
	root.add_child("members", members);
	// Only owners can browse the account directory for inviting participants.
	// Expose identity and availability, never credentials or account settings.
	if (actor.id == owner.id) {
		auto &users = json.create_array();
		root.add_child("users", users);
		for (const auto &account : accounts) {
			auto &user = users.add_child(false, true);
			user.add_text("username", account.username.c_str());
			user.add_bool("enabled", account.enabled);
			user.add_text("permission",
			    permission_for(grants, owner, account, project.id)
			        .c_str());
		}
	}
	for (const auto &account : accounts) {
		const auto access =
		    permission_for(grants, owner, account, project.id);
		if (access.empty())
			continue;
		auto &member = members.add_child(false, true);
		member.add_text("username", account.username.c_str());
		member.add_text("permission", access.c_str());
		member.add_bool("enabled", account.enabled);
	}
	return action::sendJson(res, 200, root, false);
}

bool files(request_t &req, response_t &res, acl::json *body,
    const std::string &operation, const std::string &method,
    const account_t &owner, const agent_project_record_t &project,
    const std::string &permission)
{
	const bool writing = operation == "save";
	if ((writing && method != "POST") || (!writing && method != "GET"))
		return error(res, 405, "unsupported method");
	if (writing && permission == "read")
		return error(res, 403, "此项目为只读，无法保存文件");
	std::string absolute, err, normalized;
	if (!agent_workspace_t::resolve_project_root(
	        account_workspace(owner), project.project_path, absolute, err))
		return error(res, 400, err);
	const std::string path = writing ?
	    text(body, "path") :
	    (req.getParameter("path") ? req.getParameter("path") : "");
	if (!agent_workspace_t::normalize_path(
	        path, normalized, operation == "list", err))
		return error(res, 400, err);
	// Root the workspace at the granted project, never at its owner's home.
	agent_workspace_t workspace(absolute, false);
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("path", normalized.c_str());
	if (operation == "list") {
		std::vector<webcool::ai::workspace_entry_t> entries;
		if (!workspace.list(normalized, entries, err))
			return error(res, 400, err);
		auto &items = json.create_array();
		root.add_child("entries", items);
		for (const auto &entry : entries) {
			auto &item = items.add_child(false, true);
			item.add_text("path", entry.path.c_str());
			item.add_bool("directory", entry.directory);
			item.add_number("size", entry.size);
		}
	} else if (operation == "read") {
		std::string content;
		bool truncated = false;
		if (!workspace.read(normalized, content, truncated, err))
			return error(res, 400, err);
		root.add_text("content", content.c_str());
		root.add_text("sha256",
		    agent_workspace_t::content_sha256(content).c_str());
		root.add_bool("truncated", truncated);
	} else if (writing) {
		if (!(*body)["content"] || !(*body)["content"]->get_string())
			return error(res, 400, "content must be a string");
		if (!workspace.replace_text_if_unchanged(normalized,
		        text(body, "sha256"), text(body, "content"), err))
			return error(res, 409, err);
	} else {
		return error(res, 404, "route not found");
	}
	return action::sendJson(res, 200, root, false);
}
} // namespace

bool project_sharing_route(request_t &req, response_t &res,
    const std::string &method, const account_t &actor)
{
	const std::string path = req.getPathInfo();
	const std::string prefix = "/api/v1/collaborate/";
	const std::string operation = path.substr(prefix.size());
	if (method != "GET" && method != "POST")
		return error(res, 405, "unsupported method");
	// Account locks are released before acquiring the collaboration lock.
	const auto accounts = project_accounts();
	std::lock_guard<webcool::mutex> lock(sharing_mutex);
	auto grants = load_grants();
	if (operation == "projects" && method == "GET")
		return list_projects(res, actor, accounts, grants);
	acl::json *body =
	    method == "POST" ? req.getJson(2 * 1024 * 1024) : nullptr;
	if (method == "POST" && !body)
		return error(res, 400, "invalid JSON body");
	auto parameter = [&](const char *key) {
		return body ?
		    text(body, key) :
		    std::string(
		        req.getParameter(key) ? req.getParameter(key) : "");
	};
	const auto owner_id = parameter("owner_id");
	const auto project_id = parameter("project_id");
	auto owner = std::find_if(accounts.begin(), accounts.end(),
	    [&](const account_t &a) { return a.id == owner_id; });
	if (owner == accounts.end() || !owner->enabled)
		return error(res, 404, "project not found");
	const auto permission =
	    permission_for(grants, *owner, actor, project_id);
	if (permission.empty())
		return error(res, 403, "无权访问此项目");
	agent_project_record_t project;
	std::string err;
	if (!agent_project_store_t(account_workspace(*owner))
	        .get(project_id, project, err))
		return error(res, 404, "project not found");
	if (operation == "settings")
		return settings(res, body, method, actor, *owner, project,
		    permission, accounts, grants);
	return files(
	    req, res, body, operation, method, *owner, project, permission);
}
} // namespace coolder
