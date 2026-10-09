#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/json_value.h"
#include "../workspace/agent_change_limits.h"
#include "agent_result_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace webcool
{
namespace ai
{
namespace
{

webcool::mutex g_result_mutex;
// JSON escaping can make source text larger than its in-memory form. Keep the
// individual fields tightly bounded below while leaving enough envelope space
// to guarantee that an otherwise valid completed result can be persisted.
const size_t kMaxResultFileBytes = 16 * 1024 * 1024;
const size_t kMaxTextBytes = 2 * 1024 * 1024;
const size_t kMaxReasoningBytes = 1024 * 1024;
const size_t kMaxChangeBytes = 1024 * 1024;
const size_t kMaxTotalChangeBytes = 2 * 1024 * 1024;
const size_t kMaxDiffBytes = 2 * 1024 * 1024;
const size_t kMaxTotalDiffBytes = 4 * 1024 * 1024;
const size_t kMaxTotalOriginalBytes = 2 * 1024 * 1024;

using ::webcool::ai::file_ops::join_path;

bool valid_hex_id(const std::string &id, bool allow_empty)
{
	return id.empty() ? allow_empty :
			    ::webcool::ai::identifiers::valid_id(id);
}

bool normal_directory(const std::string &path)
{
	return ::webcool::ai::file_ops::safe_directory(path);
}

bool make_directory(const std::string &path)
{
	return ::webcool::ai::file_ops::make_private_directory(path);
}

bool resolve_project(const std::string &user_root,
		     const std::string &project_path, std::string &project,
		     std::string &err)
{
	return agent_workspace_t::resolve_project_state_root(
		       user_root, project_path, project, err) &&
	       normal_directory(project);
}

bool prepare_directory(const std::string &project, std::string &directory,
		       std::string &err)
{
	const std::string agent = join_path(project, ".webcool_agent");
	directory = join_path(agent, "results");
	if (!make_directory(agent) || !make_directory(directory)) {
		err = "cannot create coding-agent result directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(agent.c_str(), 0700) != 0 ||
	    chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect coding-agent result directory";
		return false;
	}
#endif
	return true;
}

using ::webcool::ai::file_ops::replace_file;

std::string result_name(const std::string &run_id)
{
	return "result-" + run_id + ".json";
}

std::string node_text(acl::json_node *node)
{
	return ::webcool::ai::json_value::nullable_text(node);
}

long long node_number(acl::json_node *node)
{
	return ::webcool::ai::json_value::number(node);
}

bool node_bool(acl::json_node *node)
{
	return ::webcool::ai::json_value::text_boolean(node);
}

acl::json_node *object_child(acl::json_node *node, const char *name)
{
	return ::webcool::ai::json_value::object_child(node, name);
}

acl::json_node *array_value(acl::json_node *node)
{
	return ::webcool::ai::json_value::array_value(node);
}

bool safe_result(const agent_result_t &result)
{
	if (!valid_hex_id(result.run_id, false) ||
	    !valid_hex_id(result.session_id, true) ||
	    result.project_path.size() > 2048 ||
	    result.task_contract_json.size() > 512 * 1024 ||
	    result.task_acceptance_json.size() > 1024 * 1024 ||
	    result.task_acceptance_status.size() > 64 ||
	    result.text.size() > kMaxTextBytes ||
	    result.reasoning.size() > kMaxReasoningBytes ||
	    result.completion_summary.size() > 4 * 1024 ||
	    result.changes_apply_error.size() > 4096 ||
	    result.changes.size() > kMaxAgentChanges ||
	    result.rejected_changes > kMaxAgentChanges ||
	    (result.decision != "pending" && result.decision != "accepted" &&
	     result.decision != "rejected"))
		return false;
	size_t total = 0;
	size_t total_diff = 0;
	size_t total_original = 0;
	for (size_t i = 0; i < result.changes.size(); ++i) {
		const agent_change_proposal_t &change = result.changes[i];
		total += change.content.size();
		total_diff += change.diff.size();
		total_original += change.original_content.size();
		if (change.path.empty() || change.path.size() > 2048 ||
		    change.target_path.size() > 2048 ||
		    change.reason.size() > 1000 ||
		    change.base_hash.size() > 64 ||
		    change.draft_hash.size() > 64 ||
		    (change.review_status != "pending" &&
		     change.review_status != "accepted" &&
		     change.review_status != "rejected" &&
		     change.review_status != "superseded") ||
		    change.content.size() > kMaxChangeBytes ||
		    total > kMaxTotalChangeBytes ||
		    change.diff.size() > kMaxDiffBytes ||
		    total_diff > kMaxTotalDiffBytes ||
		    change.original_content.size() > kMaxChangeBytes ||
		    total_original > kMaxTotalOriginalBytes ||
		    change.added_lines < 0 || change.removed_lines < 0 ||
		    (change.operation != "write" &&
		     change.operation != "delete" &&
		     change.operation != "move" &&
		     change.operation != "mkdir" &&
		     change.operation != "replace_empty_file_with_directory")) {
			return false;
		}
	}
	return true;
}

std::string serialize(const agent_result_t &result)
{
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_number("version", 2);
	root.add_text("run_id", result.run_id.c_str());
	root.add_text("project_path", result.project_path.c_str());
	root.add_text("session_id", result.session_id.c_str());
	root.add_text("task_contract_json", result.task_contract_json.c_str());
	root.add_text("task_acceptance_json",
		      result.task_acceptance_json.c_str());
	root.add_text("task_acceptance_status",
		      result.task_acceptance_status.c_str());
	root.add_text("text", result.text.c_str());
	root.add_text("reasoning", result.reasoning.c_str());
	root.add_text("completion_summary", result.completion_summary.c_str());
	root.add_number("rejected_changes",
			static_cast<long long>(result.rejected_changes));
	root.add_bool("changes_applied", result.changes_applied);
	root.add_number("changes_applied_at", result.changes_applied_at);
	root.add_text("changes_apply_error",
		      result.changes_apply_error.c_str());
	root.add_text("decision", result.decision.c_str());
	root.add_number("saved_at", result.saved_at);
	acl::json_node &changes = json.create_array();
	root.add_child("changes", changes);
	for (size_t i = 0; i < result.changes.size(); ++i) {
		acl::json_node &item = changes.add_child(false, true);
		item.add_text("operation", result.changes[i].operation.c_str());
		item.add_text("path", result.changes[i].path.c_str());
		item.add_text("target_path",
			      result.changes[i].target_path.c_str());
		item.add_text("content", result.changes[i].content.c_str());
		item.add_text("reason", result.changes[i].reason.c_str());
		item.add_bool("creates_file", result.changes[i].creates_file);
		item.add_bool("creates_directory",
			      result.changes[i].creates_directory);
		item.add_number("added_lines", result.changes[i].added_lines);
		item.add_number("removed_lines",
				result.changes[i].removed_lines);
		item.add_text("diff", result.changes[i].diff.c_str());
		item.add_text("original_content",
			      result.changes[i].original_content.c_str());
		item.add_bool("original_content_available",
			      result.changes[i].original_content_available);
		item.add_number(
			"generation",
			static_cast<long long>(result.changes[i].generation));
		item.add_text("base_hash", result.changes[i].base_hash.c_str());
		item.add_text("draft_hash",
			      result.changes[i].draft_hash.c_str());
		item.add_text("review_status",
			      result.changes[i].review_status.c_str());
	}
	const acl::string &value = root.to_string();
	return std::string(value.c_str(), value.size());
}

bool parse(const std::string &content, agent_result_t &result, std::string &err)
{
	acl::json json(content.c_str());
	if (!json.finish() || (node_number(json["version"]) != 1 &&
			       node_number(json["version"]) != 2)) {
		err = "invalid coding-agent result JSON";
		return false;
	}
	agent_result_t parsed;
	parsed.run_id = node_text(json["run_id"]);
	parsed.project_path = node_text(json["project_path"]);
	parsed.session_id = node_text(json["session_id"]);
	parsed.task_contract_json = node_text(json["task_contract_json"]);
	parsed.task_acceptance_json = node_text(json["task_acceptance_json"]);
	parsed.task_acceptance_status =
		node_text(json["task_acceptance_status"]);
	parsed.text = node_text(json["text"]);
	parsed.reasoning = node_text(json["reasoning"]);
	parsed.completion_summary = node_text(json["completion_summary"]);
	parsed.rejected_changes =
		static_cast<size_t>(node_number(json["rejected_changes"]));
	parsed.changes_applied = node_bool(json["changes_applied"]);
	parsed.changes_applied_at = node_number(json["changes_applied_at"]);
	parsed.changes_apply_error = node_text(json["changes_apply_error"]);
	parsed.decision = node_text(json["decision"]);
	parsed.saved_at = node_number(json["saved_at"]);
	acl::json_node *items = array_value(json["changes"]);
	if (items != NULL) {
		for (acl::json_node *item = items->first_child(); item != NULL;
		     item = items->next_child()) {
			if (parsed.changes.size() >= kMaxAgentChanges) {
				err = "coding-agent result exceeds change count limit " +
				      std::to_string(kMaxAgentChanges);
				return false;
			}
			agent_change_proposal_t change;
			change.operation =
				node_text(object_child(item, "operation"));
			change.path = node_text(object_child(item, "path"));
			change.target_path =
				node_text(object_child(item, "target_path"));
			change.content =
				node_text(object_child(item, "content"));
			change.reason = node_text(object_child(item, "reason"));
			change.creates_file =
				node_bool(object_child(item, "creates_file"));
			change.creates_directory = node_bool(
				object_child(item, "creates_directory"));
			change.added_lines =
				node_number(object_child(item, "added_lines"));
			change.removed_lines = node_number(
				object_child(item, "removed_lines"));
			change.diff = node_text(object_child(item, "diff"));
			change.original_content = node_text(
				object_child(item, "original_content"));
			change.original_content_available =
				node_bool(object_child(
					item, "original_content_available"));
			change.generation = static_cast<unsigned long long>(
				node_number(object_child(item, "generation")));
			change.base_hash =
				node_text(object_child(item, "base_hash"));
			change.draft_hash =
				node_text(object_child(item, "draft_hash"));
			change.review_status =
				node_text(object_child(item, "review_status"));
			if (change.review_status.empty())
				change.review_status = "pending";
			parsed.changes.push_back(change);
		}
	}
	if (!safe_result(parsed)) {
		err = "invalid coding-agent result content";
		return false;
	}
	result = parsed;
	return true;
}

bool save_locked(const std::string &directory, const agent_result_t &result,
		 std::string &err)
{
	const std::string target =
		join_path(directory, result_name(result.run_id));
	const std::string temporary = target + ".tmp";
	const std::string content = serialize(result);
	if (content.size() > kMaxResultFileBytes) {
		err = "coding-agent result exceeds the persistent size limit";
		return false;
	}
	// fopen is UTF-8 and long-path aware through platform_compat on Windows.
	FILE *out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = "cannot write coding-agent result";
		return false;
	}
	const bool written = fwrite(content.data(), 1, content.size(), out) ==
			     content.size();
	const bool flushed = written && fflush(out) == 0;
	const bool closed = fclose(out) == 0;
	if (!flushed || !closed) {
		unlink(temporary.c_str());
		err = "cannot flush coding-agent result";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect coding-agent result";
		return false;
	}
#endif
	if (!replace_file(temporary, target)) {
		unlink(temporary.c_str());
		err = std::string("cannot install coding-agent result: ") +
		      strerror(errno);
		return false;
	}
	return true;
}

bool read_result_file(const std::string &path, std::string &content)
{
	content.clear();
	FILE *in = fopen(path.c_str(), "rb");
	if (in == NULL)
		return false;
	bool read_ok = true;
	char buffer[16384];
	while (content.size() <= kMaxResultFileBytes) {
		const size_t count = fread(buffer, 1, sizeof(buffer), in);
		if (count > 0)
			content.append(buffer, count);
		if (count < sizeof(buffer)) {
			read_ok = ferror(in) == 0;
			break;
		}
	}
	return fclose(in) == 0 && read_ok;
}

bool load_locked(const std::string &directory, const std::string &run_id,
		 agent_result_t &result)
{
	const std::string path = join_path(directory, result_name(run_id));
	std::string content;
	if (!read_result_file(path, content))
		return false;
	std::string parse_err;
	return content.size() <= kMaxResultFileBytes &&
	       parse(content, result, parse_err) && result.run_id == run_id;
}

// Scan regular result artifacts only; never follow linked result files.
bool result_names(const std::string &directory, std::vector<std::string> &names,
		  std::string &err)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), wide))
		return false;
	WIN32_FIND_DATAW data;
	HANDLE search =
		FindFirstFileW((wide + L"\\result-*.json").c_str(), &data);
	if (search == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	do {
		std::string name;
		if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
					      FILE_ATTRIBUTE_REPARSE_POINT)) ==
			    0 &&
		    webcool_wide_to_utf8(data.cFileName, name))
			names.push_back(name);
	} while (FindNextFileW(search, &data));
	const DWORD scan_error = GetLastError();
	FindClose(search);
	if (scan_error != ERROR_NO_MORE_FILES) {
		err = "cannot list saved results";
		return false;
	}
