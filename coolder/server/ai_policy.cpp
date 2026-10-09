#include "stdafx.h"
#include "action/actions.h"
#include "action/action_util.h"
#include "libai/agent/ai_admin_policy.h"
#include "libai/common/ai_error_log.h"
#include "libai/provider/ai_provider_store.h"
#include "libai/project/project_toolchain.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <string>

namespace action
{
namespace
{

std::string node_text(acl::json_node *node)
{
	if (node == NULL) {
		return "";
	}
	const char *value = node->get_string();
	if (!(value == NULL))
		return value ? value : "";
	value = node->get_text();

	return value ? value : "";
}

bool node_bool(acl::json_node *node, bool fallback)
{
	const std::string value = node_text(node);
	if (value == "true" || value == "1") {
		return true;
	}
	if (!(value == "false" || value == "0"))
		return fallback;
	return false;
}

unsigned long node_ulong(acl::json_node *node, unsigned long fallback)
{
	const std::string value = node_text(node);
	if (value.empty()) {
		return fallback;
	}
	char *end = NULL;
	errno = 0;
	const unsigned long parsed = strtoul(value.c_str(), &end, 10);
	return errno == 0 && end != value.c_str() && *end == '\0' ? parsed :
	                                                            fallback;
}

void append_executable(acl::json_node &root, const char *key, const char *tool,
    const std::string &configured)
{
	const std::string detected =
	    webcool::ai::discover_language_executable(tool, configured);
	root.add_text(key, configured.c_str());
	root.add_text(
	    (std::string(key) + "_detected").c_str(), detected.c_str());
	root.add_bool((std::string(key) + "_auto_detected").c_str(),
	    configured.empty() && !detected.empty());
}

void append_policy(
    acl::json_node &root, const webcool::ai::ai_admin_policy_t &policy)
{
	root.add_bool("enabled", policy.enabled);
	root.add_bool("allow_admin", policy.allow_admin);
	root.add_bool("allow_users", policy.allow_users);
	root.add_bool(
	    "allow_users_shared_projects", policy.allow_users_shared_projects);
	root.add_bool(
	    "allow_users_local_projects", policy.allow_users_local_projects);
	root.add_bool("allow_build_network", policy.allow_build_network);
	root.add_bool("allow_browser_debug", policy.allow_browser_debug);
	root.add_bool("browser_debug_available", false);
	root.add_bool("browser_debug_enabled", false);
	root.add_text("browser_debug_unavailable_reason",
	    "coolder 尚未接入浏览器调试扩展，此选项暂不可用。");
	root.add_text("language_tools", policy.language_tools.c_str());
	append_executable(
	    root, "node_executable_path", "node", policy.node_executable_path);
	append_executable(root, "python_executable_path", "python",
	    policy.python_executable_path);
	append_executable(root, "javac_executable_path", "javac",
	    policy.javac_executable_path);
	append_executable(
	    root, "java_executable_path", "java", policy.java_executable_path);
	append_executable(
	    root, "go_executable_path", "go", policy.go_executable_path);
	append_executable(root, "cargo_executable_path", "cargo",
	    policy.cargo_executable_path);
	append_executable(
	    root, "make_executable_path", "make", policy.make_executable_path);
	append_executable(root, "swift_executable_path", "swift",
	    policy.swift_executable_path);
	append_executable(root, "dotnet_executable_path", "dotnet",
	    policy.dotnet_executable_path);
	append_executable(root, "kotlinc_executable_path", "kotlinc",
	    policy.kotlinc_executable_path);
	append_executable(
	    root, "php_executable_path", "php", policy.php_executable_path);
	append_executable(
	    root, "dmd_executable_path", "dmd", policy.dmd_executable_path);
	root.add_text("sensitive_paths", policy.sensitive_paths.c_str());
	root.add_number("max_active_runs_per_user",
	    static_cast<long long>(policy.max_active_runs_per_user));
	root.add_number("max_output_tokens",
	    static_cast<long long>(policy.max_output_tokens));
	root.add_number("quick_mode_output_tokens",
	    static_cast<long long>(policy.quick_mode_output_tokens));
	root.add_number("provider_connect_timeout_seconds",
	    static_cast<long long>(policy.provider_connect_timeout_seconds));
	root.add_number("provider_effective_output_timeout_seconds",
	    static_cast<long long>(
	        policy.provider_effective_output_timeout_seconds));
	root.add_number("provider_request_timeout_seconds",
	    static_cast<long long>(policy.provider_request_timeout_seconds));
	root.add_number("provider_stream_timeout_seconds",
	    static_cast<long long>(policy.provider_stream_timeout_seconds));
	root.add_number("max_tool_calls_per_run",
	    static_cast<long long>(policy.max_tool_calls_per_run));
	root.add_number("quick_mode_tool_calls",
	    static_cast<long long>(policy.quick_mode_tool_calls));
	root.add_number("standard_mode_tool_calls",
	    static_cast<long long>(policy.standard_mode_tool_calls));
	root.add_number("large_mode_tool_calls",
	    static_cast<long long>(policy.large_mode_tool_calls));
	root.add_number("browser_debug_report_threshold",
	    static_cast<long long>(policy.browser_debug_report_threshold));
	root.add_number("max_no_progress_tool_calls",
	    static_cast<long long>(policy.max_no_progress_tool_calls));
	root.add_number("tool_context_compaction_kib",
	    static_cast<long long>(policy.tool_context_compaction_kib));
	root.add_number(
	    "read_chunk_kib", static_cast<long long>(policy.read_chunk_kib));
	root.add_number("read_file_limit_kib",
	    static_cast<long long>(policy.read_file_limit_kib));
	root.add_number("sandbox_timeout_ms",
	    static_cast<long long>(policy.sandbox_limits.timeout_ms));
	root.add_number("sandbox_cpu_seconds",
	    static_cast<long long>(policy.sandbox_limits.cpu_seconds));
	root.add_number("sandbox_memory_mib",
	    static_cast<long long>(
	        policy.sandbox_limits.memory_bytes / (1024ULL * 1024ULL)));
	root.add_number("sandbox_process_count",
	    static_cast<long long>(policy.sandbox_limits.process_count));
	root.add_number("sandbox_output_kib",
	    static_cast<long long>(
	        policy.sandbox_limits.output_bytes / 1024UL));
}

} // namespace

bool AdminAiPolicyAction::run(
    request_t &req, response_t &res, const std::string &upload_root)
{
	webcool::ai::ai_admin_policy_store_t store(upload_root);
	webcool::ai::ai_admin_policy_t policy;
	std::string err;
	if (!store.load(policy, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (req.getMethod() == acl::HTTP_METHOD_POST) {
		acl::json *body = req.getJson(64 * 1024);
		if (body == NULL) {
			webcool::ai::ai_log_error("http.admin.ai-policy",
			    "parse-json", "invalid JSON body");
			json_error(
			    res, 400, "invalid JSON body", req.isKeepAlive());
			return true;
		}
		for (const char *key :
		    { "allow_browser_debug", "allow_users_shared_projects" }) {
			if (!node_bool((*body)[key], false))
				continue;
			json_error(res, 400,
			    "coolder 尚未接入浏览器调试或共享目录模块",
			    req.isKeepAlive());
			return true;
		}
		policy.allow_browser_debug = node_bool(
		    (*body)["allow_browser_debug"], policy.allow_browser_debug);
		policy.enabled = node_bool((*body)["enabled"], policy.enabled);
		policy.allow_build_network = node_bool(
		    (*body)["allow_build_network"], policy.allow_build_network);
		policy.allow_admin =
		    node_bool((*body)["allow_admin"], policy.allow_admin);
		policy.allow_users =
		    node_bool((*body)["allow_users"], policy.allow_users);
		policy.allow_users_shared_projects =
		    node_bool((*body)["allow_users_shared_projects"],
		        policy.allow_users_shared_projects);
		policy.allow_users_local_projects =
		    node_bool((*body)["allow_users_local_projects"],
		        policy.allow_users_local_projects);
		// POST is a patch operation. Per-field save buttons intentionally submit
		// only one key, so an omitted string must retain its durable value rather
		// than being mistaken for an explicit request to clear it.
		if ((*body)["language_tools"] != NULL) {
			policy.language_tools =
			    node_text((*body)["language_tools"]);
		}
		if ((*body)["node_executable_path"] != NULL) {
			policy.node_executable_path =
			    node_text((*body)["node_executable_path"]);
		}
		if ((*body)["python_executable_path"] != NULL) {
			policy.python_executable_path =
			    node_text((*body)["python_executable_path"]);
		}
		if ((*body)["javac_executable_path"] != NULL) {
			policy.javac_executable_path =
			    node_text((*body)["javac_executable_path"]);
		}
		if ((*body)["java_executable_path"] != NULL) {
			policy.java_executable_path =
			    node_text((*body)["java_executable_path"]);
		}
		if ((*body)["go_executable_path"] != NULL) {
			policy.go_executable_path =
			    node_text((*body)["go_executable_path"]);
		}
		if ((*body)["cargo_executable_path"] != NULL) {
			policy.cargo_executable_path =
			    node_text((*body)["cargo_executable_path"]);
		}
		if ((*body)["make_executable_path"] != NULL) {
			policy.make_executable_path =
			    node_text((*body)["make_executable_path"]);
		}
		if ((*body)["swift_executable_path"] != NULL) {
			policy.swift_executable_path =
			    node_text((*body)["swift_executable_path"]);
		}
		if ((*body)["dotnet_executable_path"] != NULL) {
			policy.dotnet_executable_path =
			    node_text((*body)["dotnet_executable_path"]);
		}
		if ((*body)["kotlinc_executable_path"] != NULL) {
			policy.kotlinc_executable_path =
			    node_text((*body)["kotlinc_executable_path"]);
		}
		if ((*body)["php_executable_path"] != NULL) {
			policy.php_executable_path =
			    node_text((*body)["php_executable_path"]);
		}
		if ((*body)["dmd_executable_path"] != NULL) {
			policy.dmd_executable_path =
			    node_text((*body)["dmd_executable_path"]);
		}
		if ((*body)["sensitive_paths"] != NULL) {
			policy.sensitive_paths =
			    node_text((*body)["sensitive_paths"]);
		}
		policy.max_active_runs_per_user =
		    node_ulong((*body)["max_active_runs_per_user"],
		        policy.max_active_runs_per_user);
		policy.max_output_tokens = node_ulong(
		    (*body)["max_output_tokens"], policy.max_output_tokens);
		policy.quick_mode_output_tokens =
		    node_ulong((*body)["quick_mode_output_tokens"],
		        policy.quick_mode_output_tokens);
		policy.provider_connect_timeout_seconds =
		    node_ulong((*body)["provider_connect_timeout_seconds"],
		        policy.provider_connect_timeout_seconds);
		policy.provider_effective_output_timeout_seconds = node_ulong(
		    (*body)["provider_effective_output_timeout_seconds"],
		    policy.provider_effective_output_timeout_seconds);
		policy.provider_request_timeout_seconds =
		    node_ulong((*body)["provider_request_timeout_seconds"],
		        policy.provider_request_timeout_seconds);
		policy.provider_stream_timeout_seconds =
		    node_ulong((*body)["provider_stream_timeout_seconds"],
		        policy.provider_stream_timeout_seconds);
		policy.quick_mode_tool_calls =
		    node_ulong((*body)["quick_mode_tool_calls"],
		        policy.quick_mode_tool_calls);
		policy.standard_mode_tool_calls =
		    node_ulong((*body)["standard_mode_tool_calls"],
		        policy.standard_mode_tool_calls);
		policy.large_mode_tool_calls =
		    node_ulong((*body)["large_mode_tool_calls"],
		        policy.large_mode_tool_calls);
		policy.max_tool_calls_per_run =
		    std::max(policy.quick_mode_tool_calls,
		        std::max(policy.standard_mode_tool_calls,
		            policy.large_mode_tool_calls));
		if ((*body)["browser_debug_report_threshold"]) {
			policy.browser_debug_report_threshold = node_ulong(
			    (*body)["browser_debug_report_threshold"], 0);
		}
		policy.max_no_progress_tool_calls =
		    node_ulong((*body)["max_no_progress_tool_calls"],
		        policy.max_no_progress_tool_calls);
		policy.tool_context_compaction_kib =
		    node_ulong((*body)["tool_context_compaction_kib"],
		        policy.tool_context_compaction_kib);
		policy.read_chunk_kib = node_ulong(
		    (*body)["read_chunk_kib"], policy.read_chunk_kib);
		policy.read_file_limit_kib = node_ulong(
		    (*body)["read_file_limit_kib"], policy.read_file_limit_kib);
		policy.sandbox_limits.timeout_ms =
		    node_ulong((*body)["sandbox_timeout_ms"],
		        policy.sandbox_limits.timeout_ms);
		policy.sandbox_limits.cpu_seconds =
		    node_ulong((*body)["sandbox_cpu_seconds"],
		        policy.sandbox_limits.cpu_seconds);
		policy.sandbox_limits.memory_bytes =
		    static_cast<unsigned long long>(
		        node_ulong((*body)["sandbox_memory_mib"],
		            static_cast<unsigned long>(
		                policy.sandbox_limits.memory_bytes /
		                (1024ULL * 1024ULL)))) *
		    1024ULL * 1024ULL;
		policy.sandbox_limits.process_count =
		    node_ulong((*body)["sandbox_process_count"],
		        policy.sandbox_limits.process_count);
		policy.sandbox_limits.output_bytes =
		    node_ulong((*body)["sandbox_output_kib"],
		        policy.sandbox_limits.output_bytes / 1024UL) *
		    1024UL;
		policy.allow_browser_debug = false;
		policy.allow_users_shared_projects = false;
		if (!store.save(policy, err)) {
			json_error(res, 400, err.c_str(), req.isKeepAlive());
			return true;
		}
		// New runs must observe a successfully saved policy immediately. Existing
		// runs keep the immutable snapshot captured at their own start boundary.
		webcool::ai::ai_runtime_policy_set(policy);
	} else {
		webcool::ai::ai_runtime_policy_set(policy);
	}
	acl::json json;
	acl::json_node &root = json.create_node();
	root.add_bool("ok", true);
	append_policy(root, policy);
	return sendJson(res, 200, root, req.isKeepAlive());
}

} // namespace action
