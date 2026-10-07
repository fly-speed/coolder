#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "agent_project_store.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <unistd.h>
#endif

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>

namespace webcool {
namespace ai {
namespace {

webcool::mutex g_project_store_mutex;
const char* kHeader = "WEBCOOL_AGENT_PROJECTS_V1";
const char* kFile = ".webcool_agent/projects.v1";
const size_t kMaxModules = 64;
const size_t kMaxTasks = 500;

using ::webcool::ai::file_ops::join_path;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

bool parse_number(const std::string& text, long long& value) {
    return ::webcool::ai::record_codec::parse_nonnegative_number(text, value);
}

bool valid_project_id(const std::string& id) {
    return ::webcool::ai::identifiers::valid_id(id);
}

bool valid_plan_id(const std::string& id) {
	if (id.empty() || id.size() > 64) return false;
	for (size_t i = 0; i < id.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(id[i]);
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
		{
			return false;
		}
	}
	return true;
}

bool safe_text(const std::string& value, size_t limit, bool required) {
	if ((required && value.empty()) || value.size() > limit) return false;
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		if (c == 0 || c == '\r' || c == '\n' || c == '\t' || c == 127) {
			return false;
		}
	}
	return true;
}

bool safe_block_text(const std::string& value, size_t limit, bool required) {
	if ((required && value.empty()) || value.size() > limit) return false;
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char c = static_cast<unsigned char>(value[i]);
		if (c == 0 || c == '\r' || c == '\t' || c == 127) return false;
	}
	return true;
}

bool safe_plan_path(const std::string& value) {
	if (!safe_text(value, 2048, true) || value[0] == '/' || value[0] == '\\'
		|| value.find('\\') != std::string::npos)
	{
		return false;
	}
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find('/', begin);
		const std::string component = value.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (component.empty() || component == "." || component == "..") return false;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return true;
}

bool valid_task_status(const std::string& status) {
	return status == "pending" || status == "in_progress"
		|| status == "blocked" || status == "completed" || status == "failed";
}

std::string join_list(const std::vector<std::string>& values) {
	std::string result;
	for (size_t i = 0; i < values.size(); ++i) {
		if (i != 0) result.push_back('\n');
		result += values[i];
	}
	return result;
}

void split_list(const std::string& value, std::vector<std::string>& values) {
	values.clear();
	if (value.empty()) return;
	size_t begin = 0;
	for (;;) {
		const size_t end = value.find('\n', begin);
		values.push_back(value.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin));
		if (end == std::string::npos) return;
		begin = end + 1;
	}
}

using ::webcool::ai::identifiers::new_id;

