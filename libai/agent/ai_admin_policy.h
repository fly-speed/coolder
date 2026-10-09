#pragma once

#include "../sandbox/program_sandbox.h"

#include <string>

namespace webcool
{
namespace ai
{

// Server-wide policy controlled only by an administrator. It deliberately
// contains no provider credentials and is shared by every agent type.
struct ai_admin_policy_t {
	ai_admin_policy_t();
	bool enabled = true;
	bool allow_admin = true;
	bool allow_users = true;
	// External project locations are privileged expansion points. Administrators
	// may always add them; ordinary users need these explicit grants.
	bool allow_users_shared_projects = false;
	bool allow_users_local_projects = false;
	// Explicit administrator grant for CMake configure/build dependency downloads.
	bool allow_build_network = false;
	// Effective only when the local browser runtime is available.
	bool allow_browser_debug = true;
	std::string language_tools = "go,java";
	// Optional administrator-selected language tool executables. Empty values
	// retain read-only discovery from legacy environment variables and trusted
	// standard locations. Go additionally checks GOROOT and the service PATH.
	std::string node_executable_path;
	std::string python_executable_path;
	std::string javac_executable_path;
	std::string java_executable_path;
	std::string go_executable_path;
	std::string cargo_executable_path;
	std::string make_executable_path;
	std::string swift_executable_path;
	std::string dotnet_executable_path;
	std::string kotlinc_executable_path;
	std::string php_executable_path;
	std::string dmd_executable_path;
	std::string sensitive_paths;
	unsigned long max_active_runs_per_user = 2;
	unsigned long max_output_tokens = 128UL * 1024UL;
	unsigned long quick_mode_output_tokens = 128UL * 1024UL;
	// Maximum time used to establish a provider connection. Keep this separate
	// from the streaming idle timeout: some remote gateways (notably Kimi) can
	// need more than the old hard-coded eight seconds for DNS/TLS/connect setup.
	unsigned long provider_connect_timeout_seconds = 30;
	// Maximum period without provider network progress while waiting for a
	// streaming completion. Reasoning models may legitimately take several
	// minutes before their first SSE event, so this is separate from the much
	// shorter program-sandbox timeout.
	unsigned long provider_stream_timeout_seconds = 600;
	unsigned long provider_effective_output_timeout_seconds = 300;
	unsigned long provider_request_timeout_seconds = 900;
	// Per-mode workspace tool budgets. Standard is twice quick for medium-sized
	// work, and large is twice standard. Administrators can tune each value
	// independently without changing browser code.
	unsigned long quick_mode_tool_calls = 32;
	unsigned long standard_mode_tool_calls = 64;
	unsigned long large_mode_tool_calls = 128;
	// Compatibility mirror for policy files/API clients predating per-mode
	// limits. New code keeps it equal to the largest configured mode.
	unsigned long max_tool_calls_per_run = 128;
	// Stop a run that repeats tools without discovering new facts or changing
	// its durable draft. Keeping this below max_tool_calls_per_run prevents a
	// broken provider loop from consuming the complete account quota.
	unsigned long max_no_progress_tool_calls = 6;
	// User reports of the same unresolved browser display issue before live debugging.
	unsigned long browser_debug_report_threshold = 3;
	// Compact the provider transcript once it reaches this size. Source bodies
	// already live in durable staging, so a compact digest is sufficient here.
	unsigned long tool_context_compaction_kib = 32;
	unsigned long read_chunk_kib = 8;
	unsigned long read_file_limit_kib = 1024;
	sandbox_limits_t sandbox_limits;
};

class ai_admin_policy_store_t {
public:
	explicit ai_admin_policy_store_t(const std::string &upload_root);
	bool load(ai_admin_policy_t &policy, std::string &err) const;
	bool save(const ai_admin_policy_t &policy, std::string &err) const;
	static bool validate(const ai_admin_policy_t &policy, std::string &err);

private:
	std::string upload_root_;
};

// The HTTP permission gate refreshes this process snapshot from disk. Lower
// layers use it without depending on HTTP/action code, so future agents inherit
// the same policy mechanism.
void ai_runtime_policy_set(const ai_admin_policy_t &policy);
ai_admin_policy_t ai_runtime_policy_get();
bool ai_agent_access_allowed(const ai_admin_policy_t &policy, bool admin);
bool ai_language_tool_enabled(const std::string &language);
bool ai_extra_sensitive_path(const std::string &normalized_path);
unsigned long ai_tool_call_limit_for_mode(
    const ai_admin_policy_t &policy, const std::string &mode);

} // namespace ai
} // namespace webcool
