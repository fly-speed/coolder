#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/record_codec.h"
#include "agent_workflow_store.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace webcool
{
namespace ai
{
namespace
{

webcool::mutex g_workflow_mutex;
const char *kHeader = "WEBCOOL_AGENT_WORKFLOW_V1";

using ::webcool::ai::file_ops::join_path;

bool valid_hex_id(const std::string &value)
{
	if (value.size() != 32)
		return false;
	for (size_t i = 0; i < value.size(); ++i) {
		const char ch = value[i];
		if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
			continue;
		return false;
	}
	return true;
}

bool valid_token(const std::string &value, size_t maximum)
{
	if (value.empty() || value.size() > maximum)
		return false;
	for (size_t i = 0; i < value.size(); ++i) {
		const char ch = value[i];
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
		    ch == '.')
			continue;
		return false;
	}
	return true;
}

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_value;

bool hex_decode(const std::string &value, std::string &output)
{
	if (!(value.size() % 2 != 0))
		return ::webcool::ai::record_codec::hex_decode(value, output);
	return false;
}

using ::webcool::ai::record_codec::split_tabs;

bool parse_number(const std::string &value, long long &parsed)
{
	char *end = NULL;
	errno = 0;
	parsed = strtoll(value.c_str(), &end, 10);
	return errno == 0 && end != value.c_str() && *end == '\0';
}

bool valid_checkpoint(const agent_workflow_checkpoint_t &value)
{
	if (!valid_hex_id(value.project_id) ||
	    !valid_hex_id(value.session_id) ||
	    !valid_token(value.task_id, 64) ||
	    value.command_queue.size() > 32 || value.diagnostics.size() > 100 ||
	    value.updated_at < 0)
		return false;
	for (size_t i = 0; i < value.command_queue.size(); ++i) {
		if (valid_token(value.command_queue[i], 128))
			continue;
		return false;
	}
	if (!value.failed_command_id.empty() &&
	    !valid_token(value.failed_command_id, 128))
		return false;
	for (size_t i = 0; i < value.diagnostics.size(); ++i) {
		const agent_workflow_diagnostic_t &item = value.diagnostics[i];
		if (!(item.path.size() > 2048 ||
		        item.path.find('\0') != std::string::npos ||
		        item.path.find('\n') != std::string::npos ||
		        item.path.find('\r') != std::string::npos ||
		        item.line < 0 || item.column < 0 ||
		        (item.severity != "error" &&
		            item.severity != "warning" &&
		            item.severity != "note")))
			continue;
		return false;
	}
	return true;
}

std::string directory_path(const std::string &root)
{
	return join_path(root, ".webcool_agent/workflows");
}

std::string checkpoint_path(const std::string &root, const std::string &project,
    const std::string &session, const std::string &task)
{
	return join_path(
	    directory_path(root), project + "." + session + "." + task + ".v1");
}

bool ensure_one_directory(const std::string &path, std::string &err)
{
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot validate workflow checkpoint directory";
		return false;
	}
	DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		if (_wmkdir(wide.c_str()) != 0) {
			err = "cannot create workflow checkpoint directory";
			return false;
		}
		attributes = GetFileAttributesW(wide.c_str());
	}
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "workflow checkpoint path is not a safe directory";
		return false;
	}
#else
	struct stat st;

	if ((lstat(path.c_str(), &st) != 0) &&
	    (mkdir(path.c_str(), 0700) != 0 || lstat(path.c_str(), &st) != 0)) {
		err = "cannot create workflow checkpoint directory";
		return false;
	}
	if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
	    chmod(path.c_str(), 0700) != 0) {
		err =
		    "workflow checkpoint path is not a safe private directory";
		return false;
	}
#endif
	return true;
}

bool ensure_directories(const std::string &root, std::string &err)
{
	return ensure_one_directory(join_path(root, ".webcool_agent"), err) &&
	    ensure_one_directory(directory_path(root), err);
}

using ::webcool::ai::file_ops::replace_file;