bool ensure_directory(const std::string& user_root, std::string& err) {
	const std::string directory = join_path(user_root, ".webcool_agent");
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), wide)) {
		err = "cannot validate agent project directory";
		return false;
	}
	DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		if (_wmkdir(wide.c_str()) != 0) {
			err = "cannot create agent project directory";
			return false;
		}
		attributes = GetFileAttributesW(wide.c_str());
	}
	if (attributes == INVALID_FILE_ATTRIBUTES
		|| (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
		|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		err = "agent project path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(directory.c_str(), &st) != 0) {
		if (mkdir(directory.c_str(), 0700) != 0
			|| lstat(directory.c_str(), &st) != 0)
		{
			err = "cannot create agent project directory";
			return false;
		}
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)
		|| chmod(directory.c_str(), 0700) != 0)
	{
		err = "agent project path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

using ::webcool::ai::file_ops::replace_file;

bool newer_project(const agent_project_record_t& left,
	const agent_project_record_t& right)
{
	if (left.updated_at != right.updated_at) return left.updated_at > right.updated_at;
	return left.id > right.id;
}

bool cycle_visit(const std::string& id,
	const std::map<std::string, std::vector<std::string> >& graph,
	std::map<std::string, int>& colors)
{
	if (colors[id] == 1) return false;
	if (colors[id] == 2) return true;
	colors[id] = 1;
	std::map<std::string, std::vector<std::string> >::const_iterator found =
		graph.find(id);
	if (found != graph.end()) {
		for (size_t i = 0; i < found->second.size(); ++i) {
			if (!cycle_visit(found->second[i], graph, colors)) return false;
		}
	}
	colors[id] = 2;
	return true;
}

bool acyclic(const std::map<std::string, std::vector<std::string> >& graph) {
	std::map<std::string, int> colors;
	for (std::map<std::string, std::vector<std::string> >::const_iterator it =
		graph.begin(); it != graph.end(); ++it)
	{
		if (!cycle_visit(it->first, graph, colors)) return false;
	}
	return true;
}

bool safe_record(const agent_project_record_t& record, std::string& err) {
	if (!valid_project_id(record.id)
		|| !safe_text(record.title, 120, true)
		|| !safe_text(record.project_path, 2048, true)
		|| !safe_text(record.language, 64, true)
		|| !safe_text(record.platform, 64, true)
		|| !safe_block_text(record.goal, 8192, false)
		|| !safe_text(record.status, 32, true)
		|| record.created_at <= 0 || record.updated_at < record.created_at
		|| record.plan_version < 0)
	{
		err = "invalid agent project record";
		return false;
	}
	return agent_project_store_t::validate_plan(record.modules, record.tasks, err);
}

bool save_records(const std::string& user_root,
	const std::vector<agent_project_record_t>& records, std::string& err)
{
	if (!ensure_directory(user_root, err)) return false;
	const std::string path = join_path(user_root, kFile);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::binary
		| std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent project database";
		return false;
	}
	out << kHeader << '\n';
	for (size_t i = 0; i < records.size(); ++i) {
		if (!safe_record(records[i], err)) {
			out.close();
			remove(temporary.c_str());
			return false;
		}
		const agent_project_record_t& project = records[i];
		out << "P\t" << hex_encode(project.id) << '\t'
			<< hex_encode(project.title) << '\t' << hex_encode(project.project_path)
			<< '\t' << hex_encode(project.language) << '\t'
			<< hex_encode(project.platform) << '\t' << hex_encode(project.goal)
			<< '\t' << hex_encode(project.status) << '\t' << project.created_at
			<< '\t' << project.updated_at << '\t' << project.plan_version << '\n';
		for (size_t j = 0; j < project.modules.size(); ++j) {
			const agent_project_module_t& module = project.modules[j];
			out << "M\t" << hex_encode(project.id) << '\t' << hex_encode(module.id)
				<< '\t' << hex_encode(module.name) << '\t' << hex_encode(module.layer)
				<< '\t' << hex_encode(module.path) << '\t'
				<< hex_encode(join_list(module.dependencies)) << '\n';
		}
		for (size_t j = 0; j < project.tasks.size(); ++j) {
			const agent_project_task_t& task = project.tasks[j];
			out << "T\t" << hex_encode(project.id) << '\t' << hex_encode(task.id)
				<< '\t' << hex_encode(task.title) << '\t'
				<< hex_encode(task.module_id) << '\t' << hex_encode(task.status)
				<< '\t' << hex_encode(join_list(task.dependencies)) << '\t'
				<< hex_encode(join_list(task.acceptance_criteria)) << '\t'
				<< hex_encode(join_list(task.test_plan)) << '\n';
		}
	}
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush agent project database";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect agent project database";
		return false;
	}
#endif
	if (!replace_file(temporary, path)) {
		remove(temporary.c_str());
		err = std::string("cannot install agent project database: ")
			+ strerror(errno);
		return false;
	}
	return true;
}

agent_project_record_t* find_project(std::vector<agent_project_record_t>& records,
	const std::string& id)
{
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].id == id) return &records[i];
	}
	return NULL;
}

