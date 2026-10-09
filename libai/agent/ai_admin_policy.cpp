#include "stdafx.h"
#include "ai_admin_policy.h"
#include "../common/ai_error_log.h"
#include "../project/supported_languages.h"
#include <mutex>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include "../common/platform_compat.h"
#include <direct.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace webcool
{
namespace ai
{
namespace
{

std::mutex g_policy_mutex;
std::mutex g_policy_store_mutex;
ai_admin_policy_t g_runtime_policy;

std::string policy_directory(const std::string &root)
{
	return root + "/.webcool_settings";
}

std::string policy_path(const std::string &root)
{
	return policy_directory(root) + "/ai-policy.v1";
}

bool parse_bool(const std::string &value, bool fallback)
{
	if (value == "1" || value == "true")
		return true;
	if (value == "0" || value == "false")
		return false;
	return fallback;
}

bool parse_ulong(const std::string &value, unsigned long &result)
{
	char *end = NULL;
	errno = 0;
	const unsigned long parsed = strtoul(value.c_str(), &end, 10);
	if (errno != 0 || end == value.c_str() || *end != '\0')
		return false;
	result = parsed;
	return true;
}

bool safe_list(const std::string &value)
{
	if (value.size() > 4096 || value.find('\n') != std::string::npos ||
	    value.find('\r') != std::string::npos ||
	    value.find('\0') != std::string::npos) {
		return false;
	}
	for (size_t i = 0; i < value.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(value[i]);
		if (ch < 32 && ch != '\t')
			return false;
	}
	return true;
}

bool supported_language_tools(const std::string &value)
{
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find(',', begin);
		std::string item = value.substr(
			begin, end == std::string::npos ? std::string::npos :
							  end - begin);
		while (!item.empty() && (item[0] == ' ' || item[0] == '\t'))
			item.erase(0, 1);
		while (!item.empty() && (item[item.size() - 1] == ' ' ||
					 item[item.size() - 1] == '\t'))
			item.resize(item.size() - 1);
		if (!item.empty()) {
			if (!supported_configurable_language_tool(item))
				return false;
		} else if (!value.empty())
			return false;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return true;
}

bool safe_executable_path(const std::string &value)
{
	if (value.empty())
		return true;
	if (value.size() > 2048 || value.find('\n') != std::string::npos ||
	    value.find('\r') != std::string::npos ||
	    value.find('\0') != std::string::npos)
		return false;
#ifdef _WIN32
	// Accept a drive-absolute path or UNC path. Existence and reparse-point
	// checks are repeated whenever the toolchain is resolved.
	return (value.size() >= 3 && value[1] == ':' &&
		(value[2] == '\\' || value[2] == '/')) ||
	       (value.size() >= 2 && value[0] == '\\' && value[1] == '\\');
#else
	return value[0] == '/';
#endif
}

bool list_contains(const std::string &value, const std::string &wanted,
		   bool prefix)
{
	size_t begin = 0;
	while (begin <= value.size()) {
		const size_t end = value.find(',', begin);
		std::string item = value.substr(
			begin, end == std::string::npos ? std::string::npos :
							  end - begin);
		while (!item.empty() && (item[0] == ' ' || item[0] == '\t'))
			item.erase(0, 1);
		while (!item.empty() && (item[item.size() - 1] == ' ' ||
					 item[item.size() - 1] == '\t'))
			item.resize(item.size() - 1);
		if ((!prefix && item == wanted) ||
		    (prefix && !item.empty() &&
		     (wanted == item ||
		      (wanted.size() > item.size() &&
		       wanted.compare(0, item.size(), item) == 0 &&
		       wanted[item.size()] == '/'))))
			return true;
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return false;
}

} // namespace

ai_admin_policy_t::ai_admin_policy_t()
{
	// Keep output returned to a model deliberately smaller than the generic
	// broker default; administrators may raise it within validate() limits.
	sandbox_limits.output_bytes = 256UL * 1024UL;
}

ai_admin_policy_store_t::ai_admin_policy_store_t(const std::string &upload_root)
	: upload_root_(upload_root)
{
}

bool ai_admin_policy_store_t::validate(const ai_admin_policy_t &policy,
				       std::string &err)
{
	if (!safe_list(policy.language_tools) ||
	    !supported_language_tools(policy.language_tools) ||
	    !safe_list(policy.sensitive_paths) ||
	    !safe_executable_path(policy.node_executable_path) ||
	    !safe_executable_path(policy.python_executable_path) ||
	    !safe_executable_path(policy.javac_executable_path) ||
	    !safe_executable_path(policy.java_executable_path) ||
	    !safe_executable_path(policy.go_executable_path) ||
	    !safe_executable_path(policy.cargo_executable_path) ||
	    !safe_executable_path(policy.make_executable_path) ||
	    !safe_executable_path(policy.swift_executable_path) ||
	    !safe_executable_path(policy.dotnet_executable_path) ||
	    !safe_executable_path(policy.kotlinc_executable_path) ||
	    !safe_executable_path(policy.php_executable_path) ||
	    !safe_executable_path(policy.dmd_executable_path)) {
		err = "AI policy contains an unsupported language or invalid executable path";
		return ai_error("ai.admin-policy", "validate-lists", err);
	}
	if (policy.max_active_runs_per_user < 1 ||
	    policy.max_active_runs_per_user > 16 ||
	    policy.max_output_tokens < 256 ||
	    policy.max_output_tokens > 1000000 ||
	    policy.quick_mode_output_tokens < 256 ||
	    policy.quick_mode_output_tokens > 256UL * 1024UL ||
	    policy.provider_connect_timeout_seconds < 5 ||
	    policy.provider_connect_timeout_seconds > 300 ||
	    policy.provider_stream_timeout_seconds < 30 ||
	    policy.provider_stream_timeout_seconds > 3600 ||
	    policy.provider_effective_output_timeout_seconds < 30 ||
	    policy.provider_effective_output_timeout_seconds > 3600 ||
	    policy.provider_request_timeout_seconds < 30 ||
	    policy.provider_request_timeout_seconds > 7200 ||
	    policy.quick_mode_tool_calls < 8 ||
	    policy.quick_mode_tool_calls > 256 ||
	    policy.standard_mode_tool_calls < 8 ||
	    policy.standard_mode_tool_calls > 256 ||
	    policy.large_mode_tool_calls < 8 ||
	    policy.large_mode_tool_calls > 256 ||
	    policy.max_tool_calls_per_run < policy.quick_mode_tool_calls ||
	    policy.max_tool_calls_per_run < policy.standard_mode_tool_calls ||
	    policy.max_tool_calls_per_run < policy.large_mode_tool_calls ||
	    policy.max_tool_calls_per_run > 256 ||
	    policy.browser_debug_report_threshold < 1 ||
	    policy.browser_debug_report_threshold > 20 ||
	    policy.max_no_progress_tool_calls < 3 ||
	    policy.max_no_progress_tool_calls > 32 ||
	    policy.max_no_progress_tool_calls > policy.quick_mode_tool_calls ||
	    policy.tool_context_compaction_kib < 16 ||
	    policy.tool_context_compaction_kib > 96 ||
	    policy.read_chunk_kib < 1 || policy.read_chunk_kib > 64 ||
	    policy.read_file_limit_kib < policy.read_chunk_kib ||
	    policy.read_file_limit_kib > 1024 ||
	    policy.sandbox_limits.timeout_ms < 1000 ||
	    policy.sandbox_limits.timeout_ms > 300000 ||
	    policy.sandbox_limits.cpu_seconds < 1 ||
	    policy.sandbox_limits.cpu_seconds > 300 ||
	    policy.sandbox_limits.memory_bytes < 64ULL * 1024ULL * 1024ULL ||
	    policy.sandbox_limits.memory_bytes >
		    16ULL * 1024ULL * 1024ULL * 1024ULL ||
	    policy.sandbox_limits.process_count < 1 ||
	    policy.sandbox_limits.process_count > 256 ||
	    policy.sandbox_limits.output_bytes < 4096 ||
	    policy.sandbox_limits.output_bytes > 16UL * 1024UL * 1024UL) {
		err = "AI policy resource limits are outside the safe range";
		return ai_error("ai.admin-policy", "validate-limits", err);
	}
	return true;
}

bool ai_admin_policy_store_t::load(ai_admin_policy_t &policy,
				   std::string &err) const
{
	std::lock_guard<std::mutex> guard(g_policy_store_mutex);
	policy = ai_admin_policy_t();
	std::ifstream in(policy_path(upload_root_).c_str(), std::ios::in);
	if (!in.good())
		return true;
	bool has_mode_tool_calls = false;
	unsigned long legacy_tool_calls = policy.max_tool_calls_per_run;
	std::string line;
	while (std::getline(in, line)) {
		const size_t equal = line.find('=');
		if (equal == std::string::npos) {
			err = "invalid AI policy database";
			return ai_error("ai.admin-policy", "parse-line", err);
		}
		const std::string key = line.substr(0, equal);
		const std::string value = line.substr(equal + 1);
		unsigned long number = 0;
		if (key == "enabled")
			policy.enabled = parse_bool(value, policy.enabled);
		else if (key == "allow_admin")
			policy.allow_admin =
				parse_bool(value, policy.allow_admin);
		else if (key == "allow_users")
			policy.allow_users =
				parse_bool(value, policy.allow_users);
		else if (key == "allow_users_shared_projects")
			policy.allow_users_shared_projects = parse_bool(
				value, policy.allow_users_shared_projects);
		else if (key == "allow_browser_debug")
			policy.allow_browser_debug =
				parse_bool(value, policy.allow_browser_debug);
		else if (key == "allow_build_network")
			policy.allow_build_network =
				parse_bool(value, policy.allow_build_network);
		else if (key == "allow_users_local_projects")
			policy.allow_users_local_projects = parse_bool(
				value, policy.allow_users_local_projects);
		else if (key == "language_tools")
			policy.language_tools = value;
		else if (key == "node_executable_path")
			policy.node_executable_path = value;
		else if (key == "python_executable_path")
			policy.python_executable_path = value;
		else if (key == "javac_executable_path")
			policy.javac_executable_path = value;
		else if (key == "java_executable_path")
			policy.java_executable_path = value;
		else if (key == "go_executable_path")
			policy.go_executable_path = value;
		else if (key == "cargo_executable_path")
			policy.cargo_executable_path = value;
		else if (key == "make_executable_path")
			policy.make_executable_path = value;
		else if (key == "swift_executable_path")
			policy.swift_executable_path = value;
		else if (key == "dotnet_executable_path")
			policy.dotnet_executable_path = value;
		else if (key == "kotlinc_executable_path")
			policy.kotlinc_executable_path = value;
		else if (key == "php_executable_path")
			policy.php_executable_path = value;
		else if (key == "dmd_executable_path")
			policy.dmd_executable_path = value;
		else if (key == "sensitive_paths")
			policy.sensitive_paths = value;
		else if (key == "max_active_runs_per_user" &&
			 parse_ulong(value, number))
			policy.max_active_runs_per_user = number;
		else if (key == "max_output_tokens" &&
			 parse_ulong(value, number))
			policy.max_output_tokens = number;
		else if (key == "quick_mode_output_tokens" &&
			 parse_ulong(value, number))
			policy.quick_mode_output_tokens = number;
		else if (key == "provider_connect_timeout_seconds" &&
			 parse_ulong(value, number))
			policy.provider_connect_timeout_seconds = number;
		else if (key == "provider_effective_output_timeout_seconds" &&
			 parse_ulong(value, number))
			policy.provider_effective_output_timeout_seconds =
				number;
		else if (key == "provider_request_timeout_seconds" &&
			 parse_ulong(value, number))
			policy.provider_request_timeout_seconds = number;
		else if (key == "provider_stream_timeout_seconds" &&
			 parse_ulong(value, number))
			policy.provider_stream_timeout_seconds = number;
		else if (key == "quick_mode_tool_calls" &&
			 parse_ulong(value, number)) {
			policy.quick_mode_tool_calls = number;
			has_mode_tool_calls = true;
		} else if (key == "standard_mode_tool_calls" &&
			   parse_ulong(value, number)) {
			policy.standard_mode_tool_calls = number;
			has_mode_tool_calls = true;
		} else if (key == "large_mode_tool_calls" &&
			   parse_ulong(value, number)) {
			policy.large_mode_tool_calls = number;
			has_mode_tool_calls = true;
		} else if (key == "max_tool_calls_per_run" &&
			   parse_ulong(value, number))
			legacy_tool_calls = number;
		else if (key == "browser_debug_report_threshold" &&
			 parse_ulong(value, number))
			policy.browser_debug_report_threshold = number;
		else if (key == "max_no_progress_tool_calls" &&
			 parse_ulong(value, number))
			policy.max_no_progress_tool_calls = number;
		else if (key == "tool_context_compaction_kib" &&
			 parse_ulong(value, number))
			policy.tool_context_compaction_kib = number;
		else if (key == "read_chunk_kib" && parse_ulong(value, number))
			policy.read_chunk_kib = number;
		else if (key == "read_file_limit_kib" &&
			 parse_ulong(value, number))
			policy.read_file_limit_kib = number;
		else if (key == "sandbox_timeout_ms" &&
			 parse_ulong(value, number))
			policy.sandbox_limits.timeout_ms = number;
		else if (key == "sandbox_cpu_seconds" &&
			 parse_ulong(value, number))
			policy.sandbox_limits.cpu_seconds = number;
		else if (key == "sandbox_memory_mib" &&
			 parse_ulong(value, number))
			policy.sandbox_limits.memory_bytes =
				static_cast<unsigned long long>(number) *
				1024ULL * 1024ULL;
		else if (key == "sandbox_process_count" &&
			 parse_ulong(value, number))
			policy.sandbox_limits.process_count = number;
		else if (key == "sandbox_output_kib" &&
			 parse_ulong(value, number))
			policy.sandbox_limits.output_bytes = number * 1024UL;
	}
	if (!has_mode_tool_calls) {
		// Upgrade an old one-limit policy without preserving its former low default.
		// A deliberately raised legacy value still expands standard/large modes.
		policy.standard_mode_tool_calls = std::min<unsigned long>(
			256, std::max<unsigned long>(64, legacy_tool_calls));
		policy.large_mode_tool_calls = std::min<unsigned long>(
			256, std::max<unsigned long>(
				     128, legacy_tool_calls > 128 ?
						  legacy_tool_calls :
						  legacy_tool_calls * 2));
	}
	policy.max_tool_calls_per_run =
		std::max(policy.quick_mode_tool_calls,
			 std::max(policy.standard_mode_tool_calls,
				  policy.large_mode_tool_calls));
	return validate(policy, err);
}

bool ai_admin_policy_store_t::save(const ai_admin_policy_t &policy,
				   std::string &err) const
{
	std::lock_guard<std::mutex> guard(g_policy_store_mutex);
	if (!validate(policy, err))
		return false;
	const std::string directory = policy_directory(upload_root_);
	bool directory_ok = false;
#ifdef _WIN32
	std::wstring wide;
	if (webcool_utf8_path_to_wide(directory.c_str(), wide)) {
		const DWORD attrs = GetFileAttributesW(wide.c_str());
		directory_ok = (attrs != INVALID_FILE_ATTRIBUTES &&
				(attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
				(attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) ||
			       (_wmkdir(wide.c_str()) == 0);
	}
#else
	struct stat st;
	directory_ok = (lstat(directory.c_str(), &st) == 0 &&
			S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) ||
		       (mkdir(directory.c_str(), 0700) == 0);
	if (directory_ok)
		directory_ok = chmod(directory.c_str(), 0700) == 0;
#endif
	if (!directory_ok) {
		err = "cannot create AI policy directory";
		return ai_error("ai.admin-policy", "create-directory", err);
	}
	const std::string path = policy_path(upload_root_);
	const std::string temporary = path + ".tmp";
	std::ofstream out(temporary.c_str(), std::ios::out | std::ios::trunc);
	if (!out.good()) {
		err = "cannot write AI policy database";
		return ai_error("ai.admin-policy", "open-temporary", err);
	}
	out << "enabled=" << (policy.enabled ? 1 : 0) << '\n'
	    << "allow_admin=" << (policy.allow_admin ? 1 : 0) << '\n'
	    << "allow_users=" << (policy.allow_users ? 1 : 0) << '\n'
	    << "allow_users_shared_projects="
	    << (policy.allow_users_shared_projects ? 1 : 0) << '\n'
	    << "allow_browser_debug=" << (policy.allow_browser_debug ? 1 : 0)
	    << '\n'
	    << "allow_build_network=" << (policy.allow_build_network ? 1 : 0)
	    << '\n'
	    << "allow_users_local_projects="
	    << (policy.allow_users_local_projects ? 1 : 0) << '\n'
	    << "language_tools=" << policy.language_tools << '\n'
	    << "node_executable_path=" << policy.node_executable_path << '\n'
	    << "python_executable_path=" << policy.python_executable_path
	    << '\n'
	    << "javac_executable_path=" << policy.javac_executable_path << '\n'
	    << "java_executable_path=" << policy.java_executable_path << '\n'
	    << "go_executable_path=" << policy.go_executable_path << '\n'
	    << "cargo_executable_path=" << policy.cargo_executable_path << '\n'
	    << "make_executable_path=" << policy.make_executable_path << '\n'
	    << "swift_executable_path=" << policy.swift_executable_path << '\n'
	    << "dotnet_executable_path=" << policy.dotnet_executable_path
	    << '\n'
	    << "kotlinc_executable_path=" << policy.kotlinc_executable_path
	    << '\n'
	    << "php_executable_path=" << policy.php_executable_path << '\n'
	    << "dmd_executable_path=" << policy.dmd_executable_path << '\n'
	    << "sensitive_paths=" << policy.sensitive_paths << '\n'
	    << "max_active_runs_per_user=" << policy.max_active_runs_per_user
	    << '\n'
	    << "max_output_tokens=" << policy.max_output_tokens << '\n'
	    << "quick_mode_output_tokens=" << policy.quick_mode_output_tokens
	    << '\n'
	    << "provider_connect_timeout_seconds="
	    << policy.provider_connect_timeout_seconds << '\n'
	    << "provider_effective_output_timeout_seconds="
	    << policy.provider_effective_output_timeout_seconds << '\n'
	    << "provider_request_timeout_seconds="
	    << policy.provider_request_timeout_seconds << '\n'
	    << "provider_stream_timeout_seconds="
	    << policy.provider_stream_timeout_seconds << '\n'
	    << "quick_mode_tool_calls=" << policy.quick_mode_tool_calls << '\n'
	    << "standard_mode_tool_calls=" << policy.standard_mode_tool_calls
	    << '\n'
	    << "large_mode_tool_calls=" << policy.large_mode_tool_calls << '\n'
	    << "max_tool_calls_per_run=" << policy.max_tool_calls_per_run
	    << '\n'
	    << "browser_debug_report_threshold="
	    << policy.browser_debug_report_threshold << '\n'
	    << "max_no_progress_tool_calls="
	    << policy.max_no_progress_tool_calls << '\n'
	    << "tool_context_compaction_kib="
	    << policy.tool_context_compaction_kib << '\n'
	    << "read_chunk_kib=" << policy.read_chunk_kib << '\n'
	    << "read_file_limit_kib=" << policy.read_file_limit_kib << '\n'
	    << "sandbox_timeout_ms=" << policy.sandbox_limits.timeout_ms << '\n'
	    << "sandbox_cpu_seconds=" << policy.sandbox_limits.cpu_seconds
	    << '\n'
	    << "sandbox_memory_mib="
	    << policy.sandbox_limits.memory_bytes / (1024ULL * 1024ULL) << '\n'
	    << "sandbox_process_count=" << policy.sandbox_limits.process_count
	    << '\n'
	    << "sandbox_output_kib="
	    << policy.sandbox_limits.output_bytes / 1024UL << '\n';
	out.close();
	if (!out.good()) {
		remove(temporary.c_str());
		err = "cannot flush AI policy database";
		return ai_error("ai.admin-policy", "flush-temporary", err);
	}
#ifndef _WIN32
	if (chmod(temporary.c_str(), 0600) != 0) {
		remove(temporary.c_str());
		err = "cannot protect AI policy database";
		return ai_error("ai.admin-policy", "protect-temporary", err);
	}
#endif
	bool installed = false;
#ifdef _WIN32
	std::wstring temporary_wide;
	std::wstring path_wide;
	installed =
		webcool_utf8_path_to_wide(temporary.c_str(), temporary_wide) &&
		webcool_utf8_path_to_wide(path.c_str(), path_wide) &&
		MoveFileExW(temporary_wide.c_str(), path_wide.c_str(),
			    MOVEFILE_REPLACE_EXISTING |
				    MOVEFILE_WRITE_THROUGH) != 0;
#else
	installed = rename(temporary.c_str(), path.c_str()) == 0;
#endif
	if (!installed) {
		remove(temporary.c_str());
		err = std::string("cannot install AI policy database: ") +
		      strerror(errno);
		return ai_error("ai.admin-policy", "install", err);
	}
	ai_runtime_policy_set(policy);
	return true;
}

void ai_runtime_policy_set(const ai_admin_policy_t &policy)
{
	std::lock_guard<std::mutex> guard(g_policy_mutex);
	g_runtime_policy = policy;
}

ai_admin_policy_t ai_runtime_policy_get()
{
	std::lock_guard<std::mutex> guard(g_policy_mutex);
	return g_runtime_policy;
}

bool ai_agent_access_allowed(const ai_admin_policy_t &policy, bool admin)
{
	return policy.enabled &&
	       (admin ? policy.allow_admin : policy.allow_users);
}

bool ai_language_tool_enabled(const std::string &language)
{
	return list_contains(ai_runtime_policy_get().language_tools, language,
			     false);
}

bool ai_extra_sensitive_path(const std::string &normalized_path)
{
	return list_contains(ai_runtime_policy_get().sensitive_paths,
			     normalized_path, true);
}

unsigned long ai_tool_call_limit_for_mode(const ai_admin_policy_t &policy,
					  const std::string &mode)
{
	if (mode == "quick")
		return policy.quick_mode_tool_calls;
	if (mode == "large")
		return policy.large_mode_tool_calls;
	return policy.standard_mode_tool_calls;
}

} // namespace ai
} // namespace webcool
