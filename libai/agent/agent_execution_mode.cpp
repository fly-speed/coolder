#include "stdafx.h"
#include "agent_execution_mode.h"

#include <algorithm>

namespace webcool {
namespace ai {

bool build_agent_execution_profile(const std::string& mode,
	size_t policy_tool_calls, size_t policy_no_progress_calls,
	size_t policy_compaction_bytes, long long policy_output_tokens,
	long long quick_mode_output_tokens,
	long long requested_output_tokens, agent_execution_profile_t& profile,
	std::string& err)
{
	profile = agent_execution_profile_t();
	if (mode != "quick" && mode != "standard" && mode != "large") {
		err = "execution_mode must be quick, standard or large";
		return false;
	}
	if (requested_output_tokens < 64
		|| requested_output_tokens > policy_output_tokens)
	{
		err = "max_output_tokens exceeds the administrator policy limit";
		return false;
	}
	if (quick_mode_output_tokens < 256
		|| quick_mode_output_tokens > 256 * 1024)
	{
		err = "quick_mode_output_tokens must be between 256 and 262144";
		return false;
	}
	profile.max_tool_calls = policy_tool_calls;
	profile.max_no_progress_calls = policy_no_progress_calls;
	profile.context_compaction_bytes = policy_compaction_bytes;
	profile.max_output_tokens = requested_output_tokens;
	if (mode == "quick") {
		profile.max_no_progress_calls = std::min<size_t>(
			policy_no_progress_calls, 4);
		// Keep one normal batch of source files available until the model has had a
		// chance to propose a revision. The old 24 KiB cap compacted the first batch
		// immediately and caused Kimi/DeepSeek to reread the same project files.
		profile.context_compaction_bytes = std::min<size_t>(
			policy_compaction_bytes, 48 * 1024);
		profile.max_output_tokens = std::min<long long>(
			requested_output_tokens, quick_mode_output_tokens);
	}
	return true;
}

} // namespace ai
} // namespace webcool