bool load_records(const std::string& user_root,
	std::vector<agent_project_record_t>& records, std::string& err)
{
	records.clear();
	std::ifstream in(join_path(user_root, kFile).c_str(),
		std::ios::in | std::ios::binary);
	if (!in.good()) return true;
	std::string line;
	if (!std::getline(in, line) || line != kHeader) {
		err = "invalid agent project database";
		return false;
	}
	while (std::getline(in, line)) {
		if (line.empty()) continue;
		std::vector<std::string> fields;
		split_tabs(line, fields);
		if (fields.empty()) continue;
		if (fields[0] == "P" && fields.size() == 11) {
			agent_project_record_t project;
			if (!hex_decode(fields[1], project.id)
				|| !hex_decode(fields[2], project.title)
				|| !hex_decode(fields[3], project.project_path)
				|| !hex_decode(fields[4], project.language)
				|| !hex_decode(fields[5], project.platform)
				|| !hex_decode(fields[6], project.goal)
				|| !hex_decode(fields[7], project.status)
				|| !parse_number(fields[8], project.created_at)
				|| !parse_number(fields[9], project.updated_at)
				|| !parse_number(fields[10], project.plan_version))
			{
				err = "invalid agent project record";
				return false;
			}
			records.push_back(project);
		} else if (fields[0] == "M" && fields.size() == 7) {
			std::string project_id;
			agent_project_module_t module;
			std::string dependencies;
			if (!hex_decode(fields[1], project_id)
				|| !hex_decode(fields[2], module.id)
				|| !hex_decode(fields[3], module.name)
				|| !hex_decode(fields[4], module.layer)
				|| !hex_decode(fields[5], module.path)
				|| !hex_decode(fields[6], dependencies))
			{
				err = "invalid agent project module";
				return false;
			}
			agent_project_record_t* project = find_project(records, project_id);
			if (project == NULL || project->modules.size() >= kMaxModules) {
				err = "invalid agent project module owner";
				return false;
			}
			split_list(dependencies, module.dependencies);
			project->modules.push_back(module);
		} else if (fields[0] == "T" && fields.size() == 9) {
			std::string project_id;
			agent_project_task_t task;
			std::string dependencies;
			std::string acceptance;
			std::string tests;
			if (!hex_decode(fields[1], project_id)
				|| !hex_decode(fields[2], task.id)
				|| !hex_decode(fields[3], task.title)
				|| !hex_decode(fields[4], task.module_id)
				|| !hex_decode(fields[5], task.status)
				|| !hex_decode(fields[6], dependencies)
				|| !hex_decode(fields[7], acceptance)
				|| !hex_decode(fields[8], tests))
			{
				err = "invalid agent project task";
				return false;
			}
			agent_project_record_t* project = find_project(records, project_id);
			if (project == NULL || project->tasks.size() >= kMaxTasks) {
				err = "invalid agent project task owner";
				return false;
			}
			split_list(dependencies, task.dependencies);
			split_list(acceptance, task.acceptance_criteria);
			split_list(tests, task.test_plan);
			project->tasks.push_back(task);
		} else {
			err = "invalid agent project database record";
			return false;
		}
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (!safe_record(records[i], err)) return false;
	}
	return true;
}

void update_project_status(agent_project_record_t& project) {
	if (project.tasks.empty()) {
		project.status = project.modules.empty() ? "draft" : "planned";
		return;
	}
	bool all_completed = true;
	bool any_started = false;
	for (size_t i = 0; i < project.tasks.size(); ++i) {
		all_completed = all_completed && project.tasks[i].status == "completed";
		any_started = any_started || project.tasks[i].status != "pending";
	}
	project.status = all_completed ? "completed" : (any_started ? "active" : "planned");
}

} // namespace

agent_project_store_t::agent_project_store_t(const std::string& user_root)
	: user_root_(user_root) {}

