#include "coding_internal.h"
#include "../project/agent_project_store.h"
#include "../project/project_scaffold.h"
#include "../project/project_toolchain.h"
#include "../project/project_diagnostics.h"
#include "../workspace/agent_workspace.h"
namespace webcool
{
namespace ai
{
namespace coding_detail
{
namespace
{
void record(acl::json_node &out, const agent_project_record_t &p)
{
	out.add_text("id", p.id.c_str())
	    .add_text("title", p.title.c_str())
	    .add_text("project_path", p.project_path.c_str())
	    .add_text("language", p.language.c_str())
	    .add_text("platform", p.platform.c_str())
	    .add_text("goal", p.goal.c_str())
	    .add_text("status", p.status.c_str())
	    .add_number("plan_version", p.plan_version)
	    .add_number("created_at", p.created_at)
	    .add_number("updated_at", p.updated_at);
	auto &modules = coding_detail::array(out, "modules");
	for (const auto &m : p.modules) {
		auto &n = modules.add_child(false, true);
		n.add_text("id", m.id.c_str())
		    .add_text("name", m.name.c_str())
		    .add_text("layer", m.layer.c_str())
		    .add_text("path", m.path.c_str());
		strings(n, "dependencies", m.dependencies);
	}
	auto &tasks = coding_detail::array(out, "tasks");
	for (const auto &t : p.tasks) {
		auto &n = tasks.add_child(false, true);
		n.add_text("id", t.id.c_str())
		    .add_text("title", t.title.c_str())
		    .add_text("module_id", t.module_id.c_str())
		    .add_text("status", t.status.c_str());
		strings(n, "dependencies", t.dependencies);
		strings(n, "acceptance_criteria", t.acceptance_criteria);
		strings(n, "test_plan", t.test_plan);
	}
}
std::vector<std::string> list(acl::json_node &a, const char *key)
{
	std::vector<std::string> out;
	auto *array = a[key];
	if (!array)
		return out;
	if (!array->is_array())
		throw std::invalid_argument(
		    std::string(key) + " must be an array");
	for (auto *n = array->first_child(); n; n = array->next_child()) {
		if (!n->is_string())
			throw std::invalid_argument(
			    "array values must be strings");
		out.push_back(json_values::text(n));
	}
	return out;
}
bool plan(agent_project_store_t &store, acl::json_node &a,
    agent_project_record_t &p, std::string &error)
{
	std::vector<agent_project_module_t> modules;
	std::vector<agent_project_task_t> tasks;
	auto *items = array_value(a["modules"]);
	if (!items || !items->is_array())
		throw std::invalid_argument("modules must be an array");
	for (auto *n = items->first_child(); n; n = items->next_child()) {
		if (!n->is_object())
			throw std::invalid_argument("module must be an object");
		agent_project_module_t m;
		m.id = text(*n, "id");
		m.name = text(*n, "name");
		m.layer = text(*n, "layer");
		m.path = text(*n, "path");
		m.dependencies = list(*n, "dependencies");
		modules.push_back(m);
	}
	items = array_value(a["tasks"]);
	if (!items || !items->is_array())
		throw std::invalid_argument("tasks must be an array");
	for (auto *n = items->first_child(); n; n = items->next_child()) {
		if (!n->is_object())
			throw std::invalid_argument("task must be an object");
		agent_project_task_t t;
		t.id = text(*n, "id");
		t.title = text(*n, "title");
		t.module_id = text(*n, "module_id");
		t.status = text(*n, "status");
		t.dependencies = list(*n, "dependencies");
		t.acceptance_criteria = list(*n, "acceptance_criteria");
		t.test_plan = list(*n, "test_plan");
		tasks.push_back(t);
	}
	return store.save_plan(text(a, "id"),
	    number(a, "expected_plan_version"), text(a, "goal"), modules, tasks,
	    p, error);
}
bool inspection(const std::string &root, const std::string &op,
    acl::json_node &a, acl::json_node &out, std::string &error)
{
	const auto path = text(a, "project_path");
	std::string absolute;
	if (!agent_workspace_t::resolve_project_root(
	        root, path, absolute, error, false))
		return false;
	if (op == "projects.diagnostics") {
		std::vector<project_diagnostic_t> entries;
		parse_project_diagnostics(
		    text(a, "source"), absolute, path, entries);
		auto &array = coding_detail::array(out, "diagnostics");
		for (const auto &p : entries)
			array.add_child(false, true)
			    .add_text("path", p.path.c_str())
			    .add_number("line", p.line)
			    .add_number("column", p.column)
			    .add_text("severity", p.severity.c_str())
			    .add_text("message", p.message.c_str());
		return true;
	}
	if (op == "projects.toolchain") {
		project_toolchain_t p;
		if (!project_toolchain_catalog_t(root).discover(path, p, error))
			return false;
		strings(out, "detected_languages", p.detected_languages);
		strings(out, "unavailable_tools", p.unavailable_tools);
		auto &array = coding_detail::array(out, "commands");
		for (const auto &c : p.commands) {
			auto &command = array.add_child(false, true);
			command.add_text("id", c.id.c_str())
			    .add_text("executable", c.executable.c_str())
			    .add_bool("dynamic_arguments", c.allow_dynamic_arguments)
			    .add_bool("requires_confirmation", true);
			strings(command, "fixed_arguments", c.fixed_arguments);
		}
		return true;
	}
	return false;
}
}
bool projects(const std::string &root, const std::string &op, acl::json_node &a,
    acl::json_node &out, std::string &error)
{
	if (op == "projects.scaffold") {
		agent_workspace_t w(root, false);
		project_scaffold_result_t p;
		if (!create_project_scaffold(w, text(a, "project_path"),
		        text(a, "language"), text(a, "platform"), p, error))
			return false;
		out.add_text("language", p.language.c_str())
		    .add_text("platform", p.platform.c_str())
		    .add_bool(
		        "reused_empty_directory", p.reused_empty_directory);
		strings(out, "files", p.files);
		return true;
	}
	if (op == "projects.diagnostics" || op == "projects.toolchain")
		return inspection(root, op, a, out, error);
	agent_project_store_t store(root);
	agent_project_record_t p;
	if (op == "projects.list") {
		std::vector<agent_project_record_t> entries;
		if (!store.list(limit(a), entries, error))
			return false;
		auto &array = coding_detail::array(out, "projects");
		for (const auto &entry : entries)
			record(array.add_child(false, true), entry);
		return true;
	}
	bool ok = false;
	if (op == "projects.get")
		ok = store.get(text(a, "id"), p, error);
	else if (op == "projects.create") {
		const auto path = text(a, "project_path");
		std::string normalized;
		if (!agent_workspace_t::normalize_path(
		        path, normalized, false, error))
			return false;
		ok = store.create(text(a, "title"), normalized,
		    text(a, "language"), text(a, "platform"), p, error);
	} else if (op == "projects.remove")
		ok = store.remove(text(a, "id"), p, error);
	else if (op == "projects.plan")
		ok = plan(store, a, p, error);
	else if (op == "projects.task_status")
		ok = store.update_task_status(text(a, "id"), text(a, "task_id"),
		    text(a, "status"), number(a, "expected_plan_version"), p,
		    error);
	if (ok)
		record(out, p);
	return ok;
}
}
}
}
