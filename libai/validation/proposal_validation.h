#pragma once

#include "../workspace/agent_change_limits.h"
#include "placeholder_proposal.h"
#include "../workspace/agent_workspace.h"
#include "../agent/agent_protocol.h"

namespace webcool
{
namespace ai
{

// Stable causes identify failed preconditions, independent of source text or
// whether the provider submitted a single proposal or a batch.
struct proposal_validation_error_t {
	std::string code;
	std::string path;
	std::string related_path;
	std::string detail;
};

inline bool path_is_in_project(const std::string &project,
			       const std::string &path)
{
	if (project.empty())
		return true;
	return path == project ||
	       (path.size() > project.size() &&
		path.compare(0, project.size(), project) == 0 &&
		path[project.size()] == '/');
}

// Model-facing tools use paths relative to the selected project, while the
// workspace stores canonical paths relative to the user's virtual-disk root.
// Accept both forms and normalize immediately. This also keeps older saved
// conversations working after the clearer project-relative contract is used.
inline bool resolve_project_tool_path(const std::string &raw_path,
				      const std::string &project_path,
				      bool allow_empty, std::string &path,
				      std::string &err)
{
	if (!webcool::ai::agent_workspace_t::normalize_path(raw_path, path,
							    allow_empty, err))
		return false;
	if (path_is_in_project(project_path, path))
		return true;
	if (project_path.empty() || path.empty()) {
		err = "tool path is outside the selected project";
		return false;
	}
	std::string project_relative;
	if (!webcool::ai::agent_workspace_t::normalize_path(
		    project_path + "/" + path, project_relative, false, err) ||
	    !path_is_in_project(project_path, project_relative)) {
		err = "tool path is outside the selected project";
		return false;
	}
	path.swap(project_relative);
	return true;
}

// Add review-only directory dependencies, never create formal filesystem paths.
inline bool stage_missing_proposal_parent(
	agent_workspace_t &workspace, const std::string &project,
	const std::string &directory,
	std::vector<agent_change_proposal_t> &accepted, std::string &err)
{
	for (const auto &change : accepted) {
		if (change.path != directory)
			continue;
		if (change.creates_directory)
			return true;
		err = "parent path conflicts with a staged file operation";
		return false;
	}
	std::vector<workspace_entry_t> entries;
	if (workspace.list(directory, entries, err))
		return true;
	if (err != "workspace path does not exist" || directory.empty() ||
	    directory == project || !path_is_in_project(project, directory))
		return false;
	const size_t slash = directory.rfind('/');
	const std::string parent =
		slash == std::string::npos ? "" : directory.substr(0, slash);
	if (!stage_missing_proposal_parent(workspace, project, parent, accepted,
					   err))
		return false;
	// Reserve one entry for the file whose parent chain is being staged.
	if (accepted.size() + 1 >= kMaxAgentChanges) {
		err = "maximum accumulated changes exceeded by directory dependencies";
		return false;
	}
	agent_change_proposal_t proposal;
	proposal.operation = "mkdir";
	proposal.path = directory;
	proposal.creates_directory = true;
	accepted.push_back(proposal);
	return true;
}

inline size_t validate_change_proposals(
	webcool::ai::agent_workspace_t &workspace,
	const std::string &project_path,
	std::vector<agent_change_proposal_t> &changes,
	std::vector<proposal_validation_error_t> *errors = NULL,
	bool auto_missing_parents = false,
	std::vector<std::string> *unchanged_paths = NULL,
	std::vector<std::string> *missing_deletes = NULL)
{
	if (errors != NULL)
		errors->clear();
	std::vector<agent_change_proposal_t> accepted;
	size_t rejected = 0;
	size_t total_bytes = 0;
	for (size_t i = 0; i < changes.size(); ++i) {
		std::string normalized;
		std::string target;
		std::string err;
		auto reject = [&](const std::string &code,
				  const std::string &related,
				  const std::string &detail) {
			++rejected;
			if (errors != NULL) {
				proposal_validation_error_t failure;
				failure.code = code;
				failure.path = normalized.empty() ?
						       changes[i].path :
						       normalized;
				failure.related_path = related;
				failure.detail = detail;
				errors->push_back(failure);
			}
		};
		if (accepted.size() >= kMaxAgentChanges) {
			reject("change_limit", "",
			       "maximum accumulated changes: " +
				       std::to_string(kMaxAgentChanges));
			continue;
		}
		if (changes[i].operation.empty())
			changes[i].operation = "write";
		const bool valid_operation =
			changes[i].operation == "write" ||
			changes[i].operation == "delete" ||
			changes[i].operation == "move" ||
			changes[i].operation == "mkdir" ||
			changes[i].operation ==
				"replace_empty_file_with_directory";
		if (!resolve_project_tool_path(changes[i].path, project_path,
					       false, normalized, err)) {
			reject("invalid_path", "", err);
			continue;
		}
		if (changes[i].operation == "write" &&
		    explicit_placeholder_proposal(normalized,
						  changes[i].content,
						  changes[i].reason)) {
			reject("explicit_placeholder", "",
			       "source content or short-source reason is explicitly marked placeholder");
			continue;
		}
		if (!valid_operation || (changes[i].operation != "write" &&
					 !changes[i].content.empty())) {
			reject("invalid_operation", "", "");
			continue;
		}
		if (changes[i].operation == "move" &&
		    (!resolve_project_tool_path(changes[i].target_path,
						project_path, false, target,
						err) ||
		     target == normalized)) {
			reject("invalid_target", changes[i].target_path, err);
			continue;
		}
		if (changes[i].content.size() > 1024 * 1024 ||
		    changes[i].content.find('\0') != std::string::npos ||
		    total_bytes + changes[i].content.size() > 2 * 1024 * 1024) {
			reject("invalid_content", "", "");
			continue;
		}
		bool duplicate = false;
		size_t placeholder_delete = accepted.size();
		for (size_t j = 0; j < accepted.size(); ++j) {
			// Models commonly express this repair as delete(empty file)+mkdir.
			// Fold the reviewed pair into one atomic, rollback-safe operation.
			if (accepted[j].path == normalized &&
			    accepted[j].operation == "delete" &&
			    changes[i].operation == "mkdir") {
				placeholder_delete = j;
				break;
			}
			if (accepted[j].path == normalized ||
			    (!target.empty() &&
			     (accepted[j].path == target ||
			      accepted[j].target_path == target)) ||
			    (!accepted[j].target_path.empty() &&
			     accepted[j].target_path == normalized)) {
				duplicate = true;
				break;
			}
		}
		if (placeholder_delete < accepted.size()) {
			std::string placeholder;
			bool placeholder_truncated = false;
			if (!workspace.read(normalized, placeholder,
					    placeholder_truncated, err) ||
			    placeholder_truncated || !placeholder.empty()) {
				reject("placeholder_not_empty", "", err);
				continue;
			}
			accepted[placeholder_delete].operation =
				"replace_empty_file_with_directory";
			accepted[placeholder_delete].creates_directory = true;
			continue;
		}
		const size_t slash = normalized.rfind('/');
		const std::string parent = slash == std::string::npos ?
						   "" :
						   normalized.substr(0, slash);
		std::vector<webcool::ai::workspace_entry_t> entries;
		bool exists = false;
		bool virtual_parent = false;
		for (size_t j = 0; j < accepted.size(); ++j) {
			if (accepted[j].creates_directory &&
			    accepted[j].path == parent) {
				virtual_parent = true;
				break;
			}
		}
		if (!duplicate && virtual_parent) {
			// The parent will exist after the same reviewed atomic change set.
			exists = false;
		} else if (!duplicate && workspace.list(parent, entries, err)) {
			for (size_t j = 0; j < entries.size(); ++j) {
				if (entries[j].path == normalized) {
					exists = true;
					break;
				}
			}
		} else if (!duplicate) {
			auto with_parents = accepted;
			if (auto_missing_parents &&
			    changes[i].operation == "write" &&
			    err == "workspace path does not exist" &&
			    stage_missing_proposal_parent(workspace,
							  project_path, parent,
							  with_parents, err)) {
				accepted.swap(with_parents);
			} else {
				reject(err == "workspace path does not exist" ?
					       "parent_missing" :
				       err == "maximum accumulated changes exceeded by directory dependencies" ?
					       "change_limit" :
					       "parent_unavailable",
				       parent, err);
				continue;
			}
		}
		std::string current;
		bool truncated = false;
		bool target_exists = false;
		if (!target.empty()) {
			const size_t target_slash = target.rfind('/');
			const std::string target_parent =
				target_slash == std::string::npos ?
					"" :
					target.substr(0, target_slash);
			std::vector<webcool::ai::workspace_entry_t>
				target_entries;
			if (!workspace.list(target_parent, target_entries,
					    err)) {
				reject(err == "workspace path does not exist" ?
					       "parent_missing" :
					       "parent_unavailable",
				       target_parent, err);
				continue;
			}
			for (size_t j = 0; j < target_entries.size(); ++j) {
				if (target_entries[j].path == target)
					target_exists = true;
			}
		}
		const bool source_required =
			changes[i].operation == "delete" ||
			changes[i].operation == "move" ||
			changes[i].operation ==
				"replace_empty_file_with_directory";
		if (duplicate) {
			reject("duplicate_path", target, "");
			continue;
		}
		if (source_required && !exists) {
			if (changes[i].operation == "delete" &&
			    missing_deletes != NULL) {
				missing_deletes->push_back(normalized);
				continue;
			}
			reject("source_missing", "", "");
			continue;
		}
		if ((changes[i].operation == "mkdir" && exists) ||
		    (changes[i].operation == "move" && target_exists)) {
			reject("target_exists", target, "");
			continue;
		}
		if ((changes[i].operation == "write" && exists) ||
		    source_required) {
			if (!workspace.read(normalized, current, truncated,
					    err) ||
			    truncated) {
				reject("source_unreadable", "", err);
				continue;
			}
			if (changes[i].operation == "write" &&
			    changes[i].original_content_available &&
			    changes[i].review_status == "pending" &&
			    current != changes[i].original_content) {
				reject("baseline_changed", "", "");
				continue;
			}
			if (changes[i].operation == "write" &&
			    current == changes[i].content) {
				if (unchanged_paths != NULL)
					unchanged_paths->push_back(normalized);
				else
					reject("unchanged_content", "", "");
				continue;
			}
			if (changes[i].operation ==
				    "replace_empty_file_with_directory" &&
			    !current.empty()) {
				reject("placeholder_not_empty", "", "");
				continue;
			}
		}
		changes[i].path = normalized;
		changes[i].target_path = target;
		changes[i].creates_file =
			changes[i].operation == "write" && !exists;
		changes[i].creates_directory =
			changes[i].operation == "mkdir" ||
			changes[i].operation ==
				"replace_empty_file_with_directory";
		if (changes[i].operation == "write" ||
		    changes[i].operation == "delete" ||
		    changes[i].operation == "move") {
			// Keep the trusted filesystem baseline with a live staged proposal. The
			// editor can render an accurate revision and the review endpoint can use
			// compare-and-swap, while the formal file itself remains untouched.
			changes[i].original_content = exists ? current : "";
			changes[i].original_content_available = true;
		}
		if (changes[i].reason.size() > 1000)
			changes[i].reason.resize(1000);
		total_bytes += changes[i].content.size();
		accepted.push_back(changes[i]);
	}
	changes.swap(accepted);
	return rejected;
}

}
} // namespace webcool::ai
