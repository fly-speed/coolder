#include "coding_records.h"
#include "../workspace/agent_workspace.h"
namespace webcool
{
namespace ai
{
namespace coding_detail
{
namespace
{
bool sessions(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	agent_session_store_t store(root);
	agent_session_record_t p;
	if (op == "sessions.list") {
		std::vector<agent_session_record_t> entries;
		if (!store.list(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "sessions");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "sessions.remove")
		return store.remove(text(a, "id"), error);
	if (op == "sessions.update")
		return store.update_after_run(text(a, "id"), text(a, "title"),
		    text(a, "summary"), text(a, "last_run_id"),
		    text(a, "user_prompt"), text(a, "assistant_reply"),
		    text(a, "assistant_state"), text(a, "assistant_reasoning"),
		    text(a, "completion_summary"), number(a, "duration_ms"),
		    number(a, "input_tokens"), number(a, "cached_input_tokens"),
		    number(a, "output_tokens"), number(a, "reasoning_tokens"),
		    error);
	bool ok = false;
	if (op == "sessions.get")
		ok = store.get(text(a, "id"), p, error);
	else if (op == "sessions.create") {
		const auto path = text(a, "project_path");
		std::string normalized;
		if (!agent_workspace_t::normalize_path(
		        path, normalized, true, error))
			return false;
		ok = store.create(text(a, "title"), text(a, "agent_id"),
		    text(a, "provider_id"), normalized, p, error);
	}
	if (ok)
		record(out, p);
	return ok;
}
bool runs(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	agent_run_store_t store(root);
	if (op == "runs.list") {
		std::vector<agent_run_record_t> entries;
		if (!store.list(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "runs");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "runs.get") {
		agent_run_record_t p;
		if (!store.get(text(a, "id"), p, error))
			return false;
		record(out, p);
		return true;
	}
	if (op == "runs.audit_cancel")
		return store.cancel(text(a, "id"), error);
	if (op == "runs.fail")
		return store.fail(text(a, "id"), text(a, "error"), error);
	if (op == "runs.complete")
		return store.complete(text(a, "id"), number(a, "input_tokens"),
		    number(a, "cached_input_tokens"),
		    number(a, "output_tokens"), number(a, "reasoning_tokens"),
		    number(a, "latency_ms"), number(a, "tool_calls"),
		    number(a, "proposed_changes"),
		    number(a, "rejected_changes"), error);
	if (op == "runs.create") {
		agent_run_record_t p;
		p.id = text(a, "id");
		p.agent_id = text(a, "agent_id");
		p.agent_version = text(a, "agent_version");
		p.provider_id = text(a, "provider_id");
		p.model = text(a, "model");
		p.project_path = text(a, "project_path");
		p.started_at = number(a, "started_at");
		p.status = "running";
		return store.create(p, error);
	}
	return false;
}
}
bool storage(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	if (op.compare(0, 9, "sessions.") == 0)
		return sessions(root, op, a, out, error);
	if (op.compare(0, 5, "runs.") == 0)
		return runs(root, op, a, out, error);
	sandbox_execution_store_t store(root);
	if (op == "sandbox.history") {
		std::vector<sandbox_run_history_t> entries;
		if (!store.list_history(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "runs");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	if (op == "sandbox.preview") {
		const auto path = text(a, "project_path");
		std::string absolute;
		if (!agent_workspace_t::resolve_project_root(
		        root, path, absolute, error, false))
			return false;
		sandbox_execution_plan_t p;
		if (!store.create(path, text(a, "command_id"), p, error))
			return false;
		out.add_text("id", p.id.c_str())
		    .add_text("project_path", p.project_path.c_str())
		    .add_text("command_id", p.command.id.c_str())
		    .add_number("timeout_ms", p.limits.timeout_ms);
		return true;
	}
	return false;
}
}
}
}
