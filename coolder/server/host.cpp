#include "stdafx.h"
#include "host.h"
#include "auth.h"
#include "project_sharing.h"
#include "attachments.h"
#include "libai/agent/ai_admin_policy.h"
#include "action/action_util.h"
#include "action/ai/ai_agent_actions.h"
#include "action/ai/ai_provider_actions.h"
#include "action/ai/ai_workspace_actions.h"
#include "libai/workspace/agent_workspace.h"
#include <fstream>
#include <sstream>
#include <map>
#include <filesystem>

namespace coolder
{

std::string data_dir;
std::string workspace_dir;
std::string html_dir;
std::string authority;

bool reply(
    response_t &res, int status, const std::string &body, const char *type)
{
	res.setStatus(status);
	res.setContentType(type);
	res.setContentLength(body.size());
	res.setKeepAlive(false);
	return res.write(body.data(), body.size());
}

bool dispatch(request_t &req, response_t &res, const std::string &method)
{
	res.setHeader("X-Content-Type-Options", "nosniff");
	res.setHeader("Cache-Control", "no-store");
	res.setHeader("Content-Security-Policy",
	    "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
	    "worker-src 'self'; font-src 'self' data:; connect-src 'self'; "
	    "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'");
	const char *host = req.getHeader("Host");
	const char *origin = req.getHeader("Origin");
	if (!host || authority != host) {
		return reply(res, 403, "{\"error\":\"invalid Host\"}");
	}
	if (origin && std::string(origin) != "http://" + authority) {
		return reply(
		    res, 403, "{\"error\":\"cross-origin request denied\"}");
	}
	std::string path = req.getPathInfo() ? req.getPathInfo() : "/";
	if (path.compare(0, 13, "/api/v1/auth/") == 0 || path == "/api/login" ||
	    path == "/api/logout") {
		try {
			return auth_route(req, res, method);
		} catch (const std::exception &) {
			return reply(res, 500,
			    "{\"error\":\"account operation failed\"}");
		}
	}
	// Monaco's own scripts, CSS, fonts and workers are served only from its vendor
	// tree.
	if (method == "GET" &&
	    path.compare(0, 22, "/vendor/monaco-editor/") == 0) {
		const std::filesystem::path relative(path.substr(1));
		std::filesystem::path filename(html_dir);
		for (const auto &component : relative) {
			const auto part = component.string();
			if (part.empty() || part == "." || part == ".." ||
			    part.find_first_not_of(
			        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQR"
			        "STUVWXYZ0123456789-_.") != std::string::npos) {
				return reply(
				    res, 404, "asset not found", "text/plain");
			}
			filename /= component;
			if (!std::filesystem::is_symlink(filename))
				continue;
			return reply(res, 404, "asset not found", "text/plain");
		}
		const auto extension = filename.extension().string();
		const std::map<std::string, std::string> types = {
			{ ".js", "text/javascript; charset=utf-8" },
			{ ".css", "text/css; charset=utf-8" },
			{ ".ttf", "font/ttf" }, { ".woff", "font/woff" },
			{ ".woff2", "font/woff2" }
		};
		const auto type = types.find(extension);
		if (type == types.end() ||
		    !std::filesystem::is_regular_file(filename)) {
			return reply(res, 404, "asset not found", "text/plain");
		}
		std::ifstream file(filename, std::ios::binary);
		if (!file) {
			return reply(res, 404, "asset not found", "text/plain");
		}
		std::ostringstream content;
		content << file.rdbuf();
		return reply(res, 200, content.str(), type->second.c_str());
	}
	// Locale assets have a single validated filename; never serve arbitrary paths.
	if (method == "GET" && path.compare(0, 6, "/i18n/") == 0) {
		const auto name = path.substr(6);
		const auto filename = html_dir + path;
		if (name.size() < 6 || name.size() > 29 ||
		    name.substr(name.size() - 5) != ".json" ||
		    name.substr(0, name.size() - 5)
		            .find_first_not_of(
		                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
		                "VWXYZ0123456789-") != std::string::npos ||
		    std::filesystem::is_symlink(filename) ||
		    std::filesystem::is_symlink(html_dir + "/i18n")) {
			return reply(
			    res, 404, "{}", "application/json; charset=utf-8");
		}
		std::ifstream file(filename);
		if (!file) {
			return reply(
			    res, 404, "{}", "application/json; charset=utf-8");
		}
		std::ostringstream content;
		content << file.rdbuf();
		return reply(
		    res, 200, content.str(), "application/json; charset=utf-8");
	}
	if (method == "GET" &&
	    (path == "/" || path == "/coolder.js" || path == "/app.js" ||
	        path == "/admin.js" || path == "/i18n.js" ||
	        path == "/personal.js" || path == "/code-editor.js" ||
	        path == "/attachments.js" || path == "/style.css")) {
		std::ifstream file(
		    html_dir + (path == "/" ? "/index.html" : path),
		    std::ios::binary);
		if (!file) {
			return reply(
			    res, 404, "{\"error\":\"asset not found\"}");
		}
		std::ostringstream content;
		content << file.rdbuf();
		return reply(res, 200, content.str(),
		    path == "/" ? "text/html; charset=utf-8" :
		        (path == "/coolder.js" || path == "/app.js" ||
		            path == "/admin.js" || path == "/i18n.js" ||
		            path == "/personal.js" ||
		            path == "/code-editor.js" ||
		            path == "/attachments.js") ?
		                  "text/javascript; charset=utf-8" :
		                  "text/css; charset=utf-8");
	}
	account_t account;
	if (!current_account(req, account)) {
		return reply(
		    res, 401, "{\"error\":\"authentication required\"}");
	}
	if (path == "/api/health" && method == "GET") {
		return reply(res, 200,
		    "{\"ok\":true,\"app\":\"coolder\",\"version\":\"1.0.0\"}");
	}
	if (path == "/api/v1/admin/ai-policy" &&
	    (method == "GET" || method == "POST")) {
		if (account.admin)
			return action::AdminAiPolicyAction::run(
			    req, res, data_dir);
		return reply(res, 403, "{\"error\":\"需要管理员权限\"}");
	}
	const bool provider_management =
	    account.admin && path.compare(0, 20, "/api/v1/ai/providers") == 0;
	if (path.compare(0, 11, "/api/v1/ai/") == 0 && !provider_management &&
	    !webcool::ai::ai_agent_access_allowed(
	        webcool::ai::ai_runtime_policy_get(), account.admin)) {
		return reply(res, 403,
		    "{\"error\":\"管理员已关闭当前账户的 AI 编程权限\"}");
	}
	if (method == "POST" &&
	    (path == "/api/v1/ai/attachments" ||
	        path == "/api/v1/ai/attachments/discard")) {
		try {
			return attachment_route(req, res, account,
			    path != "/api/v1/ai/attachments");
		} catch (const std::exception &) {
			return reply(res, 500,
			    "{\"error\":\"attachment operation failed\"}");
		}
	}
	if (path.compare(0, 20, "/api/v1/collaborate/") == 0) {
		try {
			return project_sharing_route(req, res, method, account);
		} catch (const std::exception &) {
			return reply(res, 500,
			    "{\"error\":\"project sharing operation failed\"}");
		}
	}
	using handler = bool (*)(request_t &, response_t &);
	static const std::map<std::string, handler> routes = {
#include "routes.inc"
	};
	auto it = routes.find(method + " " + path);
	if (it == routes.end()) {
		return reply(res, 404, "{\"error\":\"route not found\"}");
	}
	try {
		return it->second(req, res);
	} catch (const std::exception &) {
		return reply(res, 500, "{\"error\":\"internal server error\"}");
	}
}

}