#else
	DIR *handle = opendir(directory.c_str());
	if (!handle) {
		err = "cannot list saved results";
		return false;
	}
	for (;;) {
		errno = 0;
		dirent *entry = readdir(handle);
		if (!entry) {
			const int scan_error = errno;
			closedir(handle);
			if (scan_error) {
				err = "cannot list saved results";
				return false;
			}
			break;
		}
		struct stat st;
		if (lstat(join_path(directory, entry->d_name).c_str(), &st) ==
			    0 &&
		    S_ISREG(st.st_mode))
			names.push_back(entry->d_name);
	}
#endif
	return true;
}

// A continued run can replace an earlier draft while retaining its baseline.
// Only an accepted, applied later generation in this same session supersedes
// it. Different sessions, baselines and unresolved drafts remain reviewable.
void mark_superseded_reviews(const std::string &directory,
			     agent_result_t &result)
{
	if (result.session_id.empty())
		return;
	std::vector<std::string> names;
	std::string err;
	if (!result_names(directory, names, err))
		return;
	for (const auto &name : names) {
		if (name.size() != 44 || name.compare(0, 7, "result-") != 0 ||
		    name.substr(39) != ".json")
			continue;
		const std::string id = name.substr(7, 32);
		if (id == result.run_id || !valid_hex_id(id, false))
			continue;
		agent_result_t newer;
		if (!load_locked(directory, id, newer) ||
		    newer.session_id != result.session_id ||
		    newer.project_path != result.project_path ||
		    newer.saved_at <= result.saved_at || !newer.changes_applied)
			continue;
		for (auto &old : result.changes) {
			if (old.review_status != "pending" ||
			    old.generation == 0 || old.draft_hash.empty())
				continue;
			for (const auto &next : newer.changes) {
				if (next.review_status != "accepted" ||
				    next.path != old.path ||
				    next.operation != old.operation ||
				    next.target_path != old.target_path)
					continue;
				const bool same_baseline =
					(old.original_content_available &&
					 next.original_content_available &&
					 next.creates_file ==
						 old.creates_file &&
					 next.original_content ==
						 old.original_content) ||
					(old.operation == "mkdir" &&
					 old.creates_directory &&
					 next.creates_directory);
				if (!same_baseline)
					continue;
				// Generations are local to a run: a later run can start at 1
				// while replacing generation 2 of an earlier run. Chronology,
				// session, applied acceptance and baseline were checked above.
				if (next.generation > 0 &&
				    !next.draft_hash.empty()) {
					old.review_status = "superseded";
					break;
				}
			}
		}
	}
}

