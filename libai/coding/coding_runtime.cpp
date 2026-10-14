#include "coding_internal.h"
#include <algorithm>
#include "../agent/ai_admin_policy.h"
#include "../project/project_toolchain.h"
namespace libai { namespace ai { namespace coding_detail {
// Generic process runtime configuration; the trusted embedding host owns authorization.
bool runtime(const std::string &op, acl::json_node &a, acl::json_node &out, std::string &error)
{
	if (op != "runtime.policy.get" && op != "runtime.policy.validate" && op != "runtime.policy.configure") { error = "unknown runtime policy operation"; return false; }
	ai_admin_policy_t p = ai_runtime_policy_get();
	if (op != "runtime.policy.get") {
		if (a["allow_build_network"]) p.allow_build_network = boolean(a, "allow_build_network", p.allow_build_network);
		if (a["allow_browser_debug"]) p.allow_browser_debug = boolean(a, "allow_browser_debug", p.allow_browser_debug);
		if (a["language_tools"]) p.language_tools = text(a, "language_tools");
		if (a["node_executable_path"]) p.node_executable_path = text(a, "node_executable_path");
		if (a["python_executable_path"]) p.python_executable_path = text(a, "python_executable_path");
		if (a["javac_executable_path"]) p.javac_executable_path = text(a, "javac_executable_path");
		if (a["java_executable_path"]) p.java_executable_path = text(a, "java_executable_path");
		if (a["go_executable_path"]) p.go_executable_path = text(a, "go_executable_path");
		if (a["cargo_executable_path"]) p.cargo_executable_path = text(a, "cargo_executable_path");
		if (a["make_executable_path"]) p.make_executable_path = text(a, "make_executable_path");
		if (a["swift_executable_path"]) p.swift_executable_path = text(a, "swift_executable_path");
		if (a["dotnet_executable_path"]) p.dotnet_executable_path = text(a, "dotnet_executable_path");
		if (a["kotlinc_executable_path"]) p.kotlinc_executable_path = text(a, "kotlinc_executable_path");
		if (a["php_executable_path"]) p.php_executable_path = text(a, "php_executable_path");
		if (a["dmd_executable_path"]) p.dmd_executable_path = text(a, "dmd_executable_path");
		if (a["sensitive_paths"]) p.sensitive_paths = text(a, "sensitive_paths");
		if (a["max_active_runs_per_user"]) p.max_active_runs_per_user = number(a, "max_active_runs_per_user", p.max_active_runs_per_user);
		if (a["max_output_tokens"]) p.max_output_tokens = number(a, "max_output_tokens", p.max_output_tokens);
		if (a["quick_mode_output_tokens"]) p.quick_mode_output_tokens = number(a, "quick_mode_output_tokens", p.quick_mode_output_tokens);
		if (a["provider_connect_timeout_seconds"]) p.provider_connect_timeout_seconds = number(a, "provider_connect_timeout_seconds", p.provider_connect_timeout_seconds);
		if (a["provider_stream_timeout_seconds"]) p.provider_stream_timeout_seconds = number(a, "provider_stream_timeout_seconds", p.provider_stream_timeout_seconds);
		if (a["provider_effective_output_timeout_seconds"]) p.provider_effective_output_timeout_seconds = number(a, "provider_effective_output_timeout_seconds", p.provider_effective_output_timeout_seconds);
		if (a["provider_request_timeout_seconds"]) p.provider_request_timeout_seconds = number(a, "provider_request_timeout_seconds", p.provider_request_timeout_seconds);
		if (a["quick_mode_tool_calls"]) p.quick_mode_tool_calls = number(a, "quick_mode_tool_calls", p.quick_mode_tool_calls);
		if (a["standard_mode_tool_calls"]) p.standard_mode_tool_calls = number(a, "standard_mode_tool_calls", p.standard_mode_tool_calls);
		if (a["large_mode_tool_calls"]) p.large_mode_tool_calls = number(a, "large_mode_tool_calls", p.large_mode_tool_calls);
		if (a["max_tool_calls_per_run"]) p.max_tool_calls_per_run = number(a, "max_tool_calls_per_run", p.max_tool_calls_per_run);
		if (a["max_no_progress_tool_calls"]) p.max_no_progress_tool_calls = number(a, "max_no_progress_tool_calls", p.max_no_progress_tool_calls);
		if (a["browser_debug_report_threshold"]) p.browser_debug_report_threshold = number(a, "browser_debug_report_threshold", p.browser_debug_report_threshold);
		if (a["tool_context_compaction_kib"]) p.tool_context_compaction_kib = number(a, "tool_context_compaction_kib", p.tool_context_compaction_kib);
		if (a["read_chunk_kib"]) p.read_chunk_kib = number(a, "read_chunk_kib", p.read_chunk_kib);
		if (a["read_file_limit_kib"]) p.read_file_limit_kib = number(a, "read_file_limit_kib", p.read_file_limit_kib);
		if (a["sandbox_timeout_ms"]) p.sandbox_limits.timeout_ms = number(a, "sandbox_timeout_ms") * 1ULL;
		if (a["sandbox_cpu_seconds"]) p.sandbox_limits.cpu_seconds = number(a, "sandbox_cpu_seconds") * 1ULL;
		if (a["sandbox_memory_mib"]) p.sandbox_limits.memory_bytes = number(a, "sandbox_memory_mib") * 1048576ULL;
		if (a["sandbox_process_count"]) p.sandbox_limits.process_count = number(a, "sandbox_process_count") * 1ULL;
		if (a["sandbox_output_kib"]) p.sandbox_limits.output_bytes = number(a, "sandbox_output_kib") * 1024ULL;
		p.max_tool_calls_per_run = std::max(p.quick_mode_tool_calls, std::max(p.standard_mode_tool_calls, p.large_mode_tool_calls));
		if (!ai_admin_policy_store_t::validate(p, error)) return false;
		if (op == "runtime.policy.configure") ai_runtime_policy_set(p);
	}
	out.add_bool("allow_build_network", p.allow_build_network);
	out.add_bool("allow_browser_debug", p.allow_browser_debug);
	out.add_text("language_tools", p.language_tools.c_str());
	out.add_text("node_executable_path", p.node_executable_path.c_str());
	out.add_text("node_executable_path_detected", discover_language_executable("node", p.node_executable_path).c_str());
	out.add_text("python_executable_path", p.python_executable_path.c_str());
	out.add_text("python_executable_path_detected", discover_language_executable("python", p.python_executable_path).c_str());
	out.add_text("javac_executable_path", p.javac_executable_path.c_str());
	out.add_text("javac_executable_path_detected", discover_language_executable("javac", p.javac_executable_path).c_str());
	out.add_text("java_executable_path", p.java_executable_path.c_str());
	out.add_text("java_executable_path_detected", discover_language_executable("java", p.java_executable_path).c_str());
	out.add_text("go_executable_path", p.go_executable_path.c_str());
	out.add_text("go_executable_path_detected", discover_language_executable("go", p.go_executable_path).c_str());
	out.add_text("cargo_executable_path", p.cargo_executable_path.c_str());
	out.add_text("cargo_executable_path_detected", discover_language_executable("cargo", p.cargo_executable_path).c_str());
	out.add_text("make_executable_path", p.make_executable_path.c_str());
	out.add_text("make_executable_path_detected", discover_language_executable("make", p.make_executable_path).c_str());
	out.add_text("swift_executable_path", p.swift_executable_path.c_str());
	out.add_text("swift_executable_path_detected", discover_language_executable("swift", p.swift_executable_path).c_str());
	out.add_text("dotnet_executable_path", p.dotnet_executable_path.c_str());
	out.add_text("dotnet_executable_path_detected", discover_language_executable("dotnet", p.dotnet_executable_path).c_str());
	out.add_text("kotlinc_executable_path", p.kotlinc_executable_path.c_str());
	out.add_text("kotlinc_executable_path_detected", discover_language_executable("kotlinc", p.kotlinc_executable_path).c_str());
	out.add_text("php_executable_path", p.php_executable_path.c_str());
	out.add_text("php_executable_path_detected", discover_language_executable("php", p.php_executable_path).c_str());
	out.add_text("dmd_executable_path", p.dmd_executable_path.c_str());
	out.add_text("dmd_executable_path_detected", discover_language_executable("dmd", p.dmd_executable_path).c_str());
	out.add_text("sensitive_paths", p.sensitive_paths.c_str());
	out.add_number("max_active_runs_per_user", p.max_active_runs_per_user);
	out.add_number("max_output_tokens", p.max_output_tokens);
	out.add_number("quick_mode_output_tokens", p.quick_mode_output_tokens);
	out.add_number("provider_connect_timeout_seconds", p.provider_connect_timeout_seconds);
	out.add_number("provider_stream_timeout_seconds", p.provider_stream_timeout_seconds);
	out.add_number("provider_effective_output_timeout_seconds", p.provider_effective_output_timeout_seconds);
	out.add_number("provider_request_timeout_seconds", p.provider_request_timeout_seconds);
	out.add_number("quick_mode_tool_calls", p.quick_mode_tool_calls);
	out.add_number("standard_mode_tool_calls", p.standard_mode_tool_calls);
	out.add_number("large_mode_tool_calls", p.large_mode_tool_calls);
	out.add_number("max_tool_calls_per_run", p.max_tool_calls_per_run);
	out.add_number("max_no_progress_tool_calls", p.max_no_progress_tool_calls);
	out.add_number("browser_debug_report_threshold", p.browser_debug_report_threshold);
	out.add_number("tool_context_compaction_kib", p.tool_context_compaction_kib);
	out.add_number("read_chunk_kib", p.read_chunk_kib);
	out.add_number("read_file_limit_kib", p.read_file_limit_kib);
	out.add_number("sandbox_timeout_ms", p.sandbox_limits.timeout_ms / 1ULL);
	out.add_number("sandbox_cpu_seconds", p.sandbox_limits.cpu_seconds / 1ULL);
	out.add_number("sandbox_memory_mib", p.sandbox_limits.memory_bytes / 1048576ULL);
	out.add_number("sandbox_process_count", p.sandbox_limits.process_count / 1ULL);
	out.add_number("sandbox_output_kib", p.sandbox_limits.output_bytes / 1024ULL);
	return true;
}
} } }