namespace action
{

std::string runtime_upload_dir_get()
{
	return coolder::data_dir;
}

bool auth_current_user(
    const request_t &req, const std::string &, std::string &user, bool &admin)
{
	coolder::account_t account;
	if (!coolder::current_account(req, account)) {
		return false;
	}
	user = account.username;
	admin = account.admin;
	return true;
}

bool authenticated_user_upload_dir(const request_t &req, const std::string &,
    std::string &root, std::string &err)
{
	coolder::account_t account;
	if (!coolder::current_account(req, account)) {
		err = "authentication required";
		return false;
	}
	root = coolder::account_workspace(account);
	return true;
}

bool auth_administrator_upload_dir(
    const std::string &, std::string &user, std::string &root, std::string &)
{
	// Stable provider principal preserves encrypted keys from single-user
	// installations.
	user = "owner";
	root = coolder::workspace_dir;
	return true;
}

bool auth_send_required(const request_t &, response_t &res)
{
	return coolder::reply(
	    res, 401, "{\"error\":\"authentication required\"}");
}

bool local_disk_access_allowed(const std::string &, bool, std::string &)
{
	// Project endpoints enforce authentication and the local-project role policy.
	return true;
}

bool local_dir_lock_path_allows(const std::string &, const std::string &,
    const std::string &, bool &allowed, std::string &, std::string &)
{
	// coolder has no separate directory-password lock service.
	allowed = true;
	return true;
}

const char *shared_folder_name()
{
	return "shared";
}

bool ensure_shared_upload_dir(std::string &err)
{
	err = "shared scope is disabled in coolder";
	return false;
}

user_prefs_t default_user_prefs()
{
	return user_prefs_t();
}

bool load_user_prefs(const std::string &, const std::string &username,
    user_prefs_t &prefs, std::string &)
{
	std::ifstream f(coolder::account_preferences(username));
	if (!f)
		return true;
	std::getline(f, prefs.ai_coding_provider_id);

	return true;
}

bool save_user_prefs(const std::string &, const std::string &username,
    const user_prefs_t &prefs, std::string &err)
{
	const std::string path = coolder::account_preferences(username);
	std::ofstream f(path + ".tmp");
	f << prefs.ai_coding_provider_id;
	f.close();
	if (!(!f || std::rename((path + ".tmp").c_str(), path.c_str()) != 0))
		return true;
	err = "cannot save preferences";
	return false;
}

bool normalize_relative_path(
    const char *p, std::string &normalized, std::string &err, bool empty)
{
	return webcool::ai::agent_workspace_t::normalize_path(
	    p ? p : "", normalized, empty, err);
}

std::string join_upload_path(const std::string &root, const std::string &path)
{
	return root + "/" + path;
}

bool sendJson(response_t &res, int code, const acl::string &json, bool)
{
	return coolder::reply(
	    res, code, std::string(json.c_str(), json.size()));
}

bool sendJson(response_t &res, int code, const acl::json_node &json, bool keep)
{
	acl::string out;
	json.to_string(&out);
	return sendJson(res, code, out, keep);
}

void json_error_at(response_t &res, int status, const char *msg, bool keep,
    const char *, int, const char *)
{
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", msg);
	sendJson(res, status, root, keep);
}

}