bool agent_project_store_t::validate_plan(
	const std::vector<agent_project_module_t>& modules,
	const std::vector<agent_project_task_t>& tasks, std::string& err)
{
	if (modules.size() > kMaxModules || tasks.size() > kMaxTasks) {
		err = "agent project plan exceeds its item limit";
		return false;
	}
	std::set<std::string> module_ids;
	std::map<std::string, std::vector<std::string> > module_graph;
	for (size_t i = 0; i < modules.size(); ++i) {
		const agent_project_module_t& module = modules[i];
		if (!valid_plan_id(module.id) || !module_ids.insert(module.id).second
			|| !safe_text(module.name, 120, true)
			|| !safe_text(module.layer, 64, true)
			|| !safe_plan_path(module.path)
			|| module.dependencies.size() > 32)
		{
			err = "invalid agent project module";
			return false;
		}
		module_graph[module.id] = module.dependencies;
	}
	for (size_t i = 0; i < modules.size(); ++i) {
		for (size_t j = 0; j < modules[i].dependencies.size(); ++j) {
			if (module_ids.count(modules[i].dependencies[j]) == 0
				|| modules[i].dependencies[j] == modules[i].id)
			{
				err = "agent project module dependency is invalid";
				return false;
			}
		}
	}
	if (!acyclic(module_graph)) {
		err = "agent project module dependencies contain a cycle";
		return false;
	}

	std::set<std::string> task_ids;
	std::map<std::string, std::vector<std::string> > task_graph;
	for (size_t i = 0; i < tasks.size(); ++i) {
		const agent_project_task_t& task = tasks[i];
		if (!valid_plan_id(task.id) || !task_ids.insert(task.id).second
			|| !safe_text(task.title, 240, true)
			|| (!task.module_id.empty() && module_ids.count(task.module_id) == 0)
			|| !valid_task_status(task.status)
			|| task.dependencies.size() > 64
			|| task.acceptance_criteria.size() > 16 || task.test_plan.size() > 16)
		{
			err = "invalid agent project task";
			return false;
		}
		for (size_t j = 0; j < task.acceptance_criteria.size(); ++j) {
			if (!safe_text(task.acceptance_criteria[j], 500, true)) {
				err = "invalid agent project acceptance criterion";
				return false;
			}
		}
		for (size_t j = 0; j < task.test_plan.size(); ++j) {
			if (!safe_text(task.test_plan[j], 500, true)) {
				err = "invalid agent project test plan";
				return false;
			}
		}
		task_graph[task.id] = task.dependencies;
	}
	for (size_t i = 0; i < tasks.size(); ++i) {
		for (size_t j = 0; j < tasks[i].dependencies.size(); ++j) {
			if (task_ids.count(tasks[i].dependencies[j]) == 0
				|| tasks[i].dependencies[j] == tasks[i].id)
			{
				err = "agent project task dependency is invalid";
				return false;
			}
		}
	}
	if (!acyclic(task_graph)) {
		err = "agent project task dependencies contain a cycle";
		return false;
	}
	return true;
}

bool agent_project_store_t::create(const std::string& title,
	const std::string& project_path, const std::string& language,
	const std::string& platform, agent_project_record_t& record,
	std::string& err) const
{
	if (!safe_text(title, 120, true) || !safe_text(project_path, 2048, true)
		|| !safe_text(language, 64, true) || !safe_text(platform, 64, true))
	{
		err = "invalid agent project settings";
		return ai_error("agent.project-store", "validate-create", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-create", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].project_path == project_path) {
			record = records[i];
			return true;
		}
	}

	record = agent_project_record_t();
	record.id = new_id();
	if (record.id.empty()) {
		err = "cannot generate agent project id";
		return ai_error("agent.project-store", "generate-id", err);
	}
	record.title = title;
	record.project_path = project_path;
	record.language = language;
	record.platform = platform;
	record.status = "draft";
	record.created_at = static_cast<long long>(time(NULL));
	record.updated_at = record.created_at;
	record.plan_version = 0;
	records.push_back(record);
	std::sort(records.begin(), records.end(), newer_project);
	if (!save_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "save-create", err);
	}
	return true;
}

bool agent_project_store_t::get(const std::string& id,
	agent_project_record_t& record, std::string& err) const
{
	if (!valid_project_id(id)) {
		err = "invalid agent project id";
		return ai_error("agent.project-store", "validate-get", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-get", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].id == id) {
			record = records[i];
			return true;
		}
	}
	err = "agent project not found";
	return ai_error("agent.project-store", "find-for-get", err);
}

