#pragma once

#include <cstddef>
#include <string>

namespace webcool
{
namespace ai
{

// Immutable tool, context and token budgets for one execution mode.
struct agent_execution_profile_t {
	// Maximum number of tool calls allowed for this run.
	size_t max_tool_calls = 0;
	// Consecutive unproductive calls allowed before intervention.
	size_t max_no_progress_calls = 0;
	// Prompt size in bytes that triggers context compaction.
	size_t context_compaction_bytes = 0;
	// Upper bound on generated tokens for one provider request.
	long long max_output_tokens = 0;
};

// Derives one immutable run profile without ever exceeding administrator
// policy. Mode selection changes cost/iteration behavior, not permissions.
bool build_agent_execution_profile(const std::string &mode,
    size_t policy_tool_calls, size_t policy_no_progress_calls,
    size_t policy_compaction_bytes, long long policy_output_tokens,
    long long quick_mode_output_tokens, long long requested_output_tokens,
    agent_execution_profile_t &profile, std::string &err);

} // namespace ai
} // namespace webcool
