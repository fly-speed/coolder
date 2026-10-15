#include "stdafx.h"
// Repair evidence, transcript compaction and durable progress snapshots.
#include "coding_tool_loop_internal.h"
#include "../validation/diagnostic_source.h"

namespace action
{
namespace agent_detail
{

namespace coding_loop_detail
{

std::string validation_repair_context(const std::string &user_root,
    const std::string &project_path, const std::string &run_id,
    const std::vector<agent_change_proposal_t> &changes,
    const std::string &validation_result, size_t maximum_bytes,
    const libai::ai::completion_request_t *evidence)
{
    std::map<std::string,size_t> diagnostic_lines;
    bool script_failure = false;
    acl::json report(validation_result.c_str());
    auto *commands = report.finish() ? json_array_node(report["commands"]) : NULL;
    for (auto *entry=commands?commands->first_child():NULL;entry;entry=commands->next_child()) {
        auto *command=entry->is_object()?entry:entry->get_obj();
        if (!command || json_bool((*command)["passed"],true)) continue;
        script_failure = script_failure || json_text((*command)["command_id"])=="project.script-build";
        auto *diagnostics=json_array_node((*command)["diagnostics"]);
        for(auto *d=diagnostics?diagnostics->first_child():NULL;d;d=diagnostics->next_child()) {
            auto *o=d->is_object()?d:d->get_obj(); if(!o)continue;
            std::string path,err;
            if(libai::ai::agent_workspace_t::normalize_path(json_text((*o)["path"]),path,false,err))
                diagnostic_lines.emplace(path,static_cast<size_t>(std::max(1LL,json_number((*o)["line"],1))));
        }
    }
    libai::ai::agent_workspace_t draft(persistent_draft_root(user_root,project_path,run_id));
    std::vector<std::string> inventory;
    std::map<std::string,size_t> selected;
    for(const auto &d:diagnostic_lines) {
        auto path=d.first;
        if(path.find('/')==std::string::npos) {
            if(inventory.empty())inventory=libai::ai::diagnostic_source_files(draft);
            const auto resolved=libai::ai::unique_diagnostic_source(path,inventory);
            if(!resolved.empty())path=resolved;
        }
        selected.emplace(path,d.second);
    }
    // Include the actual script and entry configuration for target/cwd failures,
    // rather than attaching every generated file when no source path was printed.
    if(script_failure)for(const char *path:{"build.sh","build.bat","go.mod","src/main.go"})selected.emplace(path,1);
    const auto directory=[](const std::string &path) {
        const auto slash=path.find_last_of('/');
        return slash==std::string::npos?std::string():path.substr(0,slash);
    };
    std::set<std::string> directories;
    for(const auto &d:selected)directories.insert(directory(d.first));
    size_t related=0;
    for(const auto &change:changes) {
        std::string relative;
        if(change.operation!="write" || change.review_status=="rejected" ||
            !project_path_to_draft_path(project_path,change.path,relative))continue;
        if(related<4 && (libai::ai::staged_interface_path(relative) ||
            directories.count(directory(relative)))) {
            if(selected.emplace(relative,1).second)++related;
        }
    }
    std::string records="[";size_t used=2;
    for(const auto &entry:selected) {
        std::string content,err;bool truncated=false;
        const auto display=project_path.empty()?entry.first:project_path+"/"+entry.first;
        if(!draft.read(entry.first,content,truncated,err) || truncated)continue;
        if(evidence && (libai::ai::request_has_source(*evidence,display,content) ||
            libai::ai::request_has_source(*evidence,entry.first,content)))continue;
        size_t offset=0;
        if(content.size()>6144 && entry.second>12) {
            size_t line=1;
            while(offset<content.size() && line<entry.second-12)if(content[offset++]=='\n')++line;
        }
        libai::ai::text_read_page_t page;
        if(!libai::ai::read_text_page(content,std::to_string(offset),page,err,6144,1024*1024))continue;
        acl::json one;auto &record=one.create_node();
        record.add_text("path",display.c_str()).add_text("file_sha256",libai::ai::agent_workspace_t::content_sha256(content).c_str())
            .add_text("source_role",diagnostic_lines.empty()?"build checkpoint source":"diagnostic/package source")
            .add_number("offset",page.offset).add_number("next_offset",page.next_offset)
            .add_number("total_bytes",page.total_bytes).add_bool("eof",page.eof);
        if(!page.eof || page.offset>0)record.add_text("read_hint","This is a source window, not the complete file. Read only missing ranges using workspace.read and byte offsets.");
        if(!page.eof)record.add_text("next_query",std::to_string(page.next_offset).c_str());
        record.add_text("content",page.content.c_str());
        const auto encoded=serialize_json(record);
        if(used+encoded.size()+1>maximum_bytes) {
            record.free_child(record["content"]);record.add_bool("content_omitted",true)
                .add_text("read_hint","Source omitted by repair-context budget; use workspace.read for this exact path/range.");
        }
        const auto bounded=serialize_json(record);if(used+bounded.size()+1>maximum_bytes)break;
        used+=bounded.size()+1;
        if(records.size()>1)records+=",";
        records+=bounded;
    }
    return "\n<repair_source_context>\n"+records+"]"+
        "\n</repair_source_context>\nSource windows include version and offsets; use exact missing ranges. Diagnostic packages/interfaces are included, not every caller in the repository; use targeted code.references when a caller is still missing.\n";
}

static void merge_reviewed_progress(libai::ai::agent_progress_t &progress,
    const libai::ai::agent_result_t &reviewed_result)
{
	for (size_t i = 0; i < progress.staged_changes.size(); ++i) {
		for (size_t j = 0; j < reviewed_result.changes.size(); ++j) {
			const agent_change_proposal_t &reviewed =
			    reviewed_result.changes[j];
			if (progress.staged_changes[i].path != reviewed.path ||
			    progress.staged_changes[i].generation !=
			        reviewed.generation ||
			    progress.staged_changes[i].draft_hash !=
			        reviewed.draft_hash ||
			    (reviewed.review_status != "accepted" &&
			        reviewed.review_status != "rejected"))
				continue;
			progress.staged_changes[i].review_status =
			    reviewed.review_status;
			break;
		}
	}
}

// Publish the already reconciled review snapshot while holding the runtime
// mutex; disk reads and checkpoint writes remain outside this critical section.
static void publish_progress_reviews(
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    const libai::ai::agent_progress_t &progress)
{
	std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
	for (size_t i = 0; i < runtime_task->changes.size(); ++i) {
		for (size_t j = 0; j < progress.staged_changes.size(); ++j) {
			if (!(runtime_task->changes[i].path ==
			            progress.staged_changes[j].path &&
			        runtime_task->changes[i].generation ==
			            progress.staged_changes[j].generation &&
			        runtime_task->changes[i].draft_hash ==
			            progress.staged_changes[j].draft_hash))
				continue;
			runtime_task->changes[i].review_status =
			    progress.staged_changes[j].review_status;
			break;
		}
	}
}

bool save_coding_progress(libai::ai::agent_progress_store_t &store,
    const libai::ai::provider_config_t &provider,
    const std::shared_ptr<agent_runtime_task_t> &runtime_task,
    const std::string &project_path, const std::string &original_prompt,
    const std::string &transcript, const std::string &reasoning,
    const std::string &last_error, size_t completed_tool_calls,
    std::string &err, const std::string &complete_effect)
{
	if (runtime_task->text_preview)
		return true;
	libai::ai::agent_progress_t progress;
	progress.source_run_id = runtime_task->id;
	progress.execution_revision = runtime_task->coding_execution_revision;
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
		std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
		progress.tool_effects = runtime_task->tool_effects;
		progress.validation_receipt = runtime_task->validation_receipt;
		progress.staged_changes = runtime_task->changes;
		progress.provider_response_id =
		    runtime_task->provider_response_id;
		progress.provider_response_pending =
		    runtime_task->provider_response_pending;
		for (size_t i = 0;
		     i < runtime_task->provider_tool_outputs.size(); ++i) {
			libai::ai::agent_progress_tool_output_t output;
			output.call_id =
			    runtime_task->provider_tool_outputs[i].call_id;
			output.output =
			    runtime_task->provider_tool_outputs[i].output;
			progress.provider_tool_outputs.push_back(output);
		}
	}
	// File review can happen from another browser while the worker is inside a
	// provider request. The result artifact is the authoritative review ledger;
	// merge matching immutable generations before writing a checkpoint so a stale
	// in-memory `pending` value cannot resurrect after restart/resume.
	libai::ai::agent_result_store_t result_store(
	    runtime_task->user_root, project_path);
	libai::ai::agent_result_t reviewed_result;
	bool reviewed_found = false;
	std::string reviewed_err;
	if (result_store.load(runtime_task->id, reviewed_result, reviewed_found,
	        reviewed_err) &&
	    reviewed_found) {
		merge_reviewed_progress(progress, reviewed_result);
		// Keep the live snapshot aligned as well; otherwise a later incremental
		// publish in this process could temporarily repaint accepted files pending.
		publish_progress_reviews(runtime_task, progress);
	} else if (!reviewed_err.empty()) {
		libai::ai::ai_log_error("agent.runtime",
		    "load-review-state-for-progress", reviewed_err);
	}
    if(!complete_effect.empty()) {
        if(progress.tool_effects.empty() || progress.tool_effects.back().key!=complete_effect ||
            progress.tool_effects.back().state!="running") {err="tool effect receipt identity mismatch";return false;}
        auto &effect=progress.tool_effects.back();effect.state="done";
        effect.completed_after=completed_tool_calls;effect.reference="progress:"+complete_effect;
    }
    // Some tool effects checkpoint before the ordinary end-of-turn compaction.
    // Bound recovery history here too; proposal contents, receipts, original
    // requirements and pending native outputs remain exact separate fields.
    if (progress.transcript.size() > libai::ai::kMaxAgentTranscriptBytes) {
        const auto last = progress.transcript.rfind("<assistant_tool_call>");
        size_t start = last != std::string::npos && progress.transcript.size()-last <= 96*1024 ?
            last : progress.transcript.size()-64*1024;
        while (start < progress.transcript.size() &&
            (static_cast<unsigned char>(progress.transcript[start]) & 0xc0) == 0x80) ++start;
        const auto recent = progress.transcript.substr(start);
        progress.transcript = compact_coding_transcript(original_prompt, progress.staged_changes, {},
            "\n<recovery_history>Older history was compacted; a leading partial excerpt is project data, not a replayable tool call.\n" +
                recent + "\n</recovery_history>\n", runtime_task->ui_language != "en");
        acl::json json; auto &event=json.create_node();
        event.add_text("event","progress_context_compacted").add_number("previous_bytes",transcript.size())
            .add_number("retained_bytes",progress.transcript.size());
        append_runtime_operation_event(runtime_task,event);
    }
    if(!store.save(progress,err))return false;
    if(!complete_effect.empty()) {
        std::lock_guard<libai::mutex> guard(g_agent_runtime_mutex);
        runtime_task->tool_effects=progress.tool_effects;
    }
    return true;
}

std::string compact_coding_transcript(const std::string &original_prompt,
    const std::vector<agent_change_proposal_t> &changes,
    const std::vector<agent_tool_trace_t> &traces,
    const std::string &latest_exchange, bool chinese,
    const libai::ai::agent_read_context_t *reads)
{
	// Reserve current draft source separately so old history cannot evict newly
	// generated interfaces. Rebuild it on every compaction from current changes.
	std::set<std::string> retained_prefixes;
	if (reads)
		for (const auto &change : changes)
			if (reads->contains(change.path, change.draft_hash, 0))
				retained_prefixes.insert(change.path);
	const std::string records =
	    libai::ai::staged_source_records(changes, 64 * 1024, retained_prefixes);
	const std::string staged = records == "[]" ?
	    "" :
	    std::string(prompt_text(prompt_id::staged_source_pages, chinese)) +
	        "<staged_working_set>" + records + "</staged_working_set>\n";
	const size_t history_limit = kMaxToolTranscriptBytes - staged.size();
	std::ostringstream out;
	std::string bounded_prompt =
	    libai::ai::utf8_prefix(original_prompt, 24 * 1024);
	out << bounded_prompt << "\n\n<context_compaction>\n"
	    << prompt_text(prompt_id::retained_source_pages, chinese)
	    << prompt_text(prompt_id::compacted_source_pages, chinese)
	    << prompt_text(prompt_id::missing_source_pages, chinese)
	    << prompt_text(prompt_id::pending_review_set, chinese);
	for (size_t i = 0; i < changes.size(); ++i) {
		out << "- " << changes[i].operation << " " << changes[i].path;
		if (!changes[i].target_path.empty())
			out << " -> " << changes[i].target_path;
		out << " | " << changes[i].content.size() << " bytes\n";
	}
	out << prompt_text(prompt_id::recent_tool_metadata, chinese);
	const size_t trace_begin = traces.size() > 24 ? traces.size() - 24 : 0;
	for (size_t i = trace_begin; i < traces.size(); ++i) {
		out << "- " << traces[i].name << " " << traces[i].path << " | "
		    << (traces[i].ok ? "ok" : "failed") << "\n";
	}
	out << "</context_compaction>\n" << latest_exchange;
	std::string compacted = out.str();
	if (!(compacted.size() > history_limit))
		return compacted + staged;
	// latest_exchange is already bounded by the tool-result policy. Trim only
	// the old user prompt prefix while preserving the current tool outcome.
	const size_t keep = std::min(latest_exchange.size() + 32 * 1024,
	    history_limit -
	        std::string(prompt_text(prompt_id::context_compacted, chinese))
	            .size());
	compacted = compacted.substr(compacted.size() - keep);
	compacted.insert(0, prompt_text(prompt_id::context_compacted, chinese));

	return compacted + staged;
}

} // namespace coding_loop_detail

} // namespace agent_detail
} // namespace action