bool agent_project_store_t::find_by_path(const std::string& project_path,
	agent_project_record_t& record, std::string& err) const
{
	if (!safe_text(project_path, 2048, true)) {
		err = "invalid agent project path";
		return ai_error("agent.project-store", "validate-find-path", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-find-path", err);
	}
	for (size_t i = 0; i < records.size(); ++i) {
		if (records[i].project_path == project_path) {
			record = records[i];
			return true;
		}
	}
	err = "agent project not found";
	return ai_error("agent.project-store", "find-path", err);
}

bool agent_project_store_t::list(size_t limit,
	std::vector<agent_project_record_t>& records, std::string& err) const
{

	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-list", err);
	}
	std::sort(records.begin(), records.end(), newer_project);
	if (limit != 0 && records.size() > limit) records.resize(limit);
	return true;
}

bool agent_project_store_t::remove(const std::string& id,
	agent_project_record_t& removed, std::string& err) const
{
	if (!valid_project_id(id)) {
		err = "invalid agent project id";
		return ai_error("agent.project-store", "validate-remove", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-remove", err);
	}
	for (std::vector<agent_project_record_t>::iterator it = records.begin();
		it != records.end(); ++it)
	{
		if (it->id != id) continue;
		removed = *it;
		records.erase(it);
		if (!save_records(user_root_, records, err)) {
			return ai_error("agent.project-store", "save-remove", err);
		}
		return true;
	}
	err = "agent project not found";
	return ai_error("agent.project-store", "find-for-remove", err);
}

bool agent_project_store_t::save_plan(const std::string& id,
	long long expected_plan_version, const std::string& goal,
	const std::vector<agent_project_module_t>& modules,
	const std::vector<agent_project_task_t>& tasks,
	agent_project_record_t& record, std::string& err) const
{
	if (!valid_project_id(id) || expected_plan_version < 0
		|| !safe_block_text(goal, 8192, true) || !validate_plan(modules, tasks, err))
	{
		if (err.empty()) err = "invalid agent project plan";
		return ai_error("agent.project-store", "validate-save-plan", err);
	}
	for (size_t i = 0; i < tasks.size(); ++i) {
		if (tasks[i].status != "pending") {
			err = "new agent project plan tasks must be pending";
			return ai_error("agent.project-store", "validate-new-task-state", err);
		}
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-save-plan", err);
	}
	agent_project_record_t* project = find_project(records, id);
	if (project == NULL) {
		err = "agent project not found";
		return ai_error("agent.project-store", "find-for-save-plan", err);
	}
	if (project->plan_version != expected_plan_version) {
		err = "agent project plan version conflict";
		return ai_error("agent.project-store", "compare-plan-version", err);
	}
	for (size_t i = 0; i < modules.size(); ++i) {
		const std::string& path = modules[i].path;
		if (!(path == project->project_path
			|| (path.size() > project->project_path.size()
				&& path.compare(0, project->project_path.size(),
					project->project_path) == 0
				&& path[project->project_path.size()] == '/')))
		{
			err = "agent project module path is outside the project";
			return ai_error("agent.project-store", "validate-module-path", err);
		}
	}
	// A plan edit is a new structural revision, not a reset of execution
	// history. Preserve the state of tasks whose stable IDs still exist. New
	// task IDs always begin pending because the HTTP action never accepts a
	// caller-supplied state.
	std::vector<agent_project_task_t> revised_tasks = tasks;
	for (size_t i = 0; i < revised_tasks.size(); ++i) {
		for (size_t j = 0; j < project->tasks.size(); ++j) {
			if (revised_tasks[i].id == project->tasks[j].id) {
				revised_tasks[i].status = project->tasks[j].status;
				break;
			}
		}
	}
	agent_project_record_t revised = *project;
	revised.modules = modules;
	revised.tasks = revised_tasks;
	// Do not permit a revision to add an unfinished prerequisite underneath an
	// already active task; that would make the next action nondeterministic.
	for (size_t i = 0; i < revised.tasks.size(); ++i) {
		if (revised.tasks[i].status != "in_progress") continue;
		for (size_t j = 0; j < revised.tasks[i].dependencies.size(); ++j) {
			bool complete = false;
			for (size_t k = 0; k < revised.tasks.size(); ++k) {
				if (revised.tasks[k].id == revised.tasks[i].dependencies[j]) {
					complete = revised.tasks[k].status == "completed";
					break;
				}
			}
			if (!complete) {
				err = "revised plan leaves an active task with incomplete dependencies";
				return ai_error("agent.project-store",
					"validate-revised-active-task", err);
			}
		}
	}
	project->goal = goal;
	project->modules = modules;
	project->tasks = revised_tasks;
	++project->plan_version;
	project->updated_at = static_cast<long long>(time(NULL));
	update_project_status(*project);
	record = *project;
	std::sort(records.begin(), records.end(), newer_project);
	if (!save_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "save-plan", err);
	}
	return true;
}

