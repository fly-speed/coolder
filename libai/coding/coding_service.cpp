#include "coding_service.h"
#include "coding_internal.h"
#include "../common/legacy_branding.h"
#include "task_contract_json.h"
#include "../agent/agent_registry.h"
#include "../sdk/json_guard.h"
#include "../runtime/coding_runtime.h"
#include "../runtime/browser_debug.h"
#include "../sandbox/program_sandbox.h"
#include <sys/stat.h>
#ifdef _WIN32
#include "../common/platform_compat.h"
#endif
namespace libai
{
namespace ai
{
namespace
{
void agents(acl::json_node &out)
{
	auto &array = coding_detail::array(out, "agents");
	for (const auto &agent : agent_registry_t::instance().list()) {
		auto &item = array.add_child(false, true);
		item.add_text("id", agent.id.c_str())
		    .add_text("name", agent.name.c_str())
		    .add_text("description", agent.description.c_str())
		    .add_text("version", agent.version.c_str());
		auto &tools = coding_detail::array(item, "tools");
		for (const auto &tool : agent.tools) {
			auto &entry = tools.add_child(false, true);
			entry.add_text("name", tool.name.c_str())
			    .add_text("description", tool.description.c_str())
			    .add_bool("mutating", tool.mutating)
			    .add_bool("model_enabled", tool.model_enabled)
			    .add_text(
			        "authorization", tool.authorization.c_str());
		}
	}
}
}
std::string coding_service_call(const std::string &root,
    const std::string &operation, const std::string &arguments)
{
	acl::json response;
	auto &out = response.create_node();
	bool ok = false;
	std::string error;
	try {
		struct stat info;
		if (root.empty() || root.find('\0') != std::string::npos ||
		    stat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
			throw std::invalid_argument(
			    "root must be an existing trusted directory");
		// This bridge deliberately does not inherit account-level external mounts.
		const auto registry =
		    root + "/.libai_agent/external-project-roots.v1";
		if (operation != "storage.migrate" && stat(registry.c_str(), &info) == 0)
			throw std::invalid_argument(
			    "CodingService requires a scope without external project mounts");
		if (operation != "tasks.evaluate" && (arguments.size() > (operation == "providers.complete" ?
		        32 * 1024 * 1024 : 8 * 1024 * 1024) ||
		    arguments.find('\0') != std::string::npos ||
		    arguments.find("\\u0000") != std::string::npos))
			throw std::invalid_argument(
			    "arguments exceed size limit or contain NUL");
		acl::json input(arguments.c_str());
		if (!input.finish() || !input.get_root().is_object())
			throw std::invalid_argument(
			    "arguments must be a JSON object");
		auto &a = input.get_root();
		if(operation=="sandbox.confirm") {
			libai_schema_json::Document strict;
			libai::governance_detail::parse_json_bounded(arguments,strict,16384);
			if(!strict.IsObject()||strict.MemberCount()!=5 || !strict.HasMember("result")||!strict["result"].IsObject())
				throw std::invalid_argument("sandbox.confirm requires five host fields");
			for(const auto *name:{"idempotency_key","revision","outcome","reference"})
				if(!strict.HasMember(name)||!strict[name].IsString())throw std::invalid_argument("invalid sandbox.confirm field");
		}
		if(operation=="sandbox.inspect" || operation=="batches.inspect" || operation=="browser.receipt.inspect") {
			libai_schema_json::Document strict;
			libai::governance_detail::parse_json_bounded(arguments,strict,4096);
			if(!strict.IsObject()||strict.MemberCount()!=1||!strict.HasMember("idempotency_key")||!strict["idempotency_key"].IsString())
				throw std::invalid_argument("inspection requires idempotency_key only");
		}
        if(operation=="sandbox.parent.inspect") {
            libai_schema_json::Document strict;libai::governance_detail::parse_json_bounded(arguments,strict,4096);
            if(!strict.IsObject()||strict.MemberCount()!=2||!strict.HasMember("idempotency_key")||!strict["idempotency_key"].IsString()||
                !strict.HasMember("project_path")||!strict["project_path"].IsString())throw std::invalid_argument("sandbox.parent.inspect requires key and project strings");
        }
		if (operation == "sandbox.execute") {
			libai_schema_json::Document strict;
			libai::governance_detail::parse_json_bounded(arguments, strict, 4096);
			if (!strict.IsObject())
				throw std::invalid_argument("sandbox.execute requires an object");
			for (auto it = strict.MemberBegin(); it != strict.MemberEnd(); ++it) {
				const std::string name(it->name.GetString(), it->name.GetStringLength());
				if (name != "id" && name != "confirm" && name != "helper" && name != "idempotency_key" && name != "parent_idempotency_key")
					throw std::invalid_argument("unknown sandbox.execute argument");
				if (name == "confirm" ? !it->value.IsBool() : !it->value.IsString())
					throw std::invalid_argument("invalid sandbox.execute argument type");
				if ((name == "idempotency_key" || name == "parent_idempotency_key") &&
				    !program_sandbox_t::valid_idempotency_key(std::string(it->value.GetString(), it->value.GetStringLength())))
					throw std::invalid_argument("invalid sandbox.execute idempotency key");
			}
		}
		if (operation == "progress.inspect") {
			libai_schema_json::Document strict;
			libai::governance_detail::parse_json_bounded(arguments, strict, 4096);
			if (!strict.IsObject() || strict.MemberCount() != 2 ||
			    !strict.HasMember("project_path") || !strict["project_path"].IsString() ||
			    !strict.HasMember("session_id") || !strict["session_id"].IsString())
				throw std::invalid_argument("progress.inspect requires project_path and session_id strings only");
		}
		if (operation == "capabilities") {
			coding_detail::strings(out, "operations",
			    { "capabilities", "tasks.evaluate", "tasks.revision", "tasks.describe_run", "progress.inspect", "progress.confirm", "storage.migrate", "storage.canonical_path", "agents.list", "workspace.list",
			        "workspace.read", "workspace.search",
			        "workspace.outline", "workspace.sha256",
			        "workspace.create", "workspace.replace",
			        "workspace.delete", "workspace.mkdir",
			        "workspace.rmdir", "patch.preview",
			        "patch.apply", "changes.preview",
			        "changes.apply", "changes.recover",
			        "projects.list", "projects.get",
			        "projects.create", "projects.remove",
			        "projects.plan", "projects.task_status",
			        "projects.scaffold", "projects.toolchain",
			        "projects.diagnostics", "sessions.list",
			        "sessions.get", "sessions.create",
			        "sessions.remove", "sessions.update",
			        "runs.list", "runs.get", "runs.create",
			        "runs.complete", "runs.fail",
			        "runs.audit_cancel", "sandbox.history",
			        "sandbox.preview", "sandbox.capabilities",
			        "batches.confirm", "browser.receipt.confirm", "browser.receipt.inspect", "batches.inspect", "sandbox.parent.inspect", "sandbox.execute", "sandbox.inspect", "sandbox.confirm", "sandbox.cancel",
			        "runtime.policy.get", "runtime.policy.validate",
			        "runtime.policy.configure", "providers.list",
			        "providers.save", "providers.remove", "providers.test",
			        "providers.complete" });
			ok = true;
		} else if (operation == "browser.receipt.confirm") {
            ok = browser_debug_confirm(root, arguments, out, error);
        } else if (operation == "browser.receipt.inspect") {
            ok = browser_debug_inspect(root, coding_detail::text(a,"idempotency_key"), out, error);
        } else if (operation == "batches.confirm") {
            ok = action::agent_detail::confirm_workspace_batch_child(root, arguments, out, error);
        } else if (operation == "batches.inspect") {
            ok = action::agent_detail::inspect_workspace_batch_receipt(root, coding_detail::text(a,"idempotency_key"), out, error);
        } else if (operation == "progress.confirm") {
			ok = coding_detail::confirm_progress(root, arguments, out, error);
		} else if (operation == "tasks.evaluate") {
			ok = coding_detail::task_contract_raw(arguments, out);
		} else if (operation == "tasks.revision" || operation == "tasks.describe_run") {
			ok = coding_detail::tasks(root, operation, a, out, error);
		} else if (operation == "storage.canonical_path") {
            out.add_text("path",legacy_branding::canonical_path(coding_detail::text(a,"path")).c_str());ok=true;
        } else if (operation == "storage.migrate") {
			ok = coding_detail::migrate(root, a, out, error);
		} else if (operation.compare(0, 15, "runtime.policy.") == 0) {
			ok = coding_detail::runtime(operation, a, out, error);
		} else if (operation == "agents.list") {
			agents(out);
			ok = true;
		} else if (operation.compare(0, 10, "workspace.") == 0 ||
		    operation.compare(0, 6, "patch.") == 0 ||
		    operation.compare(0, 8, "changes.") == 0)
			ok = coding_detail::workspace(
			    root, operation, a, out, error);
		else if (operation.compare(0, 9, "projects.") == 0)
			ok = coding_detail::projects(
			    root, operation, a, out, error);
		else if (operation.compare(0, 10, "providers.") == 0)
			ok = coding_detail::providers(
			    root, operation, a, out, error);
		else
			ok = coding_detail::storage(
			    root, operation, a, out, error);
	} catch (const std::exception &e) {
		error = e.what();
	} catch (...) {
		error = "coding operation failed";
	}
	out.add_bool("ok", ok);
	if (!ok)
		out.add_text("error",
		    error.empty() ? "unknown coding operation" : error.c_str());
	return out.to_string().c_str();
}
}
}
