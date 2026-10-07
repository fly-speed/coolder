#include "stdafx.h"
// Repair evidence, transcript compaction and durable progress snapshots.
#include "coding_tool_loop_internal.h"
#include "../validation/diagnostic_source.h"

namespace action {
namespace agent_detail {

namespace coding_loop_detail {

std::string validation_repair_context(const std::string& user_root,
	const std::string& project_path, const std::string& run_id,
	const std::vector<agent_change_proposal_t>& changes,
	const std::string& validation_result, size_t maximum_bytes,
	const webcool::ai::completion_request_t* evidence)
{
	// A compiler diagnostic often points at an unchanged dependent file rather
	// than the files already present in `changes`. The old repair guard forbade a
	// read while attaching only staged files, leaving the model without the exact
	// source it was required to repair. Read diagnostic paths from the private
	// draft (never the formal tree) and carry those bytes into the repair turn.
	std::vector<std::string> diagnostic_paths;
	acl::json parsed(validation_result.c_str());
	acl::json_node* commands = parsed.finish()
		? json_array_node(parsed["commands"]) : NULL;
	for (acl::json_node* command = commands ? commands->first_child() : NULL;
		command != NULL; command = commands->next_child())
	{
		acl::json_node* command_object = command->is_object()
			? command : command->get_obj();
		acl::json_node* diagnostics = command_object == NULL ? NULL
			: json_array_node((*command_object)["diagnostics"]);
		for (acl::json_node* diagnostic = diagnostics
			? diagnostics->first_child() : NULL;
			diagnostic != NULL; diagnostic = diagnostics->next_child())
		{
			acl::json_node* object = diagnostic->is_object()
				? diagnostic : diagnostic->get_obj();
			std::string path = object == NULL ? ""
				: json_text((*object)["path"]);
			std::string normalized;
			std::string normalize_err;
			if (path.empty() || !webcool::ai::agent_workspace_t::normalize_path(
				path, normalized, false, normalize_err)) continue;
			if (std::find(diagnostic_paths.begin(), diagnostic_paths.end(),
				normalized) == diagnostic_paths.end())
			{
				diagnostic_paths.push_back(normalized);
			}
		}
	}

	std::ostringstream out;
	out << "\n<repair_source_context>\n";
	size_t used = 0;
	std::set<std::string> included;
	webcool::ai::agent_workspace_t draft_workspace(
		persistent_draft_root(user_root, project_path, run_id));
    std::vector<std::string> source_files;
    if (std::any_of(diagnostic_paths.begin(), diagnostic_paths.end(),
        [](const std::string& path) { return path.find('/') == std::string::npos; }))
        source_files = webcool::ai::diagnostic_source_files(draft_workspace);
    for (auto& path : diagnostic_paths) if (path.find('/') == std::string::npos) {
        const auto resolved = webcool::ai::unique_diagnostic_source(path, source_files);
        if (!resolved.empty()) path = resolved;
    }
    for (size_t i = 0; i < diagnostic_paths.size(); ++i) {
		std::string content;
		bool truncated = false;
		std::string read_err;
        if (!draft_workspace.read(diagnostic_paths[i], content, truncated, read_err) || truncated) {
            out << "[Missing or truncated diagnostic source: " << diagnostic_paths[i]
                << "; use bounded workspace reads before editing.]\n";
            continue;
        }
		const std::string display_path = project_path.empty()
			? diagnostic_paths[i] : project_path + "/" + diagnostic_paths[i];
		if (evidence && (webcool::ai::request_has_source(*evidence, display_path, content)
			|| webcool::ai::request_has_source(*evidence, diagnostic_paths[i], content))) {
			included.insert(display_path);
			continue;
		}
		const std::string header = "--- " + display_path
			+ " (compiler diagnostic source) ---\n";
		if (used + header.size() + content.size() + 1 > maximum_bytes) break;
		out << header << content << "\n";
		used += header.size() + content.size() + 1;
		included.insert(display_path);
	}
	// Prioritize declarations, then callers. A large implementation must not
	// prevent smaller dependent files later in the snapshot from being included.
	bool omitted = false;
	for (int priority = 0; priority < 2; ++priority) {
		for (size_t i = 0; i < changes.size(); ++i) {
			if (changes[i].operation != "write" || changes[i].content.empty()
				|| changes[i].review_status == "rejected"
				|| included.find(changes[i].path) != included.end()
				|| webcool::ai::staged_interface_path(changes[i].path) != (priority == 0)) continue;
			if (evidence && (webcool::ai::request_has_source(*evidence,
				changes[i].path, changes[i].content)
				|| (workspace_path_is_below(changes[i].path, project_path)
					&& webcool::ai::request_has_source(*evidence,
						changes[i].path.substr(project_path.size() + 1), changes[i].content)))) continue;
			const std::string header = "--- " + changes[i].path
				+ " (current staged source) ---\n";
			if (used + header.size() + changes[i].content.size() + 1 > maximum_bytes) {
				omitted = true;
				continue;
			}
			out << header << changes[i].content << "\n";
			used += header.size() + changes[i].content.size() + 1;
		}
	}
	if (omitted) out << "[remaining repair source omitted by context bound]\n";
	out << "</repair_source_context>\n";
	return out.str();
}

bool save_coding_progress(webcool::ai::agent_progress_store_t& store,
	const webcool::ai::provider_config_t& provider,
	const std::shared_ptr<agent_runtime_task_t>& runtime_task,
	const std::string& project_path, const std::string& original_prompt,
	const std::string& transcript, const std::string& reasoning,
	const std::string& last_error, size_t completed_tool_calls,
	std::string& err)
{
	if (runtime_task->text_preview) return true;
	webcool::ai::agent_progress_t progress;
	progress.source_run_id = runtime_task->id;
	progress.provider_id = provider.id;
	progress.provider_model = provider.model;
	progress.project_path = project_path;
	progress.session_id = runtime_task->session_id;
	progress.original_prompt = original_prompt;
	progress.transcript = transcript;
	progress.reasoning = reasoning;
	progress.last_error = last_error;
	progress.completed_tool_calls = completed_tool_calls;
	progress.updated_at = static_cast<long long>(time(NULL));
	{
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		progress.staged_changes = runtime_task->changes;
		progress.provider_response_id = runtime_task->provider_response_id;
		progress.provider_response_pending = runtime_task->provider_response_pending;
		for (size_t i = 0; i < runtime_task->provider_tool_outputs.size(); ++i) {
			webcool::ai::agent_progress_tool_output_t output;
			output.call_id = runtime_task->provider_tool_outputs[i].call_id;
			output.output = runtime_task->provider_tool_outputs[i].output;
			progress.provider_tool_outputs.push_back(output);
		}
	}
	// File review can happen from another browser while the worker is inside a
	// provider request. The result artifact is the authoritative review ledger;
	// merge matching immutable generations before writing a checkpoint so a stale
	// in-memory `pending` value cannot resurrect after restart/resume.
	webcool::ai::agent_result_store_t result_store(runtime_task->user_root,
		project_path);
	webcool::ai::agent_result_t reviewed_result;
	bool reviewed_found = false;
	std::string reviewed_err;
	if (result_store.load(runtime_task->id, reviewed_result, reviewed_found,
		reviewed_err) && reviewed_found)
	{
		for (size_t i = 0; i < progress.staged_changes.size(); ++i) {
			for (size_t j = 0; j < reviewed_result.changes.size(); ++j) {
				const agent_change_proposal_t& reviewed = reviewed_result.changes[j];
				if (progress.staged_changes[i].path != reviewed.path
					|| progress.staged_changes[i].generation != reviewed.generation
					|| progress.staged_changes[i].draft_hash != reviewed.draft_hash
					|| (reviewed.review_status != "accepted"
						&& reviewed.review_status != "rejected")) continue;
				progress.staged_changes[i].review_status = reviewed.review_status;
				break;
			}
		}
		// Keep the live snapshot aligned as well; otherwise a later incremental
		// publish in this process could temporarily repaint accepted files pending.
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		for (size_t i = 0; i < runtime_task->changes.size(); ++i) {
			for (size_t j = 0; j < progress.staged_changes.size(); ++j) {
				if (runtime_task->changes[i].path == progress.staged_changes[j].path
					&& runtime_task->changes[i].generation
						== progress.staged_changes[j].generation
					&& runtime_task->changes[i].draft_hash
						== progress.staged_changes[j].draft_hash)
				{
					runtime_task->changes[i].review_status =
						progress.staged_changes[j].review_status;
					break;
				}
			}
		}
	} else if (!reviewed_err.empty()) {
		webcool::ai::ai_log_error("agent.runtime",
			"load-review-state-for-progress", reviewed_err);
	}
	return store.save(progress, err);
}

std::string compact_coding_transcript(const std::string& original_prompt,
	const std::vector<agent_change_proposal_t>& changes,
	const std::vector<agent_tool_trace_t>& traces,
	const std::string& latest_exchange, bool chinese)
{
	// Reserve current draft source separately so old history cannot evict newly
	// generated interfaces. Rebuild it on every compaction from current changes.
	const std::string records = webcool::ai::staged_source_records(changes, 32 * 1024);
	const std::string staged = records == "[]" ? "" :
		std::string(prompt_text(prompt_id::staged_source_pages, chinese))
		+ "<staged_working_set>" + records + "</staged_working_set>\n";
	const size_t history_limit = kMaxToolTranscriptBytes - staged.size();
	std::ostringstream out;
	std::string bounded_prompt = webcool::ai::utf8_prefix(original_prompt, 24 * 1024);
	out << bounded_prompt << "\n\n<context_compaction>\n"
		<< prompt_text(prompt_id::retained_source_pages, chinese)
		<< prompt_text(prompt_id::compacted_source_pages, chinese)
		<< prompt_text(prompt_id::missing_source_pages, chinese)
		<< prompt_text(prompt_id::pending_review_set, chinese);
	for (size_t i = 0; i < changes.size(); ++i) {
		out << "- " << changes[i].operation << " " << changes[i].path;
		if (!changes[i].target_path.empty()) out << " -> " << changes[i].target_path;
		out << " | " << changes[i].content.size() << " bytes\n";
	}
	out << prompt_text(prompt_id::recent_tool_metadata, chinese);
	const size_t trace_begin = traces.size() > 24 ? traces.size() - 24 : 0;
	for (size_t i = trace_begin; i < traces.size(); ++i) {
		out << "- " << traces[i].name << " " << traces[i].path
			<< " | " << (traces[i].ok ? "ok" : "failed") << "\n";
	}
	out << "</context_compaction>\n" << latest_exchange;
	std::string compacted = out.str();
	if (compacted.size() > history_limit) {
		// latest_exchange is already bounded by the tool-result policy. Trim only
		// the old user prompt prefix while preserving the current tool outcome.
		const size_t keep = std::min(latest_exchange.size() + 32 * 1024,
			history_limit - std::string(prompt_text(prompt_id::context_compacted, chinese)).size());
		compacted = compacted.substr(compacted.size() - keep);
		compacted.insert(0,
			prompt_text(prompt_id::context_compacted, chinese));
	}
	return compacted + staged;
}

} // namespace coding_loop_detail

} // namespace agent_detail
} // namespace action