bool remove_matching_files(const std::string &directory,
    const std::string &prefix, const std::string &contains, size_t &removed,
    std::string &err)
{
	removed = 0;
#ifdef _WIN32
	std::wstring directory_wide;
	if (!webcool_utf8_path_to_wide(directory.c_str(), directory_wide)) {
		err = "cannot encode workflow checkpoint directory";
		return false;
	}
	WIN32_FIND_DATAW data;
	const std::wstring pattern = directory_wide + L"\\*.v1";
	HANDLE search = FindFirstFileW(pattern.c_str(), &data);
	if (search == INVALID_HANDLE_VALUE) {
		return GetLastError() == ERROR_FILE_NOT_FOUND;
	}
	do {
		std::string name;
		if (!webcool_wide_to_utf8(data.cFileName, name) ||
		    (data.dwFileAttributes &
		        (FILE_ATTRIBUTE_DIRECTORY |
		            FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
			continue;
		if ((!prefix.empty() &&
		        name.compare(0, prefix.size(), prefix) != 0) ||
		    (!contains.empty() &&
		        name.find(contains) == std::string::npos))
			continue;
		const std::wstring target =
		    directory_wide + L"\\" + data.cFileName;
		if (!DeleteFileW(target.c_str())) {
			err = "cannot remove workflow checkpoint metadata";
			FindClose(search);
			return false;
		}
		++removed;
	} while (FindNextFileW(search, &data));
	const DWORD scan_error = GetLastError();
	FindClose(search);
	if (scan_error != ERROR_NO_MORE_FILES) {
		err = "cannot scan workflow checkpoint directory";
		return false;
	}
#else
	DIR *directory_handle = opendir(directory.c_str());
	if (directory_handle == NULL) {
		if (errno == ENOENT)
			return true;
		err =
		    std::string("cannot scan workflow checkpoint directory: ") +
		    strerror(errno);
		return false;
	}
	for (dirent *entry = readdir(directory_handle); entry != NULL;
	     entry = readdir(directory_handle)) {
		const std::string name = entry->d_name;
		if (name == "." || name == ".." ||
		    (!prefix.empty() &&
		        name.compare(0, prefix.size(), prefix) != 0) ||
		    (!contains.empty() &&
		        name.find(contains) == std::string::npos))
			continue;
		const std::string target = join_path(directory, name);
		struct stat st;
		if (lstat(target.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
		    S_ISLNK(st.st_mode))
			continue;
		if (::remove(target.c_str()) != 0) {
			err =
			    std::string(
			        "cannot remove workflow checkpoint metadata: ") +
			    strerror(errno);
			closedir(directory_handle);
			return false;
		}
		++removed;
	}
	closedir(directory_handle);
#endif
	return true;
}

} // namespace

agent_workflow_store_t::agent_workflow_store_t(const std::string &user_root)
        : user_root_(user_root)
{
}

bool agent_workflow_store_t::save(
    const agent_workflow_checkpoint_t &value, std::string &err) const
{
	if (!valid_checkpoint(value)) {
		err = "invalid agent workflow checkpoint";
		return ai_error("agent.workflow", "validate-save", err);
	}
	std::lock_guard<webcool::mutex> guard(g_workflow_mutex);
	if (!ensure_directories(user_root_, err)) {
		return ai_error("agent.workflow", "prepare-directory", err);
	}
	const std::string target = checkpoint_path(
	    user_root_, value.project_id, value.session_id, value.task_id);
	const std::string temporary = target + ".tmp";
	std::ofstream out(temporary.c_str(),
	    std::ios::out | std::ios::binary | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write agent workflow checkpoint";
		return ai_error("agent.workflow", "open-temporary", err);
	}
	out << kHeader << '\n'
	    << "H\t" << value.project_id << '\t' << value.session_id << '\t'
	    << value.task_id << '\t' << value.failed_exit_code << '\t'
	    << value.updated_at << '\t' << hex_encode(value.failed_command_id)
	    << '\n';
	for (size_t i = 0; i < value.command_queue.size(); ++i) {
		out << "Q\t" << hex_encode(value.command_queue[i]) << '\n';
	}
	for (size_t i = 0; i < value.diagnostics.size(); ++i) {
		out << "D\t" << hex_encode(value.diagnostics[i].path) << '\t'
		    << value.diagnostics[i].line << '\t'
		    << value.diagnostics[i].column << '\t'
		    << value.diagnostics[i].severity << '\n';
	}
	out.close();
	if (!out.good()) {
		::remove(temporary.c_str());
		err = "cannot flush agent workflow checkpoint";
		return ai_error("agent.workflow", "flush", err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect agent workflow checkpoint";
		return ai_error("agent.workflow", "protect", err);
	}
#endif
	if (replace_file(temporary, target))
		return true;
	::remove(temporary.c_str());
	err = std::string("cannot install agent workflow checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.workflow", "install", err);
}

bool agent_workflow_store_t::load(const std::string &project,
    const std::string &session, const std::string &task,
    agent_workflow_checkpoint_t &value, bool &found, std::string &err) const
{
	found = false;
	value = agent_workflow_checkpoint_t();
	if (!valid_hex_id(project) || !valid_hex_id(session) ||
	    !valid_token(task, 64)) {
		err = "invalid agent workflow checkpoint identity";
		return ai_error("agent.workflow", "validate-load", err);
	}
	std::lock_guard<webcool::mutex> guard(g_workflow_mutex);
	std::ifstream in(
	    checkpoint_path(user_root_, project, session, task).c_str(),
	    std::ios::in | std::ios::binary);
	if (!in.good())
		return true;
	std::string line;
	if (!std::getline(in, line) || line != kHeader ||
	    !std::getline(in, line)) {
		err = "invalid agent workflow checkpoint file";
		return ai_error("agent.workflow", "read-header", err);
	}
	std::vector<std::string> fields;
	split_tabs(line, fields);
	if (fields.size() != 7 || fields[0] != "H") {
		err = "invalid agent workflow checkpoint header";
		return ai_error("agent.workflow", "parse-header", err);
	}
	value.project_id = fields[1];
	value.session_id = fields[2];
	value.task_id = fields[3];
	if (!parse_number(fields[4], value.failed_exit_code) ||
	    !parse_number(fields[5], value.updated_at) ||
	    !hex_decode(fields[6], value.failed_command_id)) {
		err = "invalid agent workflow checkpoint header values";
		return ai_error("agent.workflow", "parse-header-values", err);
	}
	while (std::getline(in, line)) {
		if (line.empty())
			continue;
		split_tabs(line, fields);
		if (fields.size() == 2 && fields[0] == "Q") {
			std::string command;
			if (!hex_decode(fields[1], command)) {
				err = "invalid workflow command encoding";
				return ai_error(
				    "agent.workflow", "parse-command", err);
			}
			value.command_queue.push_back(command);
		} else if (fields.size() == 5 && fields[0] == "D") {
			agent_workflow_diagnostic_t item;
			if (!hex_decode(fields[1], item.path) ||
			    !parse_number(fields[2], item.line) ||
			    !parse_number(fields[3], item.column)) {
				err = "invalid workflow diagnostic encoding";
				return ai_error(
				    "agent.workflow", "parse-diagnostic", err);
			}
			item.severity = fields[4];
			value.diagnostics.push_back(item);
		} else {
			err = "invalid workflow checkpoint record";
			return ai_error("agent.workflow", "parse-record", err);
		}
	}
	if (value.project_id != project || value.session_id != session ||
	    value.task_id != task || !valid_checkpoint(value)) {
		err = "agent workflow checkpoint ownership mismatch";
		return ai_error("agent.workflow", "validate-loaded", err);
	}
	found = true;
	return true;
}

bool agent_workflow_store_t::remove(const std::string &project,
    const std::string &session, const std::string &task, std::string &err) const
{
	if (!valid_hex_id(project) || !valid_hex_id(session) ||
	    !valid_token(task, 64)) {
		err = "invalid agent workflow checkpoint identity";
		return ai_error("agent.workflow", "validate-remove", err);
	}
	std::lock_guard<webcool::mutex> guard(g_workflow_mutex);
	if (!(::remove(checkpoint_path(user_root_, project, session, task)
	                   .c_str()) != 0 &&
	        errno != ENOENT))
		return true;
	err = std::string("cannot remove agent workflow checkpoint: ") +
	    strerror(errno);
	return ai_error("agent.workflow", "remove", err);
}

bool agent_workflow_store_t::remove_for_project(
    const std::string &project, size_t &removed, std::string &err) const
{
	if (!valid_hex_id(project)) {
		err = "invalid agent workflow project identity";
		return ai_error(
		    "agent.workflow", "validate-remove-project", err);
	}
	std::lock_guard<webcool::mutex> guard(g_workflow_mutex);
	if (remove_matching_files(
	        directory_path(user_root_), project + ".", "", removed, err))
		return true;
	return ai_error("agent.workflow", "remove-project", err);
}

bool agent_workflow_store_t::remove_for_session(
    const std::string &session, size_t &removed, std::string &err) const
{
	if (!valid_hex_id(session)) {
		err = "invalid agent workflow session identity";
		return ai_error(
		    "agent.workflow", "validate-remove-session", err);
	}
	std::lock_guard<webcool::mutex> guard(g_workflow_mutex);
	if (remove_matching_files(directory_path(user_root_), "",
	        "." + session + ".", removed, err))
		return true;
	return ai_error("agent.workflow", "remove-session", err);
}

} // namespace ai
} // namespace webcool
