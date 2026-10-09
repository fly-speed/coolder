#include "stdafx.h"
#include "browser_debug.h"
#include "../agent/ai_admin_policy.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <set>
namespace webcool
{
namespace ai
{
namespace
{
using clock_type = std::chrono::steady_clock;
struct session_t {
	std::string evidence_id, owner, project, token, client, url, browser,
	    run, command, result, image;
	unsigned long sequence = 0;
	bool images = false, connected = false, delivered = false,
	     revoked = false;
	clock_type::time_point expires, heartbeat;
};
std::mutex guard;
std::map<std::string, std::shared_ptr<session_t>> sessions;
std::string text(const acl::json_node *n)
{
	return n && n->get_string() ? n->get_string() : "";
}
std::string encode(acl::json_node &n)
{
	const auto &s = n.to_string();
	return std::string(s.c_str(), s.size());
}
std::string error(const char *e)
{
	acl::json j;
	auto &r = j.create_node();
	r.add_bool("ok", false);
	r.add_text("error", e);
	return encode(r);
}
std::string random_token()
{
	unsigned char bytes[32];
	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return "";
	std::string out;
	for (auto b : bytes) {
		out += "0123456789abcdef"[b >> 4];
		out += "0123456789abcdef"[b & 15];
	}
	return out;
}
void prune()
{
	auto now = clock_type::now();
	for (auto i = sessions.begin(); i != sessions.end();) {
		if (i->second->revoked || i->second->expires < now) {
			i->second->revoked = true;
			i = sessions.erase(i);
		} else
			++i;
	}
}
std::shared_ptr<session_t> find(
    const std::string &owner, const std::string &project)
{
	prune();
	for (auto &p : sessions) {
		if (!(p.second->owner == owner && p.second->project == project))
			continue;
		return p.second;
	}
	return {};
}
bool alive(const session_t &s)
{
	return !s.revoked && s.connected && s.expires > clock_type::now() &&
	    clock_type::now() - s.heartbeat < std::chrono::seconds(10);
}
std::string status(const std::shared_ptr<session_t> &s)
{
	acl::json j;
	auto &r = j.create_node();
	r.add_bool("ok", true);
	r.add_bool("connected", s && alive(*s));
	r.add_bool("paired", !!s);
	if (!s)
		return encode(r);
	r.add_text("url", s->url.c_str());
	r.add_text("browser", s->browser.c_str());
	r.add_bool("images_allowed", s->images);

	return encode(r);
}
}
std::string browser_debug_create(
    const std::string &owner, const std::string &project, bool images)
{
	std::lock_guard<std::mutex> lock(guard);
	if (!ai_runtime_policy_get().allow_browser_debug)
		return error("Browser debugging is disabled by policy");
	auto old = find(owner, project);
	if (old) {
		old->revoked = true;
		sessions.erase(old->token);
	}
	if (sessions.size() >= 32)
		return error("Too many browser debug sessions");
	auto s = std::make_shared<session_t>();
	s->token = random_token();
	if (s->token.empty())
		return error("Cannot create pairing token");
	s->evidence_id = random_token();
	if (s->evidence_id.empty())
		return error("Cannot create evidence identity");
	s->owner = owner;
	s->project = project;
	s->images = images;
	s->expires = clock_type::now() + std::chrono::minutes(5);
	sessions[s->token] = s;
	acl::json j;
	auto &r = j.create_node();
	r.add_bool("ok", true);
	r.add_text("token", s->token.c_str());
	r.add_number("pairing_expires_seconds", 300);
	return encode(r);
}
std::string browser_debug_status(
    const std::string &owner, const std::string &project, bool revoke)
{
	std::lock_guard<std::mutex> lock(guard);
	auto s = find(owner, project);
	if (!(s && revoke))
		return status(s);
	s->revoked = true;
	sessions.erase(s->token);
	s.reset();

	return status(s);
}
std::string browser_debug_exchange(const std::string &body)
{
	if (body.size() > 900 * 1024)
		return error("Bridge payload too large");
	acl::json j(body.c_str());
	auto &in = j.get_root();
	std::lock_guard<std::mutex> lock(guard);
	prune();
	if (!ai_runtime_policy_get().allow_browser_debug)
		return error("Browser debugging disabled");
	auto it = sessions.find(text(in["token"]));
	if (it == sessions.end())
		return error("Pairing expired or revoked");
	auto s = it->second;
	const auto client = text(in["client"]), op = text(in["op"]);
	if (client.size() < 16 || client.size() > 128)
		return error("Invalid bridge client");
	if (op == "hello") {
		if (s->connected)
			return error(
			    "Pairing already consumed; create a new pairing");
		const auto url = text(in["url"]);
		if (url.size() > 2048 ||
		    (url.compare(0, 7, "http://") &&
		        url.compare(0, 8, "https://")))
			return error("Only HTTP(S) tabs can be paired");
		s->client = client;
		s->url = url;
		s->browser = text(in["browser"]).substr(0, 200);
		s->connected = true;
		s->expires = clock_type::now() + std::chrono::minutes(30);
	} else if (!s->connected || client != s->client)
		return error("Bridge client mismatch");
	if (op == "disconnect") {
		s->revoked = true;
		sessions.erase(it);
		return "{\"ok\":true}";
	}
	if (op != "hello" && op != "poll" && op != "result")
		return error("Unknown bridge operation");
	s->heartbeat = clock_type::now();
	if (op == "result") {
		if (s->command.empty() ||
		    text(in["id"]) != std::to_string(s->sequence) ||
		    !s->delivered || !s->result.empty())
			return error("Stale or unsolicited command result");
		const auto result = text(in["result"]);
		if (result.empty() || result.size() > 48 * 1024)
			return error("Invalid browser evidence size");
		const auto image = text(in["image"]);
		if (!image.empty()) {
			if (!s->images || image.size() > 800 * 1024 ||
			    image.size() % 4 ||
			    image.find_first_not_of(
			        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
			        std::string::npos)
				return error(
				    "Screenshot not authorized or oversized");
			std::string bytes(image.size() / 4 * 3, '\0');
			int size = EVP_DecodeBlock(
			    reinterpret_cast<unsigned char *>(&bytes[0]),
			    reinterpret_cast<const unsigned char *>(
			        image.data()),
			    static_cast<int>(image.size()));
			if (size < 0)
				return error("Invalid screenshot encoding");
			if (image.back() == '=')
				--size;
			if (image.size() > 1 && image[image.size() - 2] == '=')
				--size;
			bytes.resize(static_cast<size_t>(size));
			if (bytes.size() < 3 ||
			    bytes.compare(0, 3, "\xff\xd8\xff", 3))
				return error("Expected JPEG screenshot");
			s->image = bytes;
		}
		acl::json evidence(result.c_str());
		if (!evidence.finish() || !evidence.get_root().is_object())
			return error("Invalid evidence JSON");
		s->result = result;
	}
	acl::json out;
	auto &r = out.create_node();
	r.add_bool("ok", true);
	r.add_bool("images_allowed", s->images);
	if (!(op == "poll" && !s->command.empty() && !s->delivered))
		return encode(r);
	r.add_text("id", std::to_string(s->sequence).c_str());
	r.add_text("command", s->command.c_str());
	s->delivered = true;

	return encode(r);
}
std::string browser_debug_tool(const std::string &owner,
    const std::string &project, const std::string &run, const std::string &name,
    const std::string &selector, const std::string &content)
{
	static const std::set<std::string> allowed = { "browser.snapshot",
		"browser.inspect", "browser.overlays", "browser.interact",
		"browser.patch_style" };
	if (!ai_runtime_policy_get().allow_browser_debug)
		return error("Browser debugging disabled by policy");
	if (name == "browser.status")
		return browser_debug_status(owner, project);
	if (!allowed.count(name) || selector.size() > 300 ||
	    content.size() > 4096)
		return error("Invalid browser command");
	std::shared_ptr<session_t> s;
	{
		std::lock_guard<std::mutex> lock(guard);
		s = find(owner, project);
		if (!ai_runtime_policy_get().allow_browser_debug || !s ||
		    !alive(*s))
			return error(
			    "Browser not connected: ask user to pair the affected tab using Browser Debug");
		if (!s->run.empty() && s->run != run)
			return error(
			    "Browser session belongs to another AI run; pair again");
		if (!s->command.empty())
			return error("Another browser command is pending");
		s->run = run;
		s->image.clear();
		acl::json j;
		auto &r = j.create_node();
		r.add_text("name", name.c_str());
		r.add_text("selector", selector.c_str());
		r.add_text("content", content.c_str());
		s->command = encode(r);
		++s->sequence;
		s->delivered = false;
		s->result.clear();
	}
	for (int n = 0; n < 240; ++n) {
		{
			std::lock_guard<std::mutex> lock(guard);
			if (!alive(*s)) {
				s->command.clear();
				return error(
				    "Browser disconnected during command; outcome may be unknown");
			}
			if (!s->result.empty()) {
				auto result = s->result;
				s->command.clear();
				s->result.clear();
				return result;
			}
		}
		acl::fiber::delay(50);
	}
	// Never retry a possibly delivered click; revoke so a late response cannot be reused.
	{
		std::lock_guard<std::mutex> lock(guard);
		s->revoked = true;
		s->command.clear();
		s->image.clear();
	}
	return error(
	    "Browser command timed out; session revoked, action outcome unknown. Pair again before continuing");
}
std::string browser_debug_evidence_id(const std::string &owner,
    const std::string &project, const std::string &run)
{
	std::lock_guard<std::mutex> lock(guard);
	auto s = find(owner, project);
	return ai_runtime_policy_get().allow_browser_debug && s && alive(*s) &&
	        (s->run.empty() || s->run == run) ?
	    s->evidence_id :
	    "";
}
std::string browser_debug_take_image(const std::string &owner,
    const std::string &project, const std::string &run)
{
	std::lock_guard<std::mutex> lock(guard);
	auto s = find(owner, project);
	std::string image;
	if (!(s && alive(*s) && s->run == run))
		return image;
	image.swap(s->image);
	return image;
}
}
}
