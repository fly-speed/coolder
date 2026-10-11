#include "coding_service.h"
#include "coding_internal.h"
#include "../agent/agent_registry.h"
#include <sys/stat.h>
#ifdef _WIN32
#include "../common/platform_compat.h"
#endif
namespace webcool
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
		    root + "/.webcool_agent/external-project-roots.v1";
		if (stat(registry.c_str(), &info) == 0)
			throw std::invalid_argument(
			    "CodingService requires a scope without external project mounts");
		if (arguments.size() > 8 * 1024 * 1024 ||
		    arguments.find('\0') != std::string::npos ||
		    arguments.find("\\u0000") != std::string::npos)
			throw std::invalid_argument(
			    "arguments exceed size limit or contain NUL");
		acl::json input(arguments.c_str());
		if (!input.finish() || !input.get_root().is_object())
			throw std::invalid_argument(
			    "arguments must be a JSON object");
		auto &a = input.get_root();
		if (operation == "capabilities") {
			coding_detail::strings(out, "operations",
			    { "capabilities", "agents.list", "workspace.list",
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
			        "sandbox.preview", "providers.list",
			        "providers.save", "providers.remove",
			        "providers.complete" });
			ok = true;
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
