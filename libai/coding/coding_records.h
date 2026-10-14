#pragma once
#include "coding_internal.h"
#include "../storage/run_timing.h"
#include "../storage/agent_session_store.h"
#include "../storage/agent_run_store.h"
#include "../sandbox/sandbox_execution.h"
namespace webcool
{
namespace ai
{
namespace coding_detail
{
// Encode non-secret persistent sandbox_run_history_t metadata for every language binding.
inline void record(acl::json_node &out, const sandbox_run_history_t &p)
{
	out.add_text("idempotency_key", p.idempotency_key.c_str());
	out.add_text("id", p.id.c_str());
	out.add_text("project_path", p.project_path.c_str());
	out.add_text("command_id", p.command_id.c_str());
	out.add_text("status", p.status.c_str());
	out.add_number("started_at", static_cast<long long>(p.started_at));
	out.add_number("finished_at", static_cast<long long>(p.finished_at));
	out.add_number("exit_code", static_cast<long long>(p.exit_code));
	out.add_number("signal", static_cast<long long>(p.signal));
	out.add_number("elapsed_ms", static_cast<long long>(p.elapsed_ms));
	out.add_bool("cancelled", p.cancelled);
	out.add_bool("timed_out", p.timed_out);
	out.add_bool("output_truncated", p.output_truncated);
	out.add_text("error", p.error.c_str());
}
// Encode non-secret persistent agent_run_record_t metadata for every language binding.
inline void record(acl::json_node &out, const agent_run_record_t &p)
{
	out.add_text("id", p.id.c_str());
	out.add_text("agent_id", p.agent_id.c_str());
	out.add_text("agent_version", p.agent_version.c_str());
	out.add_text("status", p.status.c_str());
	out.add_text("provider_id", p.provider_id.c_str());
	out.add_text("model", p.model.c_str());
	out.add_text("project_path", p.project_path.c_str());
	out.add_number("started_at", static_cast<long long>(p.started_at));
	out.add_number("finished_at", static_cast<long long>(p.finished_at));
	const auto timing = run_timing(p.started_at, p.finished_at, p.status);
	out.add_number("elapsed_ms", timing.elapsed_ms);
	out.add_bool("elapsed_available", timing.available);
	out.add_number("elapsed_resolution_ms", 1000);
	out.add_text("elapsed_source", "unix_seconds");
	out.add_number("input_tokens", static_cast<long long>(p.input_tokens));
	out.add_number("cached_input_tokens",
	    static_cast<long long>(p.cached_input_tokens));
	out.add_number(
	    "output_tokens", static_cast<long long>(p.output_tokens));
	out.add_number(
	    "reasoning_tokens", static_cast<long long>(p.reasoning_tokens));
	out.add_number("latency_ms", static_cast<long long>(p.latency_ms));
	out.add_number("tool_calls", static_cast<long long>(p.tool_calls));
	out.add_number(
	    "proposed_changes", static_cast<long long>(p.proposed_changes));
	out.add_number(
	    "rejected_changes", static_cast<long long>(p.rejected_changes));
	out.add_text(
	    "provider_error_category", p.provider_error_category.c_str());
	out.add_number("provider_http_status",
	    static_cast<long long>(p.provider_http_status));
	out.add_bool("provider_error_retryable", p.provider_error_retryable);
	out.add_text("error", p.error.c_str());
}
// Encode non-secret persistent agent_session_message_t metadata for every language binding.
inline void record(acl::json_node &out, const agent_session_message_t &p)
{
	out.add_text("role", p.role.c_str());
	out.add_text("text", p.text.c_str());
	out.add_text("reasoning", p.reasoning.c_str());
	out.add_text("completion_summary", p.completion_summary.c_str());
	out.add_text("run_id", p.run_id.c_str());
	out.add_text("state", p.state.c_str());
	out.add_number("created_at", static_cast<long long>(p.created_at));
	out.add_number("duration_ms", static_cast<long long>(p.duration_ms));
	out.add_number("input_tokens", static_cast<long long>(p.input_tokens));
	out.add_number("cached_input_tokens",
	    static_cast<long long>(p.cached_input_tokens));
	out.add_number(
	    "output_tokens", static_cast<long long>(p.output_tokens));
	out.add_number(
	    "reasoning_tokens", static_cast<long long>(p.reasoning_tokens));
}
// Encode non-secret persistent agent_session_record_t metadata for every language binding.
inline void record(acl::json_node &out, const agent_session_record_t &p)
{
	out.add_text("id", p.id.c_str());
	out.add_text("title", p.title.c_str());
	out.add_text("agent_id", p.agent_id.c_str());
	out.add_text("provider_id", p.provider_id.c_str());
	out.add_text("project_path", p.project_path.c_str());
	out.add_text("summary", p.summary.c_str());
	auto &array = coding_detail::array(out, "messages");
	for (const auto &m : p.messages)
		record(array.add_child(false, true), m);
	out.add_text("last_run_id", p.last_run_id.c_str());
	out.add_number("created_at", static_cast<long long>(p.created_at));
	out.add_number("updated_at", static_cast<long long>(p.updated_at));
	out.add_number("turn_count", static_cast<long long>(p.turn_count));
}
}
}
}