bool agent_project_store_t::task_ready(const agent_project_record_t& project,
	const agent_project_task_t& task)
{
	if (task.status != "pending") return false;
	for (size_t i = 0; i < task.dependencies.size(); ++i) {
		bool complete = false;
		for (size_t j = 0; j < project.tasks.size(); ++j) {
			if (project.tasks[j].id == task.dependencies[i]) {
				complete = project.tasks[j].status == "completed";
				break;
			}
		}
		if (!complete) return false;
	}
	return true;
}

bool agent_project_store_t::update_task_status(const std::string& project_id,
	const std::string& task_id, const std::string& status,
	long long expected_plan_version, agent_project_record_t& record,
	std::string& err) const
{
	if (!valid_project_id(project_id) || !valid_plan_id(task_id)
		|| !valid_task_status(status) || expected_plan_version < 0)
	{
		err = "invalid agent project task update";
		return ai_error("agent.project-store", "validate-task-update", err);
	}
	std::lock_guard<webcool::mutex> guard(g_project_store_mutex);
	std::vector<agent_project_record_t> records;
	if (!load_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "load-for-task-update", err);
	}
	agent_project_record_t* project = find_project(records, project_id);
	if (project == NULL) {
		err = "agent project not found";
		return ai_error("agent.project-store", "find-for-task-update", err);
	}
	if (project->plan_version != expected_plan_version) {
		err = "agent project plan version conflict";
		return ai_error("agent.project-store", "compare-task-plan-version", err);
	}
	agent_project_task_t* task = NULL;
	for (size_t i = 0; i < project->tasks.size(); ++i) {
		if (project->tasks[i].id == task_id) task = &project->tasks[i];
	}
	if (task == NULL) {
		err = "agent project task not found";
		return ai_error("agent.project-store", "find-task", err);
	}
	const std::string before = task->status;
	const bool allowed = (before == "pending"
			&& (status == "in_progress" || status == "blocked"))
		|| ((before == "blocked" || before == "failed") && status == "pending")
		|| (before == "in_progress"
			&& (status == "completed" || status == "failed" || status == "blocked"));
	if (!allowed) {
		err = "invalid agent project task state transition";
		return ai_error("agent.project-store", "validate-task-transition", err);
	}
	if (status == "in_progress") {
		for (size_t i = 0; i < project->tasks.size(); ++i) {
			if (project->tasks[i].id != task_id
				&& project->tasks[i].status == "in_progress")
			{
				err = "another agent project task is already in progress";
				return ai_error("agent.project-store",
					"enforce-single-active-task", err);
			}
		}
	}
	if (status == "in_progress" && !task_ready(*project, *task)) {
		err = "agent project task dependencies are not completed";
		return ai_error("agent.project-store", "check-task-dependencies", err);
	}
	task->status = status;
	++project->plan_version;
	project->updated_at = static_cast<long long>(time(NULL));
	update_project_status(*project);
	record = *project;
	std::sort(records.begin(), records.end(), newer_project);
	if (!save_records(user_root_, records, err)) {
		return ai_error("agent.project-store", "save-task-update", err);
	}
	return true;
}

} // namespace ai
} // namespace webcool
