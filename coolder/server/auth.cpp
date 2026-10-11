#include "stdafx.h"
#include "auth.h"
#include <coding_host.h>
#include "host.h"
#include "action/action_util.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <map>
#include <set>

namespace coolder
{
namespace
{

std::map<std::string, account_t> accounts;
struct login_t {
	std::string username;
	std::chrono::steady_clock::time_point expires;
};
std::map<std::string, login_t> sessions;
struct attempt_t {
	unsigned count = 0;
	std::chrono::steady_clock::time_point start =
	    std::chrono::steady_clock::now();
};
std::map<std::string, attempt_t> attempts;
libai::cpp::Mutex accounts_mutex;
constexpr int iterations = 210000;

bool equal(const std::string &a, const std::string &b)
{
	return a.size() == b.size() &&
	    CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::string hex(const unsigned char *bytes, size_t size)
{
	std::string out;
	for (size_t i = 0; i < size; ++i) {
		out += "0123456789abcdef"[bytes[i] >> 4];
		out += "0123456789abcdef"[bytes[i] & 15];
	}
	return out;
}

std::string random_hex()
{
	unsigned char bytes[32];
	if (!(RAND_bytes(bytes, sizeof(bytes)) != 1))
		return hex(bytes, sizeof(bytes));
	throw std::runtime_error("secure random unavailable");
}

std::string password_hash(const std::string &password, const std::string &salt)
{
	unsigned char bytes[32];
	if (!(PKCS5_PBKDF2_HMAC(password.data(),
	          static_cast<int>(password.size()),
	          reinterpret_cast<const unsigned char *>(salt.data()),
	          static_cast<int>(salt.size()), iterations, EVP_sha256(),
	          sizeof(bytes), bytes) != 1))
		return hex(bytes, sizeof(bytes));
	throw std::runtime_error("password hashing failed");
}

bool valid_name(const std::string &name)
{
	return !name.empty() && name.size() <= 32 &&
	    name.find_first_not_of(
	        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
	    std::string::npos;
}

bool valid_hex(const std::string &value)
{
	return value.size() == 64 &&
	    value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string text(acl::json *body, const char *key)
{
	auto *n = body ? (*body)[key] : nullptr;
	return n && n->get_string() ? n->get_string() : "";
}

void private_directory(const std::string &path)
{
	namespace fs = std::filesystem;
	if (fs::is_symlink(fs::symlink_status(path))) {
		throw std::runtime_error(
		    "account directory cannot be a symlink");
	}
	fs::create_directories(path);
	fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
}

void migrate_preferences(const std::string &path)
{
	if (!std::filesystem::exists(path))
		return;
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
		throw std::runtime_error("preference file is a symlink");
	std::ifstream in(path);
	in >> std::ws;
	if (in.peek() == '{')
		return;
	std::string language, theme, size, extra;
	if (!(in >> language >> theme >> size) || in >> extra ||
	    language.empty() || (size != "sm" && size != "md" && size != "lg"))
		throw std::runtime_error("invalid legacy preferences");
	const auto backup = path + ".legacy";
	if (std::filesystem::is_symlink(
	        std::filesystem::symlink_status(backup)))
		throw std::runtime_error("preference backup is a symlink");
	if (!std::filesystem::exists(backup))
		std::filesystem::copy_file(path, backup);
	std::filesystem::permissions(backup,
	    std::filesystem::perms::owner_read |
	        std::filesystem::perms::owner_write,
	    std::filesystem::perm_options::replace);
	acl::json json;
	auto &row = json.create_node();
	row.add_text("language", language.c_str())
	    .add_text("theme", theme.c_str())
	    .add_text("font_size", size.c_str());
	const auto temp = path + "." + random_hex();
	std::ofstream out(temp);
	out << row.to_string().c_str();
	out.close();
	if (!out)
		throw std::runtime_error(
		    "cannot migrate interface preferences");
	std::filesystem::permissions(temp,
	    std::filesystem::perms::owner_read |
	        std::filesystem::perms::owner_write,
	    std::filesystem::perm_options::replace);
	if (!libai::cpp::coding::file_ops::replace_file(temp, path))
		throw std::runtime_error("cannot install preference JSON");
}

void migrate_provider_preferences(const std::string &path)
{
	if (!std::filesystem::exists(path))
		return;
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
		throw std::runtime_error("provider preferences is a symlink");
	std::ifstream in(path);
	in >> std::ws;
	if (in.peek() == '{')
		return;
	std::string provider, extra;
	std::getline(in, provider);
	if (std::getline(in, extra) && !extra.empty())
		throw std::runtime_error("invalid provider preferences");
	const auto backup = path + ".legacy";
	if (std::filesystem::is_symlink(
	        std::filesystem::symlink_status(backup)))
		throw std::runtime_error(
		    "provider preference backup is a symlink");
	if (!std::filesystem::exists(backup))
		std::filesystem::copy_file(path, backup);
	std::filesystem::permissions(backup,
	    std::filesystem::perms::owner_read |
	        std::filesystem::perms::owner_write,
	    std::filesystem::perm_options::replace);
	acl::json json;
	auto &row = json.create_node();
	row.add_text("ai_coding_provider_id", provider.c_str());
	const auto temp = path + "." + random_hex();
	std::ofstream out(temp);
	out << row.to_string().c_str();
	out.close();
	if (!out)
		throw std::runtime_error("cannot migrate provider preferences");
	std::filesystem::permissions(temp,
	    std::filesystem::perms::owner_read |
	        std::filesystem::perms::owner_write,
	    std::filesystem::perm_options::replace);
	if (!libai::cpp::coding::file_ops::replace_file(temp, path))
		throw std::runtime_error("cannot install provider preferences");
}

void persist(const std::map<std::string, account_t> &updated)
{
	const auto path = data_dir + "/auth/accounts.v1";
	const auto temporary = path + "." + random_hex();
	std::ofstream out(temporary);
	acl::json json;
	auto &root = json.create_node();
	for (const auto &item : updated) {
		const auto &a = item.second;
		auto &row = root.add_child(a.username.c_str(), true);
		row.add_text("username", a.username.c_str())
		    .add_text("id", a.id.c_str())
		    .add_bool("admin", a.admin)
		    .add_bool("enabled", a.enabled)
		    .add_text("salt", a.salt.c_str())
		    .add_text("digest", a.digest.c_str());
	}
	out << root.to_string().c_str() << '\n';
	out.close();
	if (!out) {
		throw std::runtime_error("cannot save accounts");
	}
	std::filesystem::permissions(temporary,
	    std::filesystem::perms::owner_read |
	        std::filesystem::perms::owner_write,
	    std::filesystem::perm_options::replace);
	if (!libai::cpp::coding::file_ops::replace_file(temporary, path)) {
		throw std::runtime_error("cannot replace account store");
	}
}

std::string session_key(const request_t &req)
{
	const char *cookies = req.getHeader("Cookie");
	std::istringstream stream(cookies ? cookies : "");
	std::string part;
	while (std::getline(stream, part, ';')) {
		part.erase(0, part.find_first_not_of(' '));
		if (!(part.compare(0, std::string("coolder_session").size() + 1,
		          std::string("coolder_session") + "=") == 0))
			continue;
		return part.substr(std::string("coolder_session").size() + 1);
	}
	return "";
}

bool current_locked(const request_t &req, account_t &out)
{
	auto s = sessions.find(session_key(req));
	if (s == sessions.end()) {
		return false;
	}
	if (s->second.expires <= std::chrono::steady_clock::now()) {
		sessions.erase(s);
		return false;
	}
	auto a = accounts.find(s->second.username);
	if (a == accounts.end() || !a->second.enabled) {
		return false;
	}
	out = a->second;
	return true;
}

void revoke(const std::string &username)
{
	for (auto i = sessions.begin(); i != sessions.end();) {
		if (i->second.username == username) {
			i = sessions.erase(i);
		} else {
			++i;
		}
	}
}

void login(response_t &res, const account_t &a)
{
	auto now = std::chrono::steady_clock::now();
	for (auto i = sessions.begin(); i != sessions.end();) {
		if (i->second.expires <= now) {
			i = sessions.erase(i);
		} else {
			++i;
		}
	}
	if (sessions.size() >= 4096) {
		throw std::runtime_error("too many login sessions");
	}
	const auto key = random_hex();
	sessions[key] = { a.username, now + std::chrono::hours(8) };
	res.setHeader("Set-Cookie",
	    (std::string("coolder_session") + "=" + key +
	        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800")
	        .c_str());
}

bool error(response_t &res, int status, const char *message)
{
	acl::json json;
	auto &root = json.create_node();
	root.add_bool("ok", false);
	root.add_text("error", message);
	return action::sendJson(res, status, root, false);
}

void append_account(acl::json_node &node, const account_t &a)
{
	node.add_text("username", a.username.c_str());
	node.add_bool("admin", a.admin);
	node.add_bool("enabled", a.enabled);
}
}

void accounts_init()
{
	for (const char *reserved :
	    { "/auth", "/users", "/.webcool_settings" }) {
		const std::string path = data_dir + reserved;
		if (!(workspace_dir == path ||
		        workspace_dir.compare(0, path.size() + 1, path + "/") ==
		            0 ||
		        path.compare(0, workspace_dir.size() + 1,
		            workspace_dir + "/") == 0))
			continue;
		throw std::runtime_error(
		    "workspace overlaps private account storage");
	}
	private_directory(data_dir + "/auth");
	private_directory(data_dir + "/users");
	const auto path = data_dir + "/auth/accounts.v1";
	if (!std::filesystem::exists(path)) {
		return;
	}
	if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
		throw std::runtime_error("account file is a symlink");
	std::ifstream file(path);
	std::string content((std::istreambuf_iterator<char>(file)), {});
	const bool legacy =
	    content.find_first_not_of(" \t\r\n") != std::string::npos &&
	    content[content.find_first_not_of(" \t\r\n")] != '{';
	std::stringstream in;
	if (legacy) {
		in.str(content);
		std::string version;
		int rounds = 0;
		if (!(in >> version >> rounds) ||
		    version != "coolder-accounts-v1" || rounds != iterations)
			throw std::runtime_error("invalid account store");
	} else {
		acl::json json(content.c_str());
		if (!json.finish() || !json.get_root().is_object())
			throw std::runtime_error("invalid account JSON");
		auto &root = json.get_root();
		for (auto *entry = root.first_child(); entry;
		     entry = root.next_child()) {
			const std::string name =
			    entry->tag_name() ? entry->tag_name() : "";
			auto *row =
			    entry->is_object() ? entry : entry->get_obj();
			if (!row)
				throw std::runtime_error(
				    "invalid account JSON record");
			if (!row->is_object() || !(*row)["admin"] ||
			    !(*row)["admin"]->is_bool() || !(*row)["enabled"] ||
			    !(*row)["enabled"]->is_bool() ||
			    name !=
			        libai::cpp::coding::json_values::text(
			            (*row)["username"]) ||
			    !(*row)["username"]->is_string() || !(*row)["id"] ||
			    !(*row)["id"]->is_string() || !(*row)["salt"] ||
			    !(*row)["salt"]->is_string() || !(*row)["digest"] ||
			    !(*row)["digest"]->is_string())
				throw std::runtime_error(
				    "invalid account JSON record");
			in << name << ' '
			   << libai::cpp::coding::json_values::text(
			          (*row)["id"])
			   << ' ' << *(*row)["admin"]->get_bool() << ' '
			   << *(*row)["enabled"]->get_bool() << ' '
			   << libai::cpp::coding::json_values::text(
			          (*row)["salt"])
			   << ' '
			   << libai::cpp::coding::json_values::text(
			          (*row)["digest"])
			   << '\n';
		}
	}
	std::set<std::string> ids;
	unsigned admins = 0;
	while (in >> std::ws && in.peek() != EOF) {
		account_t a;
		if (!(in >> a.username >> a.id >> a.admin >> a.enabled >>
		        a.salt >> a.digest) ||
		    !valid_name(a.username) || !valid_hex(a.id) ||
		    !valid_hex(a.salt) || !valid_hex(a.digest) ||
		    accounts.count(a.username) || !ids.insert(a.id).second) {
			throw std::runtime_error("invalid account store");
		}
		if (a.admin) {
			++admins;
			if (!a.enabled) {
				throw std::runtime_error(
				    "administrator disabled");
			}
		}
		if (!a.admin) {
			private_directory(data_dir + "/users/" + a.id);
			private_directory(account_workspace(a));
		}
		accounts[a.username] = a;
	}
	if (admins != 1 && (legacy || !accounts.empty())) {
		throw std::runtime_error(
		    "account store must contain one administrator");
	}
	if (legacy) {
		const auto backup = path + ".legacy";
		if (std::filesystem::is_symlink(
		        std::filesystem::symlink_status(backup)))
			throw std::runtime_error("account backup is a symlink");
		if (!std::filesystem::exists(backup)) {
			std::filesystem::copy_file(path, backup);
			std::filesystem::permissions(backup,
			    std::filesystem::perms::owner_read |
			        std::filesystem::perms::owner_write,
			    std::filesystem::perm_options::replace);
		}
		persist(accounts);
	}
	for (const auto &a : accounts) {
		migrate_preferences(
		    data_dir + "/auth/ui-" + a.second.id + ".v1");
		migrate_provider_preferences(a.second.admin ?
		        data_dir + "/preferences" :
		        data_dir + "/users/" + a.second.id + "/preferences");
	}
}

bool current_account(const request_t &req, account_t &out)
{
	std::lock_guard<libai::cpp::Mutex> guard(accounts_mutex);
	return current_locked(req, out);
}

std::vector<account_t> project_accounts()
{
	std::lock_guard<libai::cpp::Mutex> guard(accounts_mutex);
	std::vector<account_t> result;
	for (const auto &entry : accounts) {
		account_t identity;
		identity.id = entry.second.id;
		identity.username = entry.second.username;
		identity.admin = entry.second.admin;
		identity.enabled = entry.second.enabled;
		result.push_back(identity);
	}
	return result;
}

std::string account_workspace(const account_t &a)
{
	// The initial administrator inherits the existing installation's workspace.
	return a.admin ? workspace_dir :
	                 data_dir + "/users/" + a.id + "/workspace";
}

std::string account_preferences(const std::string &username)
{
	std::lock_guard<libai::cpp::Mutex> guard(accounts_mutex);
	auto a = accounts.find(username);
	if (!(a == accounts.end()))
		return a->second.admin ?
		    data_dir + "/preferences" :
		    data_dir + "/users/" + a->second.id + "/preferences";
	throw std::runtime_error("unknown account");
}

static bool handle_interface_preferences(response_t &res,
    const account_t &actor, const std::string &method, acl::json *body)
{
	// Account identity comes exclusively from the authenticated session.
	const auto filename = data_dir + "/auth/ui-" + actor.id + ".v1";
	std::string language = "zh", theme = "default", font_size = "md";
	bool legacy = false;
	std::ifstream in(filename);
	if (in) {
		std::string content((std::istreambuf_iterator<char>(in)), {});
		std::stringstream values(content);
		values >> std::ws;
		legacy = values.peek() != '{';
		if (legacy) {
			std::string extra;
			if (!(values >> language >> theme >> font_size) ||
			    values >> extra)
				throw std::runtime_error(
				    "invalid interface preferences");
		} else {
			acl::json prefs(content.c_str());
			if (!prefs.finish() || !prefs.get_root().is_object())
				throw std::runtime_error(
				    "invalid interface preferences JSON");
			language = text(&prefs, "language");
			theme = text(&prefs, "theme");
			font_size = text(&prefs, "font_size");
		}
	}
	if (method == "POST" || legacy) {
		if (method == "POST" && !body) {
			return error(res, 400, "invalid JSON body");
		}
		for (const auto *key : { "language", "theme", "font_size" }) {
			if (!(body && (*body)[key] &&
			        !(*body)[key]->get_string()))
				continue;
			return error(res, 400, "preferences must be strings");
		}
		if (body && (*body)["language"]) {
			language = text(body, "language");
		}
		if (body && (*body)["theme"]) {
			theme = text(body, "theme");
		}
		if (body && (*body)["font_size"]) {
			font_size = text(body, "font_size");
		}
		const std::set<std::string> themes = { "default", "blue",
			"green", "purple", "sand", "gray", "pink", "ocean" };
		if (language.empty() || language.size() > 24 ||
		    language.find_first_not_of(
		        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN"
		        "OPQRSTUVWXYZ0123456789-") != std::string::npos ||
		    language == "manifest" || language == "template" ||
		    !std::filesystem::is_regular_file(
		        html_dir + "/i18n/" + language + ".json") ||
		    std::filesystem::is_symlink(
		        html_dir + "/i18n/" + language + ".json") ||
		    !themes.count(theme) ||
		    (font_size != "sm" && font_size != "md" &&
		        font_size != "lg")) {
			return error(res, 400, "invalid interface preferences");
		}
		if (legacy) {
			if (std::filesystem::is_symlink(
			        std::filesystem::symlink_status(
			            filename + ".legacy")))
				throw std::runtime_error(
				    "preference backup is a symlink");
			if (!std::filesystem::exists(filename + ".legacy"))
				std::filesystem::copy_file(
				    filename, filename + ".legacy");
			std::filesystem::permissions(filename + ".legacy",
			    std::filesystem::perms::owner_read |
			        std::filesystem::perms::owner_write,
			    std::filesystem::perm_options::replace);
		}
		const auto temporary = filename + "." + random_hex();
		std::ofstream out(temporary);
		acl::json prefs;
		auto &node = prefs.create_node();
		node.add_text("language", language.c_str())
		    .add_text("theme", theme.c_str())
		    .add_text("font_size", font_size.c_str());
		out << node.to_string().c_str() << '\n';
		out.close();
		if (!out) {
			throw std::runtime_error(
			    "cannot save interface preferences");
		}
		std::filesystem::permissions(temporary,
		    std::filesystem::perms::owner_read |
		        std::filesystem::perms::owner_write,
		    std::filesystem::perm_options::replace);
		if (!libai::cpp::coding::file_ops::replace_file(
		        temporary, filename)) {
			throw std::runtime_error(
			    "cannot replace interface preferences");
		}
	}
	acl::json json;
	auto &result = json.create_node();
	result.add_bool("ok", true);
	result.add_text("language", language.c_str());
	result.add_text("theme", theme.c_str());
	result.add_text("font_size", font_size.c_str());
	return action::sendJson(res, 200, result, false);
}

bool auth_route(request_t &req, response_t &res, const std::string &method)
{
	const std::string path = req.getPathInfo();
	// Do not hold the account lock while waiting for a client's request body.
	acl::json *body = method == "POST" && path != "/api/logout" &&
	        path != "/api/v1/auth/logout" ?
	    req.getJson(8192) :
	    nullptr;
	std::lock_guard<libai::cpp::Mutex> guard(accounts_mutex);
	account_t actor;
	const bool logged_in = current_locked(req, actor);
	if (path == "/api/v1/auth/status" && method == "GET") {
		acl::json json;
		auto &root = json.create_node();
		root.add_bool("ok", true);
		root.add_bool("initialized", !accounts.empty());
		root.add_bool("authenticated", logged_in);
		if (!logged_in)
			return action::sendJson(res, 200, root, false);
		append_account(root, actor);
		root.add_bool("ai_agent_allowed",
		    libai::cpp::coding::ai_agent_access_allowed(
		        libai::cpp::coding::ai_runtime_policy_get(),
		        actor.admin));

		return action::sendJson(res, 200, root, false);
	}
	const bool registration = path == "/api/v1/auth/register";
	const bool signing_in =
	    path == "/api/login" || path == "/api/v1/auth/login";
	if (!registration && !signing_in && !logged_in) {
		return error(res, 401, "请先登录");
	}
	if ((path == "/api/logout" || path == "/api/v1/auth/logout") &&
	    method == "POST") {
		sessions.erase(session_key(req));
		res.setHeader("Set-Cookie",
		    (std::string("coolder_session") +
		        "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0")
		        .c_str());
		return reply(res, 200, "{\"ok\":true}");
	}
	if (path == "/api/v1/auth/preferences" &&
	    (method == "GET" || method == "POST")) {
		return handle_interface_preferences(res, actor, method, body);
	}
	if (path == "/api/v1/auth/users" && method == "GET") {
		if (!actor.admin) {
			return error(res, 403, "需要管理员权限");
		}
		acl::json json;
		auto &root = json.create_node();
		root.add_bool("ok", true);
		auto &list = json.create_array();
		root.add_child("users", list);
		for (const auto &a : accounts) {
			append_account(list.add_child(false, true), a.second);
		}
		return action::sendJson(res, 200, root, false);
	}
	if (method != "POST") {
		return error(res, 404, "route not found");
	}
	const bool creating = path == "/api/v1/auth/users/create";
	const bool updating = path == "/api/v1/auth/users/update";
	const bool password_change = path == "/api/v1/auth/password";
	if (!(registration || signing_in || creating || updating ||
	        password_change)) {
		return error(res, 404, "route not found");
	}
	if ((creating || updating) && !actor.admin) {
		return error(res, 403, "需要管理员权限");
	}
	if (registration && !accounts.empty()) {
		return error(res, 409, "管理员已经创建，请登录");
	}
	if (!body) {
		return error(res, 400, "invalid JSON body");
	}
	const std::string username =
	    password_change ? actor.username : text(body, "username");
	const std::string password = text(body, "password");
	if (!valid_name(username)) {
		return error(res, 400,
		    "用户名须为 1–32 位英文字母、数字、下划线或连字符");
	}
	if (signing_in) {
		auto now = std::chrono::steady_clock::now();
		if (attempts.size() > 4096) {
			attempts.clear();
		}
		auto &attempt = attempts[username];
		if (now - attempt.start > std::chrono::minutes(5)) {
			attempt = attempt_t();
		}
		if (attempt.count >= 10) {
			return error(
			    res, 429, "登录失败次数过多，请五分钟后重试");
		}
		auto a = accounts.find(username);
		const std::string salt =
		    a == accounts.end() ? std::string(64, '0') : a->second.salt;
		const auto hash = password_hash(password, salt);
		if (a == accounts.end() || !a->second.enabled ||
		    !equal(a->second.digest, hash)) {
			++attempt.count;
			return error(
			    res, 401, "用户名或密码不正确，或账户已停用");
		}
		attempts.erase(username);
		login(res, a->second);
		return reply(res, 200, "{\"ok\":true}");
	}
	if ((registration || creating || password_change ||
	        !password.empty()) &&
	    (password.size() < 10 || password.size() > 128 ||
	        password.find('\0') != std::string::npos)) {
		return error(res, 400, "密码长度须为 10–128 字节");
	}
	auto updated = accounts;
	if (registration || creating) {
		if (updated.count(username)) {
			return error(res, 409, "用户名已存在");
		}
		if (creating && (*body)["admin"] &&
		    (*body)["admin"]->get_bool() &&
		    *(*body)["admin"]->get_bool()) {
			return error(res, 400, "只能创建普通用户");
		}
		account_t a;
		a.username = username;
		a.id = random_hex();
		a.admin = registration;
		a.salt = random_hex();
		a.digest = password_hash(password, a.salt);
		private_directory(data_dir + "/users/" + a.id);
		private_directory(account_workspace(a));
		updated[username] = a;
	} else {
		auto a = updated.find(username);
		if (a == updated.end()) {
			return error(res, 404, "用户不存在");
		}
		if (updating && a->second.admin) {
			return error(res, 400, "管理员请使用修改密码功能");
		}
		if (password_change &&
		    !equal(actor.digest,
		        password_hash(
		            text(body, "current_password"), actor.salt))) {
			return error(res, 403, "原密码不正确");
		}
		if (updating && (*body)["enabled"]) {
			if (!(*body)["enabled"]->get_bool()) {
				return error(
				    res, 400, "enabled must be boolean");
			}
			a->second.enabled = *(*body)["enabled"]->get_bool();
		}
		if (!password.empty()) {
			a->second.salt = random_hex();
			a->second.digest =
			    password_hash(password, a->second.salt);
		}
	}
	persist(updated);
	accounts.swap(updated);
	revoke(username);
	attempts.erase(username);
	if (!(registration || password_change))
		return reply(res, 200, "{\"ok\":true}");
	login(res, accounts.at(username));

	return reply(res, 200, "{\"ok\":true}");
}
}