void preserve_committed_reviews(const agent_result_t &existing,
				agent_result_t &incoming)
{
	if (existing.run_id != incoming.run_id ||
	    existing.project_path != incoming.project_path)
		return;
	bool generation_bound = false;
	for (size_t i = 0; i < incoming.changes.size(); ++i) {
		if (incoming.changes[i].generation == 0 ||
		    incoming.changes[i].draft_hash.empty())
			continue;
		generation_bound = true;
		for (size_t j = 0; j < existing.changes.size(); ++j) {
			const agent_change_proposal_t &prior =
				existing.changes[j];
			if (prior.path != incoming.changes[i].path ||
			    prior.generation !=
				    incoming.changes[i].generation ||
			    prior.draft_hash !=
				    incoming.changes[i].draft_hash ||
			    (prior.review_status != "accepted" &&
			     prior.review_status != "rejected"))
				continue;
			incoming.changes[i].review_status = prior.review_status;
			break;
		}
	}
	incoming.changes_applied =
		incoming.changes_applied || existing.changes_applied;
	if (existing.changes_applied_at > incoming.changes_applied_at) {
		incoming.changes_applied_at = existing.changes_applied_at;
	}
	if (!generation_bound)
		return;
	bool pending = false;
	bool accepted = false;
	for (size_t i = 0; i < incoming.changes.size(); ++i) {
		pending = pending ||
			  incoming.changes[i].review_status == "pending";
		accepted = accepted ||
			   incoming.changes[i].review_status == "accepted";
	}
	if (pending)
		incoming.decision = "pending";
	else if (!incoming.changes.empty()) {
		incoming.decision = accepted ? "accepted" : "rejected";
	}
}

} // namespace

