#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/json_value.h"
#include "../workspace/agent_change_limits.h"
#include "agent_progress_store.h"
#include "durable_record_writer.h"
#include "../sdk/json_guard.h"
#include "../sdk/file_execution_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../common/native_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sys/stat.h>

namespace libai
{
namespace ai
{
namespace
{

libai::mutex g_progress_mutex;
const size_t kMaxTranscriptBytes = kMaxAgentTranscriptBytes;
const size_t kMaxReasoningBytes = 1024 * 1024;
const size_t kMaxProgressFileBytes = 4 * 1024 * 1024;
// This is the absolute administrator-policy ceiling, not the active per-run
// setting. Keeping recovery files valid up to the ceiling lets an administrator
// lower and later restore the configured limit without corrupting checkpoints.
const size_t kMaxCompletedToolCalls = 256;

using ::libai::ai::file_ops::join_path;

bool valid_session_id(const std::string &id)
{
	return id.empty() || ::libai::ai::identifiers::valid_id(id);
}

std::string progress_name(const std::string &session_id)
{
	return std::string("progress-") +
	    (session_id.empty() ? "single" : session_id) + ".json";
}

bool normal_directory(const std::string &path)
{
	return ::libai::ai::file_ops::safe_directory(path);
}

bool resolve_project(const std::string &user_root,
    const std::string &project_path, std::string &absolute, std::string &err)
{
	return agent_workspace_t::resolve_project_state_root(
	           user_root, project_path, absolute, err) &&
	    normal_directory(absolute);
}

bool ensure_progress_directory(
    const std::string &project, std::string &directory, std::string &err)
{
	directory = join_path(project, ".libai_agent");
#ifdef _WIN32
	std::wstring wide;
	if (!libai_utf8_path_to_wide(directory.c_str(), wide)) {
		err = "cannot encode agent progress directory";
		return false;
	}
	if (GetFileAttributesW(wide.c_str()) == INVALID_FILE_ATTRIBUTES &&
	    _wmkdir(wide.c_str()) != 0)
#else
	struct stat st;
	if (lstat(directory.c_str(), &st) != 0 &&
	    (errno != ENOENT || mkdir(directory.c_str(), 0700) != 0))
#endif
	{
		err = std::string("cannot create agent progress directory: ") +
		    strerror(errno);
		return false;
	}
	if (!normal_directory(directory)) {
		err = "agent progress path is not a safe directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect agent progress directory";
		return false;
	}
#endif
	return true;
}

using ::libai::ai::file_ops::replace_file;

std::string json_text(acl::json_node *node)
{
	return ::libai::ai::json_value::string_text(node);
}

long long json_number(acl::json_node *node)
{
	return ::libai::ai::json_value::number(node);
}

bool json_boolean(acl::json_node *node)
{
	return ::libai::ai::json_value::boolean(node);
}

bool safe_progress(const agent_progress_t &progress)
{
    if(!progress.execution_revision.empty()) {
        if(progress.execution_revision.size()!=64)return false;
        for(char c:progress.execution_revision)if(record_codec::hex_value(c)<0)return false;
    }
    if(progress.tool_effects.size()>256)return false;
    bool running=false;
    for(size_t i=0;i<progress.tool_effects.size();++i) {
        const auto &effect=progress.tool_effects[i];
        const auto dot=effect.key.find('.');
        auto digest=[](const std::string &value){if(value.size()!=64)return false;for(char c:value)if(record_codec::hex_value(c)<0)return false;return true;};
        if(dot!=32 || !identifiers::valid_id(effect.key.substr(0,dot)) || effect.key.substr(dot+1)!=std::to_string(i) ||
            effect.name.empty() || effect.name.size()>128 || effect.name.find('\0')!=std::string::npos || !digest(effect.arguments_sha256) ||
            effect.completed_before>progress.completed_tool_calls || running)return false;
        if(effect.tool_call_count>256 || effect.native_call_ids.size()>effect.tool_call_count ||
            effect.response_id.size()>512 || effect.response_id.find('\0')!=std::string::npos)return false;
        std::set<std::string> ids;
        for(const auto &id:effect.native_call_ids)
            if(id.empty() || id.size()>512 || id.find('\0')!=std::string::npos || !ids.insert(id).second)return false;
        if(!effect.native_call_ids.empty() && effect.native_call_ids.size()!=effect.tool_call_count)return false;
        if(!effect.response_id.empty() && effect.native_call_ids.empty())return false;
        if(effect.state=="running") {
            running=true;
            if(i+1!=progress.tool_effects.size() || !effect.reference.empty() || effect.completed_after!=0)return false;
        } else if(effect.state=="done") {
            if(!digest(effect.output_sha256) || effect.reference!="progress:"+effect.key ||
                effect.completed_after<effect.completed_before || effect.completed_after>progress.completed_tool_calls)return false;
        } else if(effect.state=="confirmed_none" || effect.state=="confirmed_applied") {
            if(!digest(effect.output_sha256) || effect.reference.empty() || effect.reference.size()>2048 ||
                effect.reference.find('\0')!=std::string::npos || effect.tool_call_count==0 ||
                effect.completed_after<effect.completed_before+effect.tool_call_count ||
                effect.completed_after>progress.completed_tool_calls)return false;
        } else return false;
    }
	if (!progress.source_run_id.empty() &&
	    !valid_session_id(progress.source_run_id))
		return false;
	const auto &receipt=progress.validation_receipt;
	if(!receipt.effect_key.empty()) {
		if(receipt.execution_revision.empty() || receipt.execution_revision!=progress.execution_revision ||
		    receipt.source_sha256.size()!=64 || receipt.proposal_sha256.size()!=64 || receipt.report_json.size()>512*1024)return false;
		for(const auto *hash:{&receipt.source_sha256,&receipt.proposal_sha256})for(char c:*hash)if(record_codec::hex_value(c)<0)return false;
		const agent_tool_effect_t *owner=nullptr;
		for(const auto &effect:progress.tool_effects)if(effect.key==receipt.effect_key)owner=&effect;
		if(!owner || (owner->state!="done"&&owner->state!="confirmed_applied") ||
		    owner->output_sha256!=agent_workspace_t::content_sha256(receipt.report_json))return false;
		if(owner->name!="coding.final_validation" && owner->name!="coding.recovery_validation" &&
		    owner->name!="coding.budget_validation" && owner->name!="coding.admission_validation" &&
		    owner->name!="coding.repair_full_validation")return false;
		try {
			namespace json=libai::governance_detail::json;
			json::Document report;libai::governance_detail::parse_json_bounded(receipt.report_json,report,512*1024);
			if(!report.IsObject() || !report.HasMember("ok") || !report["ok"].IsBool() || !report["ok"].GetBool())return false;
			for(const auto &pair:std::vector<std::pair<std::string,std::string>>{{"source_baseline_sha256",receipt.source_sha256},{"proposal_sha256",receipt.proposal_sha256}}) {
				if(!report.HasMember(pair.first.c_str()) || !report[pair.first.c_str()].IsString() ||
				    std::string(report[pair.first.c_str()].GetString(),report[pair.first.c_str()].GetStringLength())!=pair.second)return false;
			}
		}catch(const std::exception &){return false;}
	} else if(!receipt.execution_revision.empty() || !receipt.source_sha256.empty() || !receipt.proposal_sha256.empty() || !receipt.report_json.empty())return false;
	if (!(!progress.provider_id.empty() &&
	        progress.provider_id.size() <= 64 &&
	        progress.provider_model.size() <= 200 &&
	        progress.project_path.size() <= 2048 &&
	        valid_session_id(progress.session_id) &&
	        !progress.original_prompt.empty() &&
	        progress.original_prompt.size() <= 32 * 1024 &&
	        progress.transcript.size() <= kMaxTranscriptBytes &&
	        progress.reasoning.size() <= kMaxReasoningBytes &&
	        progress.last_error.size() <= 16 * 1024 &&
	        progress.completed_tool_calls <= kMaxCompletedToolCalls &&
	        progress.staged_changes.size() <= kMaxAgentChanges &&
	        progress.provider_response_id.size() <= 256 &&
	        progress.provider_tool_outputs.size() <= 32))
		return false;
	if ((progress.provider_response_pending ||
	        !progress.provider_tool_outputs.empty()) &&
	    progress.provider_response_id.empty())
		return false;
	size_t provider_output_bytes = 0;
	for (size_t i = 0; i < progress.provider_tool_outputs.size(); ++i) {
		if (progress.provider_tool_outputs[i].call_id.empty() ||
		    progress.provider_tool_outputs[i].call_id.size() > 256 ||
		    progress.provider_tool_outputs[i].output.find('\0') !=
		        std::string::npos)
			return false;
		provider_output_bytes +=
		    progress.provider_tool_outputs[i].output.size();
		if (!(provider_output_bytes > 512 * 1024))
			continue;
		return false;
	}
	size_t staged_bytes = 0;
	for (size_t i = 0; i < progress.staged_changes.size(); ++i) {
		const agent_change_proposal_t &change =
		    progress.staged_changes[i];
		if (change.operation.size() > 64 || change.path.size() > 2048 ||
		    change.target_path.size() > 2048 ||
		    change.reason.size() > 1000 ||
		    change.base_hash.size() > 64 ||
		    change.draft_hash.size() > 64 ||
		    (change.review_status != "pending" &&
		        change.review_status != "accepted" &&
		        change.review_status != "rejected") ||
		    change.content.find('\0') != std::string::npos ||
		    change.original_content.find('\0') != std::string::npos) {
			return false;
		}
		staged_bytes +=
		    change.content.size() + change.original_content.size();
		if (!(staged_bytes > 1024 * 1024))
			continue;
		return false;
	}
	return true;
}

std::string serialize(const agent_progress_t &progress)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_number("version", 5);
	root.add_text("execution_revision",progress.execution_revision.c_str());
	auto &validation=json.create_node();root.add_child("validation_receipt",validation);
	validation.add_text("effect_key",progress.validation_receipt.effect_key.c_str())
	    .add_text("execution_revision",progress.validation_receipt.execution_revision.c_str())
	    .add_text("source_sha256",progress.validation_receipt.source_sha256.c_str())
	    .add_text("proposal_sha256",progress.validation_receipt.proposal_sha256.c_str())
	    .add_text("report_json",progress.validation_receipt.report_json.c_str());
	root.add_text("source_run_id", progress.source_run_id.c_str());
    auto &effects=json.create_array();root.add_child("tool_effects",effects);
    for(const auto &effect:progress.tool_effects) {
        auto &item=effects.add_child(false,true);
        item.add_text("key",effect.key.c_str());item.add_text("name",effect.name.c_str());
        item.add_text("arguments_sha256",effect.arguments_sha256.c_str());item.add_text("state",effect.state.c_str());
        item.add_text("output_sha256",effect.output_sha256.c_str());item.add_text("reference",effect.reference.c_str());
        item.add_number("completed_before",static_cast<long long>(effect.completed_before));
        item.add_number("completed_after",static_cast<long long>(effect.completed_after));
        item.add_number("tool_call_count",static_cast<long long>(effect.tool_call_count));
        item.add_text("response_id",effect.response_id.c_str());
        auto &ids=json.create_array();item.add_child("native_call_ids",ids);
        for(const auto &id:effect.native_call_ids)ids.add_child(json.create_array_text(id.c_str()));
    }
	root.add_text("provider_id", progress.provider_id.c_str());
	root.add_text("provider_model", progress.provider_model.c_str());
	root.add_text("project_path", progress.project_path.c_str());
	root.add_text("session_id", progress.session_id.c_str());
	root.add_text("original_prompt", progress.original_prompt.c_str());
	root.add_text("transcript", progress.transcript.c_str());
	root.add_text("reasoning", progress.reasoning.c_str());
	root.add_text("last_error", progress.last_error.c_str());
	root.add_text(
	    "provider_response_id", progress.provider_response_id.c_str());
	root.add_bool(
	    "provider_response_pending", progress.provider_response_pending);
	acl::json_node &provider_outputs = json.create_array();
	root.add_child("provider_tool_outputs", provider_outputs);
	for (size_t i = 0; i < progress.provider_tool_outputs.size(); ++i) {
		acl::json_node &item = provider_outputs.add_child(false, true);
		item.add_text("call_id",
		    progress.provider_tool_outputs[i].call_id.c_str());
		item.add_text(
		    "output", progress.provider_tool_outputs[i].output.c_str());
	}
	root.add_number("completed_tool_calls",
	    static_cast<long long>(progress.completed_tool_calls));
	root.add_number("updated_at", progress.updated_at);
	acl::json_node &staged = json.create_array();
	root.add_child("staged_changes", staged);
	for (size_t i = 0; i < progress.staged_changes.size(); ++i) {
		const agent_change_proposal_t &change =
		    progress.staged_changes[i];
		acl::json_node &item = staged.add_child(false, true);
		item.add_text("operation", change.operation.c_str());
		item.add_text("path", change.path.c_str());
		item.add_text("target_path", change.target_path.c_str());
		item.add_text("content", change.content.c_str());
		item.add_text("reason", change.reason.c_str());
		item.add_bool("creates_file", change.creates_file);
		item.add_bool("creates_directory", change.creates_directory);
		item.add_text(
		    "original_content", change.original_content.c_str());
		item.add_bool("original_content_available",
		    change.original_content_available);
		item.add_number(
		    "generation", static_cast<long long>(change.generation));
		item.add_text("base_hash", change.base_hash.c_str());
		item.add_text("draft_hash", change.draft_hash.c_str());
		item.add_text("review_status", change.review_status.c_str());
	}
	const acl::string &value = root.to_string();
	return std::string(value.c_str(), value.size());
}

#ifndef _WIN32
// Caller serializes local access; mutators additionally hold the record lease.
bool read_checkpoint_bytes(const std::string &directory, const std::string &name,
    std::string &content, bool &found, std::string &err)
{
    found=false;content.clear();
	storage_detail::record_fd_t dir(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
	if (dir.fd < 0) {
		if (errno == ENOENT) return true;
		err = "cannot open coding-agent progress directory"; return false;
	}
	struct stat metadata;
	if (::fstat(dir.fd, &metadata) || !S_ISDIR(metadata.st_mode) ||
	    metadata.st_uid != ::geteuid() || (metadata.st_mode & 0777) != 0700) {
		err = "coding-agent progress directory is not private"; return false;
	}
	storage_detail::record_fd_t file(::openat(dir.fd, name.c_str(),
	    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
	if (file.fd < 0) {
		if (errno == ENOENT) return true;
		err = "cannot open coding-agent progress checkpoint"; return false;
	}
	if (::fstat(file.fd, &metadata) || !S_ISREG(metadata.st_mode) ||
	    metadata.st_uid != ::geteuid() || (metadata.st_mode & 0777) != 0600 || metadata.st_nlink != 1) {
		err = "coding-agent progress checkpoint is not private"; return false;
	}
	char buffer[16384];
	while (content.size() <= kMaxProgressFileBytes) {
		const auto count = ::read(file.fd, buffer, sizeof(buffer));
		if (count < 0) {
			if (errno == EINTR) continue;
			err = "cannot read coding-agent progress checkpoint"; return false;
		}
		if (!count) break;
		content.append(buffer, static_cast<size_t>(count));
	}

    if(content.size()>kMaxProgressFileBytes){err="coding-agent progress checkpoint is too large";return false;}
    found=true;return true;
}
std::unique_ptr<libai::file_execution_store_t> progress_record_lease(
    const std::string &directory,const std::string &session_id)
{
    std::unique_ptr<libai::file_execution_store_t> lease(new libai::file_execution_store_t(
        directory,libai::execution_file_codec_t::plaintext()));
    if(!lease->open("coding_progress_"+(session_id.empty()?std::string("single"):session_id)).state_json.empty())
        throw std::runtime_error("unexpected progress record lease state");
    return lease;
}
#endif

} // namespace

agent_progress_store_t::agent_progress_store_t(const std::string &user_root,
    const std::string &project_path, const std::string &session_id)
        : user_root_(user_root)
        , project_path_(project_path)
        , session_id_(session_id)
{
}

std::string agent_progress_store_t::relative_path() const
{
	return join_path(join_path(project_path_, ".libai_agent"),
	    progress_name(session_id_));
}

bool agent_progress_store_t::save(
    const agent_progress_t &progress, std::string &err) const
{
    std::string revision;
    return save_impl(progress,nullptr,revision,err);
}
bool agent_progress_store_t::save_if_revision(const agent_progress_t &progress,
    const std::string &expected_revision,std::string &revision,std::string &err) const
{
    revision.clear();
    if(expected_revision.size()!=64){err="invalid checkpoint revision";return false;}
    for(char c:expected_revision)if(record_codec::hex_value(c)<0){err="invalid checkpoint revision";return false;}
    return save_impl(progress,&expected_revision,revision,err);
}
bool agent_progress_store_t::save_impl(const agent_progress_t &progress,
    const std::string *expected_revision,std::string &revision,std::string &err) const
{
    revision.clear();
#ifdef _WIN32
    if(expected_revision){err="checkpoint compare-and-save requires POSIX";return false;}
#endif
	if (!safe_progress(progress) ||
	    progress.project_path != project_path_ ||
	    progress.session_id != session_id_) {
		err = "invalid coding-agent progress checkpoint";
		return ai_error("agent.progress", "validate-save", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.progress", "resolve-project", err);
	}
	std::lock_guard<libai::mutex> guard(g_progress_mutex);
	std::string directory;
	if (!ensure_progress_directory(project, directory, err)) {
		return ai_error("agent.progress", "prepare-directory", err);
	}
#ifdef _WIN32
	const std::string target =
	    join_path(directory, progress_name(session_id_));
	const std::string temporary = target + ".tmp";
#endif
	const std::string content = serialize(progress);
	if (content.size() > kMaxProgressFileBytes) {
		err =
		    "serialized coding-agent progress exceeds the recovery file limit";
		return ai_error("agent.progress", "validate-file-size", err);
	}
#ifndef _WIN32
    std::unique_ptr<libai::file_execution_store_t> lease;
    try {lease=progress_record_lease(directory,session_id_);}
    catch(const std::exception &error){err=std::string("progress record lease unavailable: ")+error.what();return false;}
    if(expected_revision){
        std::string current;bool found=false;
        if(!read_checkpoint_bytes(directory,progress_name(session_id_),current,found,err))return false;
        if(!found || agent_workspace_t::content_sha256(current)!=*expected_revision){
            err="checkpoint revision conflict";return false;
        }
    }
	if (storage_detail::write_private_record(directory, progress_name(session_id_), content, err)) {
        revision=agent_workspace_t::content_sha256(content);
		return true;
    }
	return ai_error("agent.progress", "durable-save", err);
#else
	std::ofstream out(temporary.c_str(),
	    std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write coding-agent progress checkpoint";
		return ai_error("agent.progress", "write", err);
	}
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
	out.close();
	if (!out.good()) {
		::remove(temporary.c_str());
		err = "cannot flush coding-agent progress checkpoint";
		return ai_error("agent.progress", "flush", err);
	}
	if (replace_file(temporary, target))
		return true;
	::remove(temporary.c_str());
	err = std::string("cannot install coding-agent progress checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.progress", "install", err);
#endif
}

bool agent_progress_store_t::load(
    agent_progress_t &progress, bool &found, std::string &err) const
{
	std::string revision;
	return load_snapshot(progress, found, revision, err);
}

bool agent_progress_store_t::load_snapshot(agent_progress_t &progress,
    bool &found, std::string &revision, std::string &err) const
{
	found = false;
	revision.clear();
	if (!valid_session_id(session_id_)) {
		err = "invalid coding-agent progress session id";
		return ai_error("agent.progress", "validate-load", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.progress", "resolve-project", err);
	}
	const std::string path = join_path(
	    join_path(project, ".libai_agent"), progress_name(session_id_));
	std::string content;
	{
		std::lock_guard<libai::mutex> guard(g_progress_mutex);
#ifndef _WIN32
        bool input_found=false;
        if(!read_checkpoint_bytes(join_path(project,".libai_agent"),progress_name(session_id_),content,input_found,err))return false;
        if(!input_found)return true;
#else
		std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
		if (!in.good())
			return true;
		char buffer[16384];
		while (content.size() <= kMaxProgressFileBytes && in.good()) {
			in.read(buffer, sizeof(buffer));
			if (!(in.gcount() > 0))
				continue;
			content.append(
			    buffer, static_cast<size_t>(in.gcount()));
		}
#endif
		if (content.size() > kMaxProgressFileBytes) {
			err = "coding-agent progress checkpoint is too large";
			return ai_error("agent.progress", "read-limit", err);
		}
	}
	acl::json json(content.c_str());
	if (!json.finish() ||
	    (json_number(json["version"]) != 1 &&
	        json_number(json["version"]) != 2 &&
	        json_number(json["version"]) != 3 &&
	        json_number(json["version"]) != 4 &&
	        json_number(json["version"]) != 5)) {
		err = "invalid coding-agent progress checkpoint JSON";
		return ai_error("agent.progress", "parse", err);
	}
	agent_progress_t parsed;
    // Validate the original bytes, including duplicate keys and integer tokens.
    try {
        namespace json=libai::governance_detail::json;
        json::Document document;libai::governance_detail::parse_json_bounded(content,document,kMaxProgressFileBytes);
        if(!document.IsObject())throw std::invalid_argument("invalid checkpoint object");
        if(document.HasMember("execution_revision")) {
            if(!document["execution_revision"].IsString())throw std::invalid_argument("invalid execution revision");
            parsed.execution_revision.assign(document["execution_revision"].GetString(),document["execution_revision"].GetStringLength());
        }
        if(document.HasMember("validation_receipt")) {
            const auto &receipt=document["validation_receipt"];
            if(!receipt.IsObject() || receipt.MemberCount()!=5)throw std::invalid_argument("invalid validation receipt");
            auto text=[&](const char *field){if(!receipt.HasMember(field)||!receipt[field].IsString())throw std::invalid_argument("invalid validation receipt field");return std::string(receipt[field].GetString(),receipt[field].GetStringLength());};
            parsed.validation_receipt.effect_key=text("effect_key");parsed.validation_receipt.execution_revision=text("execution_revision");
            parsed.validation_receipt.source_sha256=text("source_sha256");parsed.validation_receipt.proposal_sha256=text("proposal_sha256");parsed.validation_receipt.report_json=text("report_json");
        }
        if(document.HasMember("tool_effects")) {
            const auto &list=document["tool_effects"];
            if(!list.IsArray() || list.Size()>256)throw std::invalid_argument("invalid tool effect list");
            for(const auto &item:list.GetArray()) {
                if(!item.IsObject() || (item.MemberCount()!=8 && item.MemberCount()!=11))throw std::invalid_argument("invalid tool effect record");
                auto text=[&](const char *key){if(!item.HasMember(key)||!item[key].IsString())throw std::invalid_argument("invalid tool effect string");return std::string(item[key].GetString(),item[key].GetStringLength());};
                auto count=[&](const char *key){if(!item.HasMember(key)||!item[key].IsUint64()||item[key].GetUint64()>256)throw std::invalid_argument("invalid tool effect counter");return static_cast<size_t>(item[key].GetUint64());};
                agent_tool_effect_t effect;effect.key=text("key");effect.name=text("name");effect.arguments_sha256=text("arguments_sha256");
                effect.state=text("state");effect.output_sha256=text("output_sha256");effect.reference=text("reference");
                effect.completed_before=count("completed_before");effect.completed_after=count("completed_after");
                if(item.MemberCount()==11) {
                    effect.tool_call_count=count("tool_call_count");effect.response_id=text("response_id");
                    if(!item.HasMember("native_call_ids") || !item["native_call_ids"].IsArray() || item["native_call_ids"].Size()>256)
                        throw std::invalid_argument("invalid native call IDs");
                    for(const auto &id:item["native_call_ids"].GetArray()) {
                        if(!id.IsString())throw std::invalid_argument("invalid native call ID");
                        effect.native_call_ids.emplace_back(id.GetString(),id.GetStringLength());
                    }
                }
                parsed.tool_effects.push_back(effect);
            }
        }
    }catch(const std::exception &error){err=std::string("invalid coding tool effect checkpoint: ")+error.what();return ai_error("agent.progress","parse-effects",err);}
	parsed.source_run_id = json_text(json["source_run_id"]);
	parsed.provider_id = json_text(json["provider_id"]);
	parsed.provider_model = json_text(json["provider_model"]);
	parsed.project_path = json_text(json["project_path"]);
	parsed.session_id = json_text(json["session_id"]);
	parsed.original_prompt = json_text(json["original_prompt"]);
	parsed.transcript = json_text(json["transcript"]);
	parsed.reasoning = json_text(json["reasoning"]);
	parsed.last_error = json_text(json["last_error"]);
	parsed.provider_response_id = json_text(json["provider_response_id"]);
	parsed.provider_response_pending =
	    json_boolean(json["provider_response_pending"]);
	acl::json_node *provider_outputs = json["provider_tool_outputs"];
	if (provider_outputs != NULL && !provider_outputs->is_array()) {
		provider_outputs = provider_outputs->get_obj();
	}
	for (acl::json_node *item =
	         provider_outputs && provider_outputs->is_array() ?
	         provider_outputs->first_child() :
	         NULL;
	     item != NULL; item = provider_outputs->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL ||
		    parsed.provider_tool_outputs.size() >= 32) {
			err =
			    "invalid provider tool outputs in coding-agent progress";
			return ai_error("agent.progress",
			    "parse-provider-tool-outputs", err);
		}
		agent_progress_tool_output_t output;
		output.call_id = json_text((*object)["call_id"]);
		output.output = json_text((*object)["output"]);
		parsed.provider_tool_outputs.push_back(output);
	}
	parsed.completed_tool_calls =
	    static_cast<size_t>(json_number(json["completed_tool_calls"]));
	parsed.updated_at = json_number(json["updated_at"]);
	acl::json_node *staged = json["staged_changes"];
	if (staged != NULL && !staged->is_array())
		staged = staged->get_obj();
	for (acl::json_node *item =
	         staged && staged->is_array() ? staged->first_child() : NULL;
	     item != NULL; item = staged->next_child()) {
		acl::json_node *object =
		    item->is_object() ? item : item->get_obj();
		if (object == NULL ||
		    parsed.staged_changes.size() >= kMaxAgentChanges) {
			err = "invalid staged changes in coding-agent progress";
			return ai_error(
			    "agent.progress", "parse-staged-changes", err);
		}
		agent_change_proposal_t change;
		change.operation = json_text((*object)["operation"]);
		change.path = json_text((*object)["path"]);
		change.target_path = json_text((*object)["target_path"]);
		change.content = json_text((*object)["content"]);
		change.reason = json_text((*object)["reason"]);
		change.creates_file = json_boolean((*object)["creates_file"]);
		change.creates_directory =
		    json_boolean((*object)["creates_directory"]);
		change.original_content =
		    json_text((*object)["original_content"]);
		change.original_content_available =
		    json_boolean((*object)["original_content_available"]);
		change.generation = static_cast<unsigned long long>(
		    json_number((*object)["generation"]));
		change.base_hash = json_text((*object)["base_hash"]);
		change.draft_hash = json_text((*object)["draft_hash"]);
		change.review_status = json_text((*object)["review_status"]);
		if (change.review_status.empty())
			change.review_status = "pending";
		parsed.staged_changes.push_back(change);
	}
	if (!safe_progress(parsed) || parsed.project_path != project_path_ ||
	    parsed.session_id != session_id_) {
		err =
		    "coding-agent progress checkpoint does not match this project/session";
		return ai_error("agent.progress", "validate-loaded", err);
	}
	progress = parsed;
	revision = agent_workspace_t::content_sha256(content);
	found = true;
	return true;
}

bool agent_progress_store_t::exists(bool &found, std::string &err) const
{
	found = false;
	if (!valid_session_id(session_id_)) {
		err = "invalid coding-agent progress session id";
		return ai_error("agent.progress", "validate-exists", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error(
		    "agent.progress", "resolve-exists-project", err);
	}
	const std::string path = join_path(
	    join_path(project, ".libai_agent"), progress_name(session_id_));
#ifdef _WIN32
	std::wstring wide;
	if (!libai_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot encode coding-agent progress checkpoint path";
		return ai_error("agent.progress", "encode-exists", err);
	}
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD code = GetLastError();
		if (code == ERROR_FILE_NOT_FOUND ||
		    code == ERROR_PATH_NOT_FOUND)
			return true;
		err = "cannot inspect coding-agent progress checkpoint";
		return ai_error("agent.progress", "stat-exists", err);
	}
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err =
		    "coding-agent progress checkpoint is not a safe regular file";
		return ai_error("agent.progress", "validate-exists-file", err);
	}
#else
	struct stat st;
	if (lstat(path.c_str(), &st) != 0) {
		if (errno == ENOENT)
			return true;
		err = std::string(
		          "cannot inspect coding-agent progress checkpoint: ") +
		    strerror(errno);
		return ai_error("agent.progress", "stat-exists", err);
	}
	if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
		err =
		    "coding-agent progress checkpoint is not a safe regular file";
		return ai_error("agent.progress", "validate-exists-file", err);
	}
#endif
	found = true;
	return true;
}

bool agent_progress_store_t::remove(std::string &err) const
{
	if (!valid_session_id(session_id_)) { err="invalid coding-agent progress session id";return false; }
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error(
		    "agent.progress", "resolve-remove-project", err);
	}
	const std::string path = join_path(
	    join_path(project, ".libai_agent"), progress_name(session_id_));
	std::lock_guard<libai::mutex> guard(g_progress_mutex);
#ifndef _WIN32
    const auto directory=join_path(project,".libai_agent");
    struct stat metadata;
    if(::lstat(directory.c_str(),&metadata)!=0 && errno==ENOENT)return true;
    std::unique_ptr<libai::file_execution_store_t> lease;
    try {lease=progress_record_lease(directory,session_id_);}
    catch(const std::exception &error){err=std::string("progress record lease unavailable: ")+error.what();return false;}
#endif
	if (!(::remove(path.c_str()) != 0 && errno != ENOENT))
		return true;
	err = std::string("cannot remove coding-agent progress checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.progress", "remove", err);
}

} // namespace ai
} // namespace libai
