#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/json_value.h"
#include "../workspace/agent_change_limits.h"
#include "agent_progress_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace webcool
{
namespace ai
{
namespace
{

webcool::mutex g_progress_mutex;
const size_t kMaxTranscriptBytes = kMaxAgentTranscriptBytes;
const size_t kMaxReasoningBytes = 1024 * 1024;
const size_t kMaxProgressFileBytes = 4 * 1024 * 1024;
// This is the absolute administrator-policy ceiling, not the active per-run
// setting. Keeping recovery files valid up to the ceiling lets an administrator
// lower and later restore the configured limit without corrupting checkpoints.
const size_t kMaxCompletedToolCalls = 256;

using ::webcool::ai::file_ops::join_path;

bool valid_session_id(const std::string &id)
{
	return id.empty() || ::webcool::ai::identifiers::valid_id(id);
}

std::string progress_name(const std::string &session_id)
{
	return std::string("progress-") +
	    (session_id.empty() ? "single" : session_id) + ".json";
}

bool normal_directory(const std::string &path)
{
	return ::webcool::ai::file_ops::safe_directory(path);
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
	directory = join_path(project, ".webcool_agent");
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), wide)) {
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

using ::webcool::ai::file_ops::replace_file;

std::string json_text(acl::json_node *node)
{
	return ::webcool::ai::json_value::string_text(node);
}

long long json_number(acl::json_node *node)
{
	return ::webcool::ai::json_value::number(node);
}

bool json_boolean(acl::json_node *node)
{
	return ::webcool::ai::json_value::boolean(node);
}

bool safe_progress(const agent_progress_t &progress)
{
	if (!progress.source_run_id.empty() &&
	    !valid_session_id(progress.source_run_id))
		return false;
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
	root.add_text("source_run_id", progress.source_run_id.c_str());
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
	return join_path(join_path(project_path_, ".webcool_agent"),
	    progress_name(session_id_));
}

bool agent_progress_store_t::save(
    const agent_progress_t &progress, std::string &err) const
{
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
	std::lock_guard<webcool::mutex> guard(g_progress_mutex);
	std::string directory;
	if (!ensure_progress_directory(project, directory, err)) {
		return ai_error("agent.progress", "prepare-directory", err);
	}
	const std::string target =
	    join_path(directory, progress_name(session_id_));
	const std::string temporary = target + ".tmp";
	const std::string content = serialize(progress);
	if (content.size() > kMaxProgressFileBytes) {
		err =
		    "serialized coding-agent progress exceeds the recovery file limit";
		return ai_error("agent.progress", "validate-file-size", err);
	}
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
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		::remove(temporary.c_str());
		err = "cannot protect coding-agent progress checkpoint";
		return ai_error("agent.progress", "protect", err);
	}
#endif
	if (replace_file(temporary, target))
		return true;
	::remove(temporary.c_str());
	err = std::string("cannot install coding-agent progress checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.progress", "install", err);
}

bool agent_progress_store_t::load(
    agent_progress_t &progress, bool &found, std::string &err) const
{
	found = false;
	if (!valid_session_id(session_id_)) {
		err = "invalid coding-agent progress session id";
		return ai_error("agent.progress", "validate-load", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.progress", "resolve-project", err);
	}
	const std::string path = join_path(
	    join_path(project, ".webcool_agent"), progress_name(session_id_));
	std::string content;
	{
		std::lock_guard<webcool::mutex> guard(g_progress_mutex);
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
	    join_path(project, ".webcool_agent"), progress_name(session_id_));
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
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
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error(
		    "agent.progress", "resolve-remove-project", err);
	}
	const std::string path = join_path(
	    join_path(project, ".webcool_agent"), progress_name(session_id_));
	std::lock_guard<webcool::mutex> guard(g_progress_mutex);
	if (!(::remove(path.c_str()) != 0 && errno != ENOENT))
		return true;
	err = std::string("cannot remove coding-agent progress checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.progress", "remove", err);
}

} // namespace ai
} // namespace webcool
