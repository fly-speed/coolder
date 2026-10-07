#include "stdafx.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "agent_request_store.h"
#include "../workspace/agent_workspace.h"
#include "../common/ai_error_log.h"
#include "../common/webcool_mutex.h"

#ifdef _WIN32
#include "../common/platform_compat.h"
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

namespace webcool {
namespace ai {
namespace {

webcool::mutex g_request_store_mutex;
const size_t kMaxRequestBytes = 32 * 1024 * 1024;
const unsigned long kMaxRequestsPerRun = 1000000;

using ::webcool::ai::file_ops::join_path;

bool valid_run_id(const std::string& id) {
    return ::webcool::ai::identifiers::valid_id(id);
}

bool normal_directory(const std::string& path) {
    return ::webcool::ai::file_ops::safe_directory(path);
}

bool make_directory(const std::string& path) {
    return ::webcool::ai::file_ops::make_private_directory(path);
}

bool path_exists(const std::string& path) {
    return ::webcool::ai::file_ops::path_entry_exists(path);
}

using ::webcool::ai::file_ops::replace_file;

bool resolve_project(const std::string& user_root,
	const std::string& project_path, std::string& project, std::string& err)
{
	return agent_workspace_t::resolve_project_state_root(user_root, project_path,
		project, err) && normal_directory(project);
}

bool prepare_directory(const std::string& project, std::string& directory,
	std::string& err)
{
	const std::string agent = join_path(project, ".webcool_agent");
	directory = join_path(agent, "requests");
	if (!make_directory(agent) || !make_directory(directory)) {
		err = "cannot create AI request archive directory";
		return false;
	}
#ifndef _WIN32
	if (chmod(agent.c_str(), 0700) != 0 || chmod(directory.c_str(), 0700) != 0) {
		err = "cannot protect AI request archive directory";
		return false;
	}
#endif
	return true;
}

std::string request_name(const std::string& run_id, unsigned long sequence) {
	std::ostringstream out;
	out << "request-" << run_id << "-" << std::setw(6)
		<< std::setfill('0') << sequence << ".json";
	return out.str();
}

bool valid_json_payload(const std::string& payload) {
	if (payload.empty() || payload.size() > kMaxRequestBytes) return false;
	acl::json json(payload.c_str());
	return json.finish();
}

bool save_file(const std::string& target, const std::string& payload,
	std::string& err)
{
	const std::string temporary = target + ".tmp";
	// fopen is mapped to the UTF-8-aware webcool_fopen on Windows.  A narrow
	// std::ofstream path is interpreted using the active ANSI code page and
	// cannot reliably create archives below non-ASCII project directories.
	FILE* out = fopen(temporary.c_str(), "wb");
	if (out == NULL) {
		err = "cannot write AI request archive";
		return false;
	}
	const bool written = fwrite(payload.data(), 1, payload.size(), out)
		== payload.size();
	const bool flushed = written && fflush(out) == 0;
	const bool closed = fclose(out) == 0;
	if (!flushed || !closed) {
		unlink(temporary.c_str());
		err = "cannot flush AI request archive";
		return false;
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		unlink(temporary.c_str());
		err = "cannot protect AI request archive";
		return false;
	}
#endif
	if (!replace_file(temporary, target)) {
		unlink(temporary.c_str());
		err = std::string("cannot install AI request archive: ") + strerror(errno);
		return false;
	}
	return true;
}

} // namespace

agent_request_store_t::agent_request_store_t(const std::string& user_root,
	const std::string& project_path, const std::string& run_id)
	: user_root_(user_root), project_path_(project_path), run_id_(run_id) {}

bool agent_request_store_t::append(const std::string& payload,
	std::string& relative_path, std::string& err) const
{
	relative_path.clear();
	if (!valid_run_id(run_id_) || !valid_json_payload(payload)) {
		err = "invalid AI provider JSON request archive";
		return ai_error("agent.request", "validate", err);
	}
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) {
		return ai_error("agent.request", "resolve-project", err);
	}
	std::lock_guard<webcool::mutex> guard(g_request_store_mutex);
	std::string directory;
	if (!prepare_directory(project, directory, err)) {
		return ai_error("agent.request", "prepare-directory", err);
	}
	for (unsigned long sequence = 1; sequence <= kMaxRequestsPerRun; ++sequence) {
		const std::string name = request_name(run_id_, sequence);
		const std::string target = join_path(directory, name);
		if (path_exists(target)) continue;
		if (!save_file(target, payload, err)) {
			return ai_error("agent.request", "persist", err);
		}
		relative_path = join_path(join_path(join_path(project_path_,
			".webcool_agent"), "requests"), name);
		return true;
	}
	err = "AI request archive sequence limit reached";
	return ai_error("agent.request", "sequence-limit", err);
}

bool agent_request_store_t::operation_log(std::string& content, bool write,
	std::string& err) const
{
	if (!valid_run_id(run_id_)) { err = "invalid operation log run ID"; return false; }
	std::string project;
	if (!resolve_project(user_root_, project_path_, project, err)) return false;
	std::lock_guard<webcool::mutex> guard(g_request_store_mutex);
	const std::string directory = join_path(project, ".webcool_agent");
	if (!(write ? make_directory(directory) : normal_directory(directory))) {
		err = "operation log directory unavailable"; return false;
	}
	// Trusted internal root; normal model workspace access remains restricted.
	agent_workspace_t workspace(directory);
	const std::string name = "ai-operations-" + run_id_ + ".jsonl";
	if (write) return workspace.save_generated_text(name, content, err);
	bool truncated = false;
	return workspace.read(name, content, truncated, err) && !truncated;
}

} // namespace ai
} // namespace webcool
