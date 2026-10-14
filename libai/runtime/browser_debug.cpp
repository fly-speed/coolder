#include "stdafx.h"
#include "browser_debug.h"
#include "../agent/ai_admin_policy.h"
#include "../sandbox/program_sandbox.h"
#include "../workspace/agent_workspace.h"
#include "../sdk/json_guard.h"
#include "../sdk/file_execution_store.h"
#include "../common/file_ops.h"
#include "../common/json_value.h"
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <set>
namespace libai
{
namespace ai
{
namespace
{
using clock_type = std::chrono::steady_clock;
struct session_t {
	struct execution_t { std::string fingerprint, result; bool finished=false; };
	std::map<std::string, execution_t> executions;
	std::string execution_key;
	size_t result_bytes=0;
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
#if defined(__unix__) || defined(__APPLE__)
static void validate_browser_receipt(const std::string &wire,const std::string &key)
{
    namespace json=libai::governance_detail::json;
    json::Document record;libai::governance_detail::parse_json_bounded(wire,record,16384);
    if(!record.IsObject()||(record.MemberCount()!=11&&record.MemberCount()!=12)||!record.HasMember("version")||!record["version"].IsInt()||record["version"].GetInt()!=1 ||
        !record.HasMember("callback_ok")||!record["callback_ok"].IsBool())throw std::invalid_argument("invalid browser receipt");
    for(const auto *field:{"key","project_path","run_id","command_name","evidence_id","request_sha256","state","output_sha256","image_sha256"})
        if(!record.HasMember(field)||!record[field].IsString()||
            std::string(record[field].GetString(),record[field].GetStringLength()).find('\0')!=std::string::npos)
            throw std::invalid_argument("invalid browser receipt field");
    auto text=[&](const char *field){return std::string(record[field].GetString(),record[field].GetStringLength());};
    auto hash=[](const std::string &value){return value.size()==64&&value.find_first_not_of("0123456789abcdef")==std::string::npos;};
    if(text("key")!=key||!hash(text("request_sha256"))||text("evidence_id").empty()||text("command_name").compare(0,8,"browser.")!=0)throw std::invalid_argument("invalid browser receipt identity");
    const auto state=text("state");
    const bool confirmed=state=="confirmed_none"||state=="confirmed_applied";
    if(confirmed) {
        if(record.MemberCount()!=12||!record.HasMember("reference")||!record["reference"].IsString()||
            record["reference"].GetStringLength()==0||record["reference"].GetStringLength()>2048||
            std::string(record["reference"].GetString(),record["reference"].GetStringLength()).find('\0')!=std::string::npos)
            throw std::invalid_argument("invalid browser host reference");
    } else if(record.MemberCount()!=11)throw std::invalid_argument("unexpected browser host reference");
    if(state=="running"||state=="confirmed_none") {
        if(!text("output_sha256").empty()||!text("image_sha256").empty()||record["callback_ok"].GetBool())
            throw std::invalid_argument("invalid browser running receipt");
    } else if((state!="returned"&&state!="confirmed_applied")||!hash(text("output_sha256")) ||
        (!text("image_sha256").empty()&&!hash(text("image_sha256"))))throw std::invalid_argument("invalid browser result receipt");
}
class browser_receipt_t {
    std::unique_ptr<libai::file_execution_store_t> store_;
    std::string scope_,revision_,key_,project_,name_,wire_;
public:
    browser_receipt_t(const std::string &owner,const std::string &project,const std::string &key,const std::string &name):key_(key),project_(project),name_(name) {
        if(key.empty())return;
        const auto directory=owner+"/.libai_agent/browser_commands";
        if(!file_ops::make_private_directory(owner+"/.libai_agent")||!file_ops::make_private_directory(directory))
            throw std::runtime_error("cannot prepare browser receipts");
        store_.reset(new libai::file_execution_store_t(directory,libai::execution_file_codec_t::plaintext()));
        scope_="browser_command_"+agent_workspace_t::content_sha256(key);
        const auto snapshot=store_->open(scope_);revision_=snapshot.revision;wire_=snapshot.state_json;
        if(!wire_.empty())validate_browser_receipt(wire_,key);
    }
    bool exists()const{return !wire_.empty();}
    bool matches(const std::string &fingerprint,const std::string &result="")const {
        if(wire_.empty())return false;
        acl::json record(wire_.c_str());record.finish();
        return text(record["project_path"])==project_&&text(record["request_sha256"])==fingerprint &&
            (result.empty() || (text(record["state"])=="returned"&&text(record["output_sha256"])==agent_workspace_t::content_sha256(result)));
    }
    void save(const std::string &run,const std::string &evidence,const std::string &fingerprint,
        const std::string &result="",const std::string &image="") {
        if(!store_)return;
        acl::json json;auto &record=json.create_node();
        record.add_number("version",1).add_text("key",key_.c_str()).add_text("project_path",project_.c_str())
            .add_text("run_id",run.c_str()).add_text("command_name",name_.c_str()).add_text("evidence_id",evidence.c_str()).add_text("request_sha256",fingerprint.c_str())
            .add_text("state",result.empty()?"running":"returned")
            .add_text("output_sha256",result.empty()?"":agent_workspace_t::content_sha256(result).c_str())
            .add_text("image_sha256",image.empty()?"":agent_workspace_t::content_sha256(image).c_str());
        acl::json response(result.c_str());
        record.add_bool("callback_ok",!result.empty()&&response.finish()&&json_value::text_boolean(response["ok"],false));
        const auto wire=encode(record);validate_browser_receipt(wire,key_);
        revision_=store_->commit(scope_,revision_,wire);wire_=wire;
    }
};
#else
class browser_receipt_t {
public:
    browser_receipt_t(const std::string &,const std::string &,const std::string &key,const std::string &) {
        if(!key.empty())throw std::runtime_error("durable browser receipts require POSIX storage");
    }
    bool exists()const{return false;} bool matches(const std::string &,const std::string & = "")const{return false;}
    void save(const std::string &,const std::string &,const std::string &,const std::string & = "",const std::string & = ""){}
};
#endif

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
		if (in["idempotency_key"] &&
		    (!in["idempotency_key"]->is_string() ||
		        text(in["idempotency_key"]) != s->execution_key))
			return error("Browser execution key mismatch");
		const auto result = text(in["result"]);
		if (result.empty() || result.size() > 48 * 1024)
			return error("Invalid browser evidence size");
		try {
			libai_schema_json::Document strict;
			libai::governance_detail::parse_json_bounded(result,strict,48*1024);
			if(!strict.IsObject() || strict.HasMember("browser_execution_reused"))
				return error("Invalid or reserved browser result metadata");
			if(strict.HasMember("idempotency_key") &&
			    (!strict["idempotency_key"].IsString() ||
			        std::string(strict["idempotency_key"].GetString(),strict["idempotency_key"].GetStringLength())!=s->execution_key))
				return error("Browser result execution key mismatch");
		} catch(const std::exception &) {
			return error("Invalid browser result JSON");
		}
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
bool browser_debug_inspect(const std::string &owner,const std::string &key,acl::json_node &out,std::string &reason)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        if(key.empty()||!program_sandbox_t::valid_idempotency_key(key))throw std::invalid_argument("invalid browser inspection key");
        const auto directory=owner+"/.libai_agent/browser_commands";
        struct stat info;
        if(stat(directory.c_str(),&info)!=0) {
            if(errno==ENOENT){out.add_bool("found",false);return true;}
            throw std::runtime_error("cannot inspect browser receipt directory");
        }
        libai::file_execution_store_t store(directory,libai::execution_file_codec_t::plaintext());
        const auto snapshot=store.open("browser_command_"+agent_workspace_t::content_sha256(key));
        out.add_bool("found",!snapshot.state_json.empty());
        if(snapshot.state_json.empty())return true;
        validate_browser_receipt(snapshot.state_json,key);
        acl::json record(snapshot.state_json.c_str());record.finish();
        out.add_text("revision",snapshot.revision.c_str()).add_text("receipt_json",snapshot.state_json.c_str())
            .add_text("recovery_policy",text(record["state"])=="running"?"host_reconciliation":"inspect_parent_before_resume");
        return true;
    } catch(const std::exception &exception){reason=exception.what();return false;}
#else
    reason="browser receipts require POSIX storage";return false;
#endif
}

bool browser_debug_lock_parent_receipts(const std::string &owner,const std::string &project,
    const std::string &key,const std::string &run,const std::string &operation,const std::string &outcome,
    std::vector<std::shared_ptr<libai::execution_store_t>> &leases,std::string &reason)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        const auto directory=owner+"/.libai_agent/browser_commands";
        struct stat info;
        if(stat(directory.c_str(),&info)!=0) {
            if(errno==ENOENT)return true; // Legacy parent: do not fabricate child evidence.
            throw std::runtime_error("cannot inspect browser child receipts");
        }
        for(const auto *suffix:{"",".undo-previous",".baseline.inspect",".baseline.snapshot",
                ".applied.inspect",".applied.snapshot",".restored.inspect",".restored.snapshot"}) {
            const auto child_key=key+suffix;
            if(!program_sandbox_t::valid_idempotency_key(child_key))throw std::invalid_argument("invalid browser child key");
            auto lease=std::make_shared<libai::file_execution_store_t>(directory,libai::execution_file_codec_t::plaintext());
            const auto snapshot=lease->open("browser_command_"+agent_workspace_t::content_sha256(child_key));
            leases.push_back(lease); // Missing children stay locked too, preventing creation during commit.
            if(snapshot.state_json.empty())continue;
            validate_browser_receipt(snapshot.state_json,child_key);
            acl::json record(snapshot.state_json.c_str());record.finish();
            const auto state=text(record["state"]),command=text(record["command_name"]);
            const std::string step=suffix;
            const auto expected=step.empty()?operation:step==".undo-previous"?std::string("browser.patch_style"):
                step.find(".inspect")!=std::string::npos?std::string("browser.inspect"):std::string("browser.snapshot");
            if(text(record["project_path"])!=project||text(record["run_id"])!=run||command!=expected||state=="running")
                throw std::runtime_error("browser child receipt requires reconciliation: "+child_key);
            const bool mutating=command=="browser.interact"||command=="browser.patch_style";
            if(outcome=="none"&&mutating&&state!="confirmed_none")
                throw std::runtime_error("none parent receipt conflicts with browser child effects: "+child_key);
        }
        return true;
    }catch(const std::exception &exception){reason=exception.what();return false;}
#else
    reason="browser child receipt checks require POSIX storage";return false;
#endif
}

