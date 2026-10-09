#include "stdafx.h"
// Staged worktrees, validation, review synchronization and background cleanup.
#include "coding_runtime.h"
#include "../validation/repair_scope.h"
#include "../validation/diagnostic_source.h"

namespace action
{
namespace agent_detail
{

bool workspace_entry_state(webcool::ai::agent_workspace_t &workspace,
    const std::string &raw_path, bool &exists, bool &directory,
    std::string &err)
{
	std::string path;
	if (!webcool::ai::agent_workspace_t::normalize_path(
	        raw_path, path, false, err))
		return false;
	const size_t slash = path.rfind('/');
	const std::string parent =
	    slash == std::string::npos ? "" : path.substr(0, slash);
	std::vector<webcool::ai::workspace_entry_t> entries;
	if (!workspace.list(parent, entries, err, false)) {
		if (err != "workspace path does not exist")
			return false;
		exists = false;
		directory = false;
		err.clear();
		return true;
	}
	exists = false;
	directory = false;
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].path != path)
			continue;
		exists = true;
		directory = entries[i].directory;
		break;
	}
	return true;
}

bool verify_result_changes_applied(const std::string &user_root,
    const webcool::ai::agent_result_t &result, std::string &err)
{
	webcool::ai::agent_workspace_t workspace(user_root);
	for (size_t i = 0; i < result.changes.size(); ++i) {
		const agent_change_proposal_t &change = result.changes[i];
		bool exists = false;
		bool directory = false;
		if (!workspace_entry_state(
		        workspace, change.path, exists, directory, err))
			return false;
		bool matches = false;
		if (change.operation == "write" && exists && !directory) {
			std::string content;
			bool truncated = false;
			matches = workspace.read(
			              change.path, content, truncated, err) &&
			    !truncated && content == change.content;
		} else if (change.operation == "delete") {
			matches = !exists;
		} else if (change.operation == "mkdir" ||
		    change.operation == "replace_empty_file_with_directory") {
			matches = exists && directory;
		} else if (change.operation == "move" && !exists) {
			bool target_exists = false;
			bool target_directory = false;
			if (!workspace_entry_state(workspace,
			        change.target_path, target_exists,
			        target_directory, err))
				return false;
			std::string content;
			bool truncated = false;
			matches = target_exists && !target_directory &&
			    workspace.read(
			        change.target_path, content, truncated, err) &&
			    !truncated && change.original_content_available &&
			    content == change.original_content;
		}
		if (matches)
			continue;
		err =
		    "formal workspace does not match the accepted coding-agent result";
		return false;
	}
	return true;
}

bool workspace_change_matches(const std::string &user_root,
    const webcool::ai::workspace_change_input_t &change, bool &matches,
    std::string &err)
{
	webcool::ai::agent_workspace_t workspace(user_root);
	bool exists = false;
	bool directory = false;
	if (!workspace_entry_state(
	        workspace, change.path, exists, directory, err)) {
		return false;
	}
	matches = false;
	if (change.operation == "write" && exists && !directory) {
		std::string content;
		bool truncated = false;
		if (!workspace.read(change.path, content, truncated, err))
			return false;
		matches = !truncated && content == change.content;
	} else if (change.operation == "delete") {
		matches = !exists;
	} else if (change.operation == "mkdir" ||
	    change.operation == "replace_empty_file_with_directory") {
		matches = exists && directory;
	} else if (change.operation == "move" && !exists) {
		bool target_exists = false;
		bool target_directory = false;
		if (!workspace_entry_state(workspace, change.target_path,
		        target_exists, target_directory, err))
			return false;
		std::string content;
		bool truncated = false;
		if (target_exists && !target_directory &&
		    !workspace.read(
		        change.target_path, content, truncated, err)) {
			return false;
		}
		matches = target_exists && !target_directory && !truncated &&
		    content == change.expected_current_content;
	}
	return true;
}

bool workspace_path_is_below(
    const std::string &path, const std::string &directory)
{
	return !directory.empty() && path.size() > directory.size() &&
	    path.compare(0, directory.size(), directory) == 0 &&
	    path[directory.size()] == '/';
}

