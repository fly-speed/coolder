#include "coding_internal.h"
#include "coding_records.h"
#include "../sandbox/sandbox_execution.h"
#include <map>
#include <mutex>

namespace webcool
{
namespace ai
{
namespace coding_detail
{
namespace
{
std::mutex active_mutex;
std::map<std::string, std::shared_ptr<std::atomic<bool>>> active;

bool execute(const std::string &root, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	if (!boolean(a, "confirm", false)) {
		error = "sandbox execution requires explicit confirmation";
		return false;
	}
	sandbox_execution_store_t store(root);
	sandbox_execution_plan_t plan;
	if (!store.consume(text(a, "id"), plan, error))
		return false;
	const auto key = root + ":" + plan.id;
	auto cancel = std::make_shared<std::atomic<bool>>(false);
	{
		std::lock_guard<std::mutex> lock(active_mutex);
		active[key] = cancel;
	}
	struct lease_t {
		std::string key;
		~lease_t()
		{
			std::lock_guard<std::mutex> lock(active_mutex);
			active.erase(key);
		}
	} lease{ key };
	program_sandbox_t sandbox(root, plan.project_path, { plan.command },
	    plan.limits, text(a, "helper"));
	sandbox_request_t request;
	request.command_id = plan.command.id;
	sandbox_result_t result;
	if (!store.audit_started(plan, error))
		return false;
	const bool ok = sandbox.execute(request, result, cancel.get());
	std::string audit_error;
	store.audit_finished(plan, result, audit_error);
	out.add_text("id", plan.id.c_str())
	    .add_text("project_path", plan.project_path.c_str())
	    .add_text("command_id", plan.command.id.c_str())
	    .add_bool("started", result.started)
	    .add_bool("cancelled", result.cancelled)
	    .add_bool("timed_out", result.timed_out)
	    .add_bool("output_truncated", result.output_truncated)
	    .add_number("exit_code", result.exit_code)
	    .add_number("elapsed_ms", result.elapsed_ms)
	    .add_text("stdout", result.standard_output.c_str())
	    .add_text("stderr", result.standard_error.c_str());
	error = result.error.empty() ? audit_error : result.error;
	return ok && audit_error.empty();
}
}

bool sandbox(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error)
{
	if (op == "sandbox.capabilities") {
		std::string reason;
		out.add_bool("available", program_sandbox_t::backend_available(reason))
		    .add_text("reason", reason.c_str());
		return true;
	}
	if (op == "sandbox.execute")
		return execute(root, a, out, error);
	if (op == "sandbox.cancel") {
		std::lock_guard<std::mutex> lock(active_mutex);
		auto found = active.find(root + ":" + text(a, "id"));
		if (found == active.end()) {
			error = "sandbox execution is not active";
			return false;
		}
		found->second->store(true);
		return true;
	}
	return false;
}
}
}
}
