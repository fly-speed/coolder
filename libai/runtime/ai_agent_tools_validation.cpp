#include "stdafx.h"
#include "ai_agent_tools_internal.h"

namespace action { namespace agent_detail {

namespace workspace_tool_detail {

std::string execute_workspace_validate(workspace_tool_context_t& context)
{
	// Establishing a clean baseline is useful before the first edit. Validation
	// still runs in the private worktree and never mutates formal project files.
	const std::vector<agent_change_proposal_t> empty_changes;
	const std::vector<agent_change_proposal_t>& validation_changes =
		context.staged_changes == NULL ? empty_changes : *context.staged_changes;
	if (context.unavailable_validation != NULL && context.unavailable_validation->matches(
		staged_change_fingerprint(validation_changes))) {
		context.trace.ok = true;
		return context.unavailable_validation->report;
	}
	// No cached report means there is nothing to compare. Materialization
	// already computes the source fingerprint; reuse that result below.
	const std::string baseline = context.validation_cache && !context.validation_cache->report.empty()
		? validation_baseline_fingerprint(context.workspace, context.project_path) : "";
	const std::string fingerprint = baseline + "\n" + staged_change_fingerprint(validation_changes);
	if (context.validation_cache && !baseline.empty() && !context.validation_cache->report.empty()
		&& context.validation_cache->fingerprint == fingerprint
		&& !context.validation_cache->draft_fingerprint.empty()
		&& context.validation_cache->draft_fingerprint == validation_baseline_fingerprint(context.draft_workspace, "")) {
		acl::json cached(context.validation_cache->report.c_str());
		cached.get_root().add_bool("validation_reused", true);
		cached.get_root().add_bool("executed_this_call", false);
		cached.get_root().add_text("reuse_note",
			prompt_text(json_bool(cached["validation_passed"], false) ? prompt_id::validation_success_reused : prompt_id::validation_next_step, context.chinese));
		context.trace.ok = true;
		return serialize_json(cached.get_root());
	}
	std::string validated_source;
	const std::string report = validate_staged_draft(context.user_root, context.project_path, context.run_id,
		validation_changes, context.sandbox_limits, context.trace, &validated_source);
	if (context.validation_cache) {
		context.validation_cache->report.clear();
		if (!validated_source.empty() && context.trace.ok && report.find("\"validation_passed\":") != std::string::npos
			&& report.find("\"commands_executed\":0") == std::string::npos) {
			context.validation_cache->fingerprint = validated_source + "\n" + staged_change_fingerprint(validation_changes);
			context.validation_cache->draft_fingerprint = validation_baseline_fingerprint(context.draft_workspace, "");
			context.validation_cache->report = report;
		}
	}
	return report;
}

} // namespace workspace_tool_detail

} }