std::string persistent_draft_root(const std::string &user_root,
    const std::string &project_path, const std::string &run_id)
{
	return webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	    .root_path();
}

bool materialize_staged_worktree(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes, size_t &skipped_files,
    std::string &err)
{
	const operation_cpu_snapshot_t started;
	const auto task = find_runtime_task(user_root, run_id);
	acl::json begin_json;
	acl::json_node &begin = begin_json.create_node();
	begin.add_text("event", "draft_materialize_started");
	begin.add_number(
	    "change_count", static_cast<long long>(changes.size()));
	append_runtime_operation_event(task, begin);
	bool reused = false;
	const bool ok =
	    webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	        .materialize(changes, skipped_files, err, &reused, NULL,
	            [task] { return task && runtime_cancel_requested(task); });
	acl::json end_json;
	acl::json_node &end = end_json.create_node();
	end.add_text("event", "draft_materialize_finished");
	end.add_bool("ok", ok);
	end.add_bool("reused", reused);
	started.add_delta(end, operation_cpu_snapshot_t());
	if (ok)
		end.add_number(
		    "skipped_files", static_cast<long long>(skipped_files));
	append_runtime_operation_event(task, end);
	return ok;
}

bool synchronize_live_review_state(
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    const std::string &project_path,
    std::vector<agent_change_proposal_t> &active_changes,
    size_t &resolved_count, std::string &err)
{
	// A user may accept or reject a proposal while an HTTP model request is in
	// flight. The worker owns a separate vector, so take a short locked snapshot
	// and discard only exact generations that the browser has resolved.
	std::vector<agent_change_proposal_t> review_snapshot;
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		review_snapshot = runtime_task->changes;
	}
	webcool::ai::agent_result_store_t ledger(
	    runtime_task->user_root, project_path);
	webcool::ai::agent_result_t durable_review;
	bool ledger_found = false;
	if (!ledger.load(runtime_task->id, durable_review, ledger_found, err))
		return false;
	if (ledger_found)
		review_snapshot = durable_review.changes;
	const std::vector<agent_change_proposal_t> before = active_changes;
	resolved_count = webcool::ai::remove_resolved_agent_review_changes(
	    review_snapshot, active_changes);
	if (resolved_count == 0)
		return true;

	// Rebuild from the current formal source plus the still-pending overlay.
	// Accepted bytes are now formal; rejected bytes must disappear from the
	// private worktree before the next model tool is evaluated.
	size_t skipped_files = 0;
	if (!materialize_staged_worktree(runtime_task->user_root, project_path,
	        runtime_task->id, active_changes, skipped_files, err)) {
		active_changes = before;
		webcool::ai::ai_log_error(
		    "agent.runtime", "synchronize-live-review-worktree", err);
		return false;
	}

	acl::json synchronized_json;
	acl::json_node &event = synchronized_json.create_node();
	event.add_text("event", "live_review_state_synchronized");
	event.add_number("resolved_generation_count",
	    static_cast<long long>(resolved_count));
	event.add_number("remaining_pending_change_count",
	    static_cast<long long>(active_changes.size()));
	event.add_number("skipped_binary_or_large_files",
	    static_cast<long long>(skipped_files));
	append_runtime_operation_event(runtime_task, event);
	return true;
}

bool remove_persistent_worktree(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    std::string &err)
{
	const operation_cpu_snapshot_t started;
	const auto task = find_runtime_task(user_root, run_id);
	acl::json begin_json;
	acl::json_node &begin = begin_json.create_node();
	begin.add_text("event", "draft_cleanup_started");
	append_runtime_operation_event(task, begin);
	const bool ok =
	    webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	        .remove(err);
	acl::json end_json;
	acl::json_node &end = end_json.create_node();
	end.add_text("event", "draft_cleanup_finished");
	end.add_bool("ok", ok);
	started.add_delta(end, operation_cpu_snapshot_t());
	append_runtime_operation_event(task, end);
	return ok;
}