bool browser_debug_confirm(const std::string &owner,const std::string &arguments,acl::json_node &out,std::string &reason)
{
#if defined(__unix__) || defined(__APPLE__)
    try {
        namespace json=libai::governance_detail::json;
        json::Document input;libai::governance_detail::parse_json_bounded(arguments,input,16384);
        if(!input.IsObject()||input.MemberCount()!=5||!input.HasMember("result")||!input["result"].IsObject())
            throw std::invalid_argument("browser confirmation requires five host fields");
        auto value=[&](const char *name){
            if(!input.HasMember(name)||!input[name].IsString())throw std::invalid_argument("invalid browser confirmation field");
            std::string text(input[name].GetString(),input[name].GetStringLength());
            if(text.find('\0')!=std::string::npos)throw std::invalid_argument("NUL in browser confirmation");return text;
        };
        const auto key=value("idempotency_key"),revision=value("revision"),outcome=value("outcome"),reference=value("reference");
        if(key.empty()||!program_sandbox_t::valid_idempotency_key(key)||revision.empty()||
            (outcome!="none"&&outcome!="applied")||reference.empty()||reference.size()>2048)
            throw std::invalid_argument("invalid browser confirmation identity");
        const auto &result=input["result"];
        auto hash=[](const json::Value &v){if(!v.IsString())return false;std::string s(v.GetString(),v.GetStringLength());return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;};
        if(outcome=="none") {
            if(result.MemberCount()!=0)throw std::invalid_argument("none browser confirmation requires empty result");
        } else if(result.MemberCount()!=3||!result.HasMember("output_sha256")||!hash(result["output_sha256"])||
            !result.HasMember("image_sha256")||!result["image_sha256"].IsString()||
            (result["image_sha256"].GetStringLength()!=0&&!hash(result["image_sha256"]))||
            !result.HasMember("callback_ok")||!result["callback_ok"].IsBool())
            throw std::invalid_argument("invalid browser confirmation result");
        libai::file_execution_store_t store(owner+"/.libai_agent/browser_commands",libai::execution_file_codec_t::plaintext());
        const auto scope="browser_command_"+agent_workspace_t::content_sha256(key);const auto snapshot=store.open(scope);
        if(snapshot.state_json.empty()||snapshot.revision!=revision)throw std::runtime_error("browser receipt revision conflict");
        validate_browser_receipt(snapshot.state_json,key);
        json::Document record;libai::governance_detail::parse_json_bounded(snapshot.state_json,record,16384);
        if(std::string(record["state"].GetString())!="running")throw std::runtime_error("browser receipt is already confirmed");
        auto &allocator=record.GetAllocator();
        record["state"].SetString(outcome=="none"?"confirmed_none":"confirmed_applied",allocator);
        record.AddMember("reference",json::Value(reference.c_str(),static_cast<json::SizeType>(reference.size()),allocator),allocator);
        if(outcome=="applied")for(const auto *field:{"output_sha256","image_sha256","callback_ok"})record[field].CopyFrom(result[field],allocator);
        json::StringBuffer buffer;json::Writer<json::StringBuffer> writer(buffer);record.Accept(writer);
        const std::string wire(buffer.GetString(),buffer.GetSize());validate_browser_receipt(wire,key);
        const auto next=store.commit(scope,revision,wire);
        out.add_text("revision",next.c_str()).add_text("state",record["state"].GetString());return true;
    } catch(const std::exception &exception){reason=exception.what();return false;}
#else
    reason="browser confirmation requires POSIX storage";return false;
#endif
}