agent_result_store_t::agent_result_store_t(const std::string &user_root,
					   const std::string &project_path)
	: user_root_(user_root)
	, project_path_(project_path)
{
}

std::string agent_result_store_t::relative_path(const std::string &run_id) const
{
	return join_path(join_path(join_path(project_path_, ".webcool_agent"),
				   "results"),
			 result_name(run_id));
}

bool agent_result_store_t::save(const agent_result_t &result,
				std::string &err) const
{
	if (!safe_result(result) || result.project_path != project_path_) {
		err = result.changes.size() > kMaxAgentChanges ?
			      "coding-agent result has " +
				      std::to_string(result.changes.size()) +
				      " changes; maximum is " +
				      std::to_string(kMaxAgentChanges) :
			      "invalid coding-agent result metadata or content limits";
		return ai_error("agent.result", "validate-save", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.result", "resolve-project", err);
	}
	std::lock_guard<webcool::mutex> guard(g_result_mutex);
	std::string directory;
	agent_result_t durable = result;
	if (!prepare_directory(project, directory, err)) {
		return ai_error("agent.result", "persist", err);
	}
	// Streaming generation and browser review are concurrent. Never allow a
	// worker snapshot made before the click to overwrite a committed decision for
	// the same generation. A later edit has a new generation/hash and is therefore
	// intentionally left pending.
	agent_result_t existing;
	if (load_locked(directory, result.run_id, existing)) {
		preserve_committed_reviews(existing, durable);
	}
	if (!save_locked(directory, durable, err)) {
		return ai_error("agent.result", "persist", err);
	}
	return true;
}

bool agent_result_store_t::load(const std::string &run_id,
				agent_result_t &result, bool &found,
				std::string &err) const
{
	found = false;
	if (!valid_hex_id(run_id, false)) {
		err = "invalid coding-agent result id";
		return ai_error("agent.result", "validate-load", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.result", "resolve-load-project", err);
	}
	const std::string path = join_path(
		join_path(join_path(project, ".webcool_agent"), "results"),
		result_name(run_id));
	std::string content;
	{
		std::lock_guard<webcool::mutex> guard(g_result_mutex);
		if (!read_result_file(path, content))
			return true;
	}
	if (content.size() > kMaxResultFileBytes ||
	    !parse(content, result, err) || result.run_id != run_id ||
	    result.project_path != project_path_) {
		if (err.empty())
			err = "coding-agent result does not match this run";
		return ai_error("agent.result", "parse-load", err);
	}
	{
		std::lock_guard<webcool::mutex> guard(g_result_mutex);
		mark_superseded_reviews(
			join_path(join_path(project, ".webcool_agent"),
				  "results"),
			result);
	}
	found = true;
	return true;
}

bool agent_result_store_t::list_pending(
	const std::string &session_id,
	std::vector<agent_pending_result_t> &results, std::string &err) const
{
	results.clear();
	if (!valid_hex_id(session_id, false)) {
		err = "invalid review session";
		return false;
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err))
		return false;
	const std::string agent = join_path(project, ".webcool_agent");
	const std::string directory = join_path(agent, "results");
	if (!normal_directory(agent) || !normal_directory(directory))
		return true;
	std::vector<std::string> names;
	if (!result_names(directory, names, err))
		return false;
	for (const auto &name : names) {
		if (name.size() != 44 || name.compare(0, 7, "result-") != 0 ||
		    name.substr(39) != ".json")
			continue;
		const std::string run_id = name.substr(7, 32);
		if (!valid_hex_id(run_id, false))
			continue;
		agent_result_t saved;
		bool found = false;
		if (!load(run_id, saved, found, err))
			return false;
		if (!found || saved.session_id != session_id)
			continue;
		agent_pending_result_t item;
		item.run_id = run_id;
		item.saved_at = saved.saved_at;
		for (const auto &change : saved.changes) {
			if (change.review_status == "pending" ||
			    (change.review_status.empty() &&
			     saved.decision == "pending"))
				++item.pending_count;
		}
		if (item.pending_count)
			results.push_back(item);
	}
	std::sort(results.begin(), results.end(),
		  [](const agent_pending_result_t &a,
		     const agent_pending_result_t &b) {
			  return a.saved_at != b.saved_at ?
					 a.saved_at > b.saved_at :
					 a.run_id > b.run_id;
		  });
	return true;
}

bool agent_result_store_t::set_decision(const std::string &run_id,
					const std::string &decision,
					agent_result_t &result,
					std::string &err) const
{
	if (decision != "accepted" && decision != "rejected") {
		err = "coding-agent result decision must be accepted or rejected";
		return ai_error("agent.result", "validate-decision", err);
	}
	bool found = false;
	if (!load(run_id, result, found, err))
		return false;
	if (!found) {
		err = "coding-agent result not found";
		return ai_error("agent.result", "decision-not-found", err);
	}
	bool has_accepted_change = false;
	bool has_pending_generation = false;
	for (size_t i = 0; i < result.changes.size(); ++i) {
		has_accepted_change =
			has_accepted_change ||
			result.changes[i].review_status == "accepted";
		has_pending_generation =
			has_pending_generation ||
			(result.changes[i].review_status == "pending" &&
			 result.changes[i].generation > 0 &&
			 !result.changes[i].draft_hash.empty());
	}
	if (decision == "accepted" && has_pending_generation) {
		err = "pending coding-agent file generations must be reviewed first";
		return ai_error("agent.result", "accept-pending-generations",
				err);
	}
	if ((result.decision == "accepted" || has_accepted_change) &&
	    decision == "rejected") {
		err = "an accepted coding-agent result cannot be rejected";
		return ai_error("agent.result", "reject-accepted", err);
	}
	result.decision = decision;
	// The whole-result action is also the compatibility path for artifacts made
	// before per-file review generations existed. Keep every still-pending item in
	// agreement with the durable result decision.
	for (size_t i = 0; i < result.changes.size(); ++i) {
		if (result.changes[i].review_status == "pending") {
			result.changes[i].review_status = decision;
		}
	}
	return save(result, err);
}

bool agent_result_store_t::set_change_reviews(
	const std::string &run_id,
	const std::vector<agent_change_review_t> &reviews,
	agent_result_t &result, std::string &err) const
{
	if (reviews.empty() || reviews.size() > kMaxAgentChanges) {
		err = "coding-agent review must contain 1-" +
		      std::to_string(kMaxAgentChanges) + " file decisions";
		return ai_error("agent.result", "validate-change-reviews", err);
	}
	bool found = false;
	if (!load(run_id, result, found, err))
		return false;
	if (!found) {
		err = "coding-agent result not found";
		return ai_error("agent.result", "change-review-not-found", err);
	}
	for (size_t i = 0; i < reviews.size(); ++i) {
		if ((reviews[i].decision != "accepted" &&
		     reviews[i].decision != "rejected") ||
		    reviews[i].path.empty() || reviews[i].generation == 0 ||
		    reviews[i].draft_hash.empty()) {
			err = "invalid coding-agent file review decision";
			return ai_error("agent.result", "validate-file-review",
					err);
		}
		size_t match = result.changes.size();
		for (size_t j = 0; j < result.changes.size(); ++j) {
			if (result.changes[j].path == reviews[i].path &&
			    result.changes[j].generation ==
				    reviews[i].generation &&
			    result.changes[j].draft_hash ==
				    reviews[i].draft_hash) {
				match = j;
				break;
			}
		}
		if (match == result.changes.size()) {
			err = "coding-agent file review generation is stale";
			return ai_error("agent.result", "stale-file-review",
					err);
		}
		if (result.changes[match].review_status != "pending" &&
		    result.changes[match].review_status !=
			    reviews[i].decision) {
			err = "coding-agent file review was already decided differently";
			return ai_error("agent.result",
					"conflicting-file-review", err);
		}
		result.changes[match].review_status = reviews[i].decision;
		// One file-level decision resolves every older intermediate snapshot of
		// that path. A genuinely newer generation remains pending, which protects
		// concurrent review while preserving the expected "confirm once" UX.
		for (size_t j = 0; j < result.changes.size(); ++j) {
			if (result.changes[j].path == reviews[i].path &&
			    result.changes[j].review_status == "pending" &&
			    result.changes[j].generation <=
				    reviews[i].generation) {
				result.changes[j].review_status =
					reviews[i].decision;
			}
		}
	}
	bool pending = false;
	bool accepted = false;
	for (size_t i = 0; i < result.changes.size(); ++i) {
		pending =
			pending || result.changes[i].review_status == "pending";
		accepted = accepted ||
			   result.changes[i].review_status == "accepted";
	}
	if (!pending)
		result.decision = accepted ? "accepted" : "rejected";
	// A review decision alone never proves that bytes reached the formal
	// workspace. The authenticated review action sets changes_applied only after
	// its compare-and-swap filesystem transaction succeeds.
	return save(result, err);
}

} // namespace ai
} // namespace webcool