// One process-wide cleaner avoids concurrent accepted runs multiplying I/O.
// Only detached identities enter this queue; resumed run paths are never used.
struct reviewed_cleanup_job_t {
	std::string user_root, project, run, retired;
};
class reviewed_cleanup_queue_t {
public:
	std::mutex mutex;
	std::condition_variable ready;
	std::deque<reviewed_cleanup_job_t> jobs;
	bool stopping = false;
	std::thread worker;
	reviewed_cleanup_queue_t()
	        : worker([this] { run(); })
	{
	}
	~reviewed_cleanup_queue_t()
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			stopping = true;
			jobs.clear();
		}
		ready.notify_one();
		if (worker.joinable())
			worker.join();
	}
	void run();
};

bool schedule_reviewed_worktree_cleanup(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    std::string &err)
{
	const std::string retired = new_run_id();
	bool detached = false;
	if (!webcool::ai::agent_draft_store_t(user_root, project_path, run_id)
	         .retire(retired, detached, err))
		return false;
	if (!detached)
		return true;
	const auto task = find_runtime_task(user_root, run_id);
	acl::json json;
	auto &event = json.create_node();
	event.add_text("event", "draft_cleanup_queued");
	event.add_text("cleanup_id", retired.c_str());
	append_runtime_operation_event(task, event);
	static reviewed_cleanup_queue_t cleaner;
	{
		std::lock_guard<std::mutex> guard(cleaner.mutex);
		cleaner.jobs.push_back(
		    { user_root, project_path, run_id, retired });
	}
	cleaner.ready.notify_one();
	return true;
}

void reviewed_cleanup_queue_t::run()
{
	while (true) {
		reviewed_cleanup_job_t job;
		{
			std::unique_lock<std::mutex> guard(mutex);
			ready.wait(guard,
			    [this] { return stopping || !jobs.empty(); });
			if (stopping)
				return;
			job = jobs.front();
			jobs.pop_front();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		const auto cleanup_task =
		    find_runtime_task(job.user_root, job.run);
		const operation_cpu_snapshot_t started;
		acl::json begin_json;
		auto &begin = begin_json.create_node();
		begin.add_text("event", "draft_cleanup_started");
		begin.add_bool("background", true);
		begin.add_text("cleanup_id", job.retired.c_str());
		append_runtime_operation_event(cleanup_task, begin);
		std::string cleanup_err;
		const bool ok = webcool::ai::agent_draft_store_t(
		    job.user_root, job.project, job.retired)
		                    .remove(cleanup_err);
		if (!ok)
			webcool::ai::ai_log_error("agent.draft",
			    "background-reviewed-cleanup", cleanup_err);
		acl::json end_json;
		auto &end = end_json.create_node();
		end.add_text("event", "draft_cleanup_finished");
		end.add_bool("background", true);
		end.add_bool("ok", ok);
		end.add_text("cleanup_id", job.retired.c_str());
		started.add_delta(end, operation_cpu_snapshot_t());
		append_runtime_operation_event(cleanup_task, end);
	}
}

std::string staged_change_fingerprint(
    const std::vector<agent_change_proposal_t> &changes)
{
	std::string fingerprint;
	for (size_t i = 0; i < changes.size(); ++i) {
		fingerprint += changes[i].operation + "\n" + changes[i].path +
		    "\n" + changes[i].target_path + "\n" +
		    webcool::ai::agent_workspace_t::content_sha256(
		        changes[i].content) +
		    "\n";
	}
	return fingerprint;
}

std::string staged_cycle_fingerprint(
    const std::vector<agent_change_proposal_t> &changes)
{
	std::vector<std::string> entries;
	for (const auto &change : changes)
		entries.push_back(change.operation + "\n" + change.path + "\n" +
		    change.target_path + "\n" +
		    webcool::ai::agent_workspace_t::content_sha256(
		        change.content));
	std::sort(entries.begin(), entries.end());
	std::string canonical;
	for (const auto &entry : entries)
		canonical += std::to_string(entry.size()) + ":" + entry;
	return webcool::ai::agent_workspace_t::content_sha256(canonical);
}

} // namespace agent_detail
} // namespace action