std::string browser_debug_tool(const std::string &owner,
    const std::string &project, const std::string &run, const std::string &name,
    const std::string &selector, const std::string &content,
    const std::string &idempotency_key)
{
	if (!program_sandbox_t::valid_idempotency_key(idempotency_key))
		return error("Invalid browser execution key");
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
    std::unique_ptr<browser_receipt_t> durable;
    try {durable.reset(new browser_receipt_t(owner,project,idempotency_key,name));}
    catch(const std::exception &exception){return error(exception.what());}
    std::string fingerprint;
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
		acl::json j;
		auto &r = j.create_node();
		r.add_text("name", name.c_str());
		r.add_text("selector", selector.c_str());
		r.add_text("content", content.c_str());
		fingerprint=agent_workspace_t::content_sha256(encode(r));
        if(durable->exists()&&!durable->matches(fingerprint))return error("Browser persistent execution identity changed");
		if (!idempotency_key.empty()) {
			auto previous=s->executions.find(idempotency_key);
			if(previous!=s->executions.end()) {
				if(previous->second.fingerprint!=fingerprint)
					return error("Browser execution key arguments changed");
				if(!previous->second.finished)
					return error("Browser execution is unconfirmed; do not replay");
				if(!s->command.empty())
					return error("Another browser command is pending");
				if(previous->second.result.empty())
					return error("Browser execution receipt is unavailable; host inspection required");
                if(!durable->matches(fingerprint,previous->second.result))return error("Browser persistent receipt is unconfirmed; do not replay");
				acl::json cached(previous->second.result.c_str());
				if(!cached.finish())return error("Invalid browser execution receipt");
				// Cached JSON is historical evidence, never a new screenshot.
				s->image.clear();
				cached.get_root().add_bool("browser_execution_reused",true);
				return encode(cached.get_root());
			}
            if(durable->exists())return error("Browser execution already has a persistent receipt; host inspection required, do not replay");
			if(s->executions.size()>=256)
				return error("Browser execution ledger limit reached; pair again");
		}
		if (!s->command.empty())
			return error("Another browser command is pending");
        try {durable->save(run,s->evidence_id,fingerprint);}
        catch(const std::exception &exception){return error(exception.what());}
		if(!idempotency_key.empty()) {
			session_t::execution_t entry;entry.fingerprint=fingerprint;
			s->executions.emplace(idempotency_key,entry);
			r.add_text("idempotency_key",idempotency_key.c_str());
		}
		s->execution_key=idempotency_key;
		s->run = run;
		s->image.clear();
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
				if(!idempotency_key.empty()) {
					acl::json receipt(result.c_str());
					if(!receipt.finish() || (receipt["idempotency_key"] &&
					    (!receipt["idempotency_key"]->is_string() || text(receipt["idempotency_key"])!=idempotency_key))) {
						s->revoked=true;s->command.clear();s->result.clear();
						return error("Browser result execution key mismatch; outcome unknown");
					}
					if(!receipt["idempotency_key"])
						receipt.get_root().add_text("idempotency_key",idempotency_key.c_str());
					result=encode(receipt.get_root());
                    try {durable->save(run,s->evidence_id,fingerprint,result,s->image);}
                    catch(const std::exception &exception){
                        s->revoked=true;s->command.clear();s->result.clear();s->image.clear();
                        return error((std::string("Browser result persistence failed; outcome unknown: ")+exception.what()).c_str());
                    }
					auto &entry=s->executions.at(idempotency_key);entry.finished=true;
					if(s->result_bytes+result.size()<=512*1024) {
						entry.result=result;s->result_bytes+=result.size();
					}
				}
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
