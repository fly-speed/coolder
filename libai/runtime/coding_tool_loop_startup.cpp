#include "stdafx.h"
// Initial context, checkpoint recovery and validation capabilities.
#include "coding_tool_loop_internal.h"

namespace action
{
namespace agent_detail
{

using coding_loop_detail::validation_repair_context;
using coding_loop_detail::save_coding_progress;
using coding_loop_detail::compact_coding_transcript;

void coding_tool_loop_t::initialize_transcript()
{
	// The persistent transcript contains the original request and completed tool
	// results. A previous failed model stream is added only to this invocation's
	// context so repeated transport retries cannot grow the checkpoint forever.
	persistent_transcript = initial_prompt;
	transcript = persistent_transcript;
	if (!recovered_reasoning.empty()) {
		transcript +=
		    prompt_text(prompt_id::reasoning_recovery_begin, chinese);
		size_t tail = recovered_reasoning.size() > 8 * 1024 ?
		    recovered_reasoning.size() - 8 * 1024 :
		    0;
		// Avoid starting the UTF-8 excerpt in the middle of a continuation byte.
		while (tail < recovered_reasoning.size() &&
		    (static_cast<unsigned char>(recovered_reasoning[tail]) &
		        0xc0) == 0x80) {
			++tail;
		}
		transcript.append(recovered_reasoning, tail, std::string::npos);
		transcript +=
		    prompt_text(prompt_id::reasoning_recovery_end, chinese);
	}
	provider_history_base = transcript;
	accumulated_reasoning = recovered_reasoning;
	if (accumulated_reasoning.size() > 1024 * 1024) {
		accumulated_reasoning.resize(1024 * 1024);
	}
	rejected_changes = 0;
}

// Reconcile durable review decisions before restoring provider continuation
// and draft state, so an accepted generation is not proposed again on resume.
bool coding_tool_loop_t::restore_checkpoint_contents(
    const webcool::ai::agent_progress_t &restored, std::string &restore_err)
{
	changes = restored.staged_changes;
	// Reconcile each immutable generation with its durable review ledger.
	// A mixed checkpoint can contain accepted, rejected and pending files.
	if (!restored.source_run_id.empty()) {
		webcool::ai::agent_result_store_t ledger(
		    runtime_task->user_root, project_path);
		webcool::ai::agent_result_t reviewed;
		bool ledger_found = false;
		if (!ledger.load(restored.source_run_id, reviewed, ledger_found,
		        restore_err)) {
			err = restore_err;
			return false;
		}
		if (ledger_found &&
		    reviewed.session_id == runtime_task->session_id) {
			recovered_checkpoint_reconciled =
			    webcool::ai::remove_resolved_agent_review_changes(
			        reviewed.changes, changes) > 0;
		}
	}
	const std::vector<agent_change_proposal_t> checkpoint_reviews = changes;
	if (webcool::ai::remove_resolved_agent_review_changes(
	        checkpoint_reviews, changes) > 0)
		recovered_checkpoint_reconciled = true;
	// Old checkpoints lack a ledger ID: only a complete file/operation
	// match is sufficient to retire a proposal. Never infer from a fragment.
	for (size_t i = 0; i < changes.size();) {
		webcool::ai::agent_result_t single;
		single.changes.push_back(changes[i]);
		std::string match_err;
		if (verify_result_changes_applied(
		        runtime_task->user_root, single, match_err)) {
			changes.erase(changes.begin() + i);
			recovered_checkpoint_reconciled = true;
			continue;
		}
		// Do not silently overwrite the checkpoint baseline when the user
		// edited the formal file while this run was stopped.
		if (changes[i].operation == "write" &&
		    !changes[i].creates_file && !changes[i].base_hash.empty()) {
			std::string current;
			bool truncated = false;
			if (!workspace.read(changes[i].path, current, truncated,
			        restore_err) ||
			    truncated ||
			    webcool::ai::agent_workspace_t::content_sha256(
			        current) != changes[i].base_hash) {
				err = "恢复修订存在版本冲突，请先审查该文件：" +
				    changes[i].path;
				return false;
			}
		}
		++i;
	}
	if (provider.protocol == "openai_responses") {
		provider_response_id = restored.provider_response_id;
		provider_response_pending = restored.provider_response_pending;
		for (size_t i = 0; i < restored.provider_tool_outputs.size();
		     ++i) {
			webcool::ai::completion_tool_output_t item;
			item.call_id =
			    restored.provider_tool_outputs[i].call_id;
			item.output = restored.provider_tool_outputs[i].output;
			provider_tool_outputs.push_back(item);
		}
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		runtime_task->provider_response_id = provider_response_id;
		runtime_task->provider_response_pending =
		    provider_response_pending;
		runtime_task->provider_tool_outputs = provider_tool_outputs;
	}
	if (!changes.empty()) {
		rejected_changes =
		    validate_change_proposals(workspace, project_path, changes);
		size_t skipped_files = 0;
		if (!materialize_staged_worktree(runtime_task->user_root,
		        project_path, runtime_task->id, changes, skipped_files,
		        restore_err)) {
			webcool::ai::ai_log_error("agent.runtime",
			    "restore-draft-worktree", restore_err);
			err = restore_err;
			return false;
		}
		publish_runtime_staged_changes(runtime_task, changes);
	}
	return true;
}

bool coding_tool_loop_t::restore_checkpoint()
{
	changes.clear();
	if (initial_completed_tool_calls > 0) {
		webcool::ai::agent_progress_t restored;
		bool found = false;
		std::string restore_err;
		if (!progress_store.load(restored, found, restore_err)) {
			webcool::ai::ai_log_error("agent.runtime",
			    "restore-staged-progress", restore_err);
			err = restore_err;
			return false;
		}

		if ((found && restored.provider_id == provider.id &&
		        (restored.provider_model.empty() ||
		            restored.provider_model == provider.model) &&
		        restored.project_path == project_path &&
		        restored.original_prompt == original_prompt) &&
		    (!restore_checkpoint_contents(restored, restore_err)))
			return false;
	}
	if (initial_completed_tool_calls > 0 && changes.empty() &&
	    !recovered_checkpoint_reconciled) {
		// Empty pending changes do not imply stalled analysis or verification.
		const std::string recovery_guard =
		    prompt_text(prompt_id::recovery_progress, chinese);
		persistent_transcript += recovery_guard;
		transcript += recovery_guard;
		provider_history_base = transcript;
	} else if (recovered_checkpoint_reconciled) {
		// Accepted/rejected work is external progress, not an empty discovery loop.
		// Start the guard from a clean baseline and explicitly supersede any older
		// recovery instruction retained inside the compacted transcript.
		progress_supervisor.reset_after_external_progress();
		const std::string reconciled_guard =
		    prompt_text(prompt_id::recovery_baseline, chinese);
		persistent_transcript += reconciled_guard;
		transcript += reconciled_guard;
		provider_history_base = transcript;
		acl::json reconciled_json;
		acl::json_node &reconciled_event =
		    reconciled_json.create_node();
		reconciled_event.add_text(
		    "event", "recovery_baseline_reconciled");
		reconciled_event.add_number("completed_tool_calls",
		    static_cast<long long>(initial_completed_tool_calls));
		append_runtime_operation_event(runtime_task, reconciled_event);
	}
	// Recovered edits were materialized above. Fresh read-only turns need no
	// private tree: proposal persistence and validation create it on demand.
	if (changes.empty())
		append_simple_operation_event(runtime_task,
		    "draft_materialization_deferred", "preparing", "",
		    initial_completed_tool_calls);
	memory_summary.clear();
	completion_summary.clear();
	session_title.clear();
	return true;
}

void coding_tool_loop_t::validate_recovered_draft()
{
	if (initial_completed_tool_calls > 0 && !changes.empty()) {
		agent_tool_trace_t recovery_validation_trace;
		recovery_validation_trace.name = "workspace.validate";
		recovery_validation_trace.path = project_path;
		const std::string recovery_validation = validate_staged_draft(
		    runtime_task->user_root, project_path, runtime_task->id,
		    changes, sandbox_limits, recovery_validation_trace);
		traces.push_back(recovery_validation_trace);
		const bool validation_passed =
		    recovery_validation.find("\"validation_passed\":true") !=
		    std::string::npos;
		const bool validation_unavailable =
		    recovery_validation.find("\"commands_executed\":0") !=
		    std::string::npos;
		last_validation_report = recovery_validation;
		record_acceptance_evidence(recovery_validation);
		last_validated_fingerprint = staged_change_fingerprint(changes);
		last_validation_passed = validation_passed;
		if (validation_unavailable) {
			unavailable_validation.fingerprint =
			    last_validated_fingerprint;
			unavailable_validation.report = recovery_validation;
		}
		// Preserve one bounded diagnostic for repair. The restored staged draft is
		// still authoritative and remains visible in the review tree.
		const std::string recovery_addition =
		    "\n\n<draft_validation>\n" + recovery_validation +
		    "\n</draft_validation>\n" +
		    (validation_passed ?
		            prompt_text(prompt_id::recovery_validation_passed,
		                chinese) :
		            validation_unavailable ?
		            prompt_text(
		                prompt_id::recovery_validation_unavailable,
		                chinese) :
		            prompt_text(prompt_id::recovery_validation_failed,
		                chinese)) +
		    validation_repair_context(runtime_task->user_root,
		        project_path, runtime_task->id, changes,
		        recovery_validation, 48 * 1024);
		persistent_transcript =
		    compact_coding_transcript(initial_prompt, changes, traces,
		        recovery_addition, chinese);
		transcript = persistent_transcript;
		provider_history_base = transcript;
		recovery_repair_only = false;
	}
	// Recheck previously unavailable validators once. Tool availability alone
	// must neither terminate the user's task nor force a speculative code edit.
	const bool recovered_without_validator =
	    initial_completed_tool_calls > 0 &&
	    last_validation_report.empty() &&
	    persistent_transcript.find("\"name\":\"workspace.validate\"") !=
	        std::string::npos &&
	    persistent_transcript.find("\"commands_executed\":0") !=
	        std::string::npos;
	if (recovered_without_validator) {
		// The administrator may have enabled the toolchain after this checkpoint
		// was written. Re-probe locally without another paid provider request.
		agent_tool_trace_t recovery_validation_trace;
		recovery_validation_trace.name = "workspace.validate";
		recovery_validation_trace.path = project_path;
		const std::string recovery_validation = validate_staged_draft(
		    runtime_task->user_root, project_path, runtime_task->id,
		    changes, sandbox_limits, recovery_validation_trace);
		traces.push_back(recovery_validation_trace);
		const bool still_unavailable =
		    recovery_validation.find("\"commands_executed\":0") !=
		    std::string::npos;
		const bool now_valid =
		    recovery_validation.find("\"validation_passed\":true") !=
		    std::string::npos;
		last_validation_report = recovery_validation;
		record_acceptance_evidence(recovery_validation);
		last_validated_fingerprint = staged_change_fingerprint(changes);
		last_validation_passed = now_valid;
		// A newly available validator found real compiler/test failures. Give the
		// model the bounded diagnostics once so it can repair code instead of
		// resuming the stale file-reading loop preserved in the checkpoint.
		const std::string recovery_addition =
		    "\n\n<draft_validation>\n" + recovery_validation +
		    "\n</draft_validation>\n" +
		    (still_unavailable ?
		            prompt_text(
		                prompt_id::validation_unavailable, chinese) :
		            now_valid ?
		            prompt_text(prompt_id::validation_passed, chinese) :
		            prompt_text(
		                prompt_id::validation_failed, chinese)) +
		    validation_repair_context(runtime_task->user_root,
		        project_path, runtime_task->id, changes,
		        recovery_validation, 48 * 1024);
		persistent_transcript =
		    compact_coding_transcript(initial_prompt, changes, traces,
		        recovery_addition, chinese);
		transcript = persistent_transcript;
		provider_history_base = transcript;
	}
}

bool coding_tool_loop_t::prepare_model_tools()
{
	const webcool::ai::agent_definition_t *coding_agent =
	    webcool::ai::agent_registry_t::instance().find("coding");
	if (coding_agent == NULL) {
		err = "coding agent definition is unavailable";
		webcool::ai::ai_log_error(
		    "agent.runtime", "load-tool-registry", err);
		return false;
	}
	for (size_t i = 0; i < coding_agent->tools.size(); ++i) {
		const webcool::ai::agent_tool_t &tool = coding_agent->tools[i];
		if (!tool.model_enabled)
			continue;
		if (tool.name == "workspace.create" ||
		    tool.name == "workspace.mkdir") {
			continue;
		}
		if (tool.requires_file_content && !provider.allow_file_content)
			continue;
		model_tools.push_back(tool);
		if (!(tool.authorization == "proposal"))
			continue;
		proposal_tools.push_back(tool);
	}
	proposal_only_turn = recovery_repair_only;
	stateless_responses =
	    webcool::ai::provider_client_t::responses_are_stateless(provider);
	if (stateless_responses) {
		// A stale checkpoint from the earlier stateful implementation can contain
		// an ID/output pair which the stateless provider can never resolve. The durable text
		// transcript already contains the completed tool result, so discard only
		// the incompatible native continuation fields.
		provider_response_id.clear();
		provider_response_pending = false;
		provider_tool_outputs.clear();
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		runtime_task->provider_response_id.clear();
		runtime_task->provider_response_pending = false;
		runtime_task->provider_tool_outputs.clear();
	}
	if (save_coding_progress(progress_store, provider, runtime_task,
	        project_path, original_prompt, persistent_transcript,
	        accumulated_reasoning, "", initial_completed_tool_calls, err))
		return true;
	return false;
}

void coding_tool_loop_t::restore_context()
{
	restore_read_context(persistent_transcript, read_context);
	// A checkpoint can outlive edits made by another browser or a restart.
	// Never reuse its pages without comparing them with the current baseline.
	for (const auto &file : read_context.versions()) {
		std::string current, read_err;
		bool truncated = false;
		if (!(!workspace.read(
		          file.first, current, truncated, read_err) ||
		        truncated ||
		        webcool::ai::agent_workspace_t::content_sha256(
		            current) != file.second))
			continue;
		read_context.erase(file.first);
	}
	for (const auto &change : changes) {
		read_context.erase(change.path);
		if (change.target_path.empty())
			continue;
		read_context.erase(change.target_path);
	}
	// Keep assistant's visible findings separately so large source reads cannot
	// evict the task decision along with the oldest tool result.
	// Rehydrate the same bounded findings after a checkpoint resume.
	for (size_t offset = 0; offset < persistent_transcript.size();) {
		const size_t begin =
		    persistent_transcript.find("<assistant_findings>", offset);
		if (begin == std::string::npos)
			break;
		const size_t end =
		    persistent_transcript.find("</assistant_findings>", begin);
		if (end == std::string::npos)
			break;
		offset = end + std::string("</assistant_findings>").size();
		recent_findings.append(
		    persistent_transcript.substr(begin, offset - begin));
	}
}

static void append_validation_commands(
    acl::json_node &commands, const webcool::ai::project_toolchain_t &available)
{
	for (const auto &command : available.commands) {
		if (!(command.id.compare(0, 4, "git.") != 0))
			continue;
		commands.add_array_text(command.id.c_str());
	}
}

void coding_tool_loop_t::discover_validation_capabilities()
{
	if (!runtime_task->text_preview && !runtime_task->assistant_chat) {
		webcool::ai::project_toolchain_t available;
		std::string discovery_error;
		webcool::ai::project_toolchain_catalog_t catalog(
		    runtime_task->user_root);
		acl::json capability_json;
		acl::json_node &capability = capability_json.create_node();
		const bool discovered =
		    catalog.discover(project_path, available, discovery_error);
		capability.add_bool("discovery_ok", discovered);
		capability.add_bool("build_network_enabled",
		    webcool::ai::ai_runtime_policy_get().allow_build_network);
		if (discovered) {
			capability.add_bool("node_acceptance_policy_enabled",
			    available.functional_acceptance_enabled);
			acl::json_node &commands =
			    capability_json.create_array();
			capability.add_child("commands", commands);
			append_validation_commands(commands, available);
			acl::json_node &unavailable =
			    capability_json.create_array();
			capability.add_child("unavailable_tools", unavailable);
			for (const auto &tool : available.unavailable_tools)
				unavailable.add_array_text(tool.c_str());
		}
		validation_capabilities =
		    prompt_with_value(prompt_id::coding_validation_capabilities,
		        chinese, capability.to_string().c_str());
	}
	transport_session =
	    webcool::ai::provider_client_t::create_transport_session();
	read_batch_argument_errors =
	    persistent_transcript.find(
	        "read_batch content must be a JSON array") !=
	        std::string::npos ?
	    1 :
	    0;
}

} // namespace agent_detail
} // namespace action
