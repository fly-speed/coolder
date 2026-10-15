#pragma once
#include <algorithm>
#include <string>

namespace libai {
namespace ai {

struct coding_turn_budget_t {
    std::string effort;
    long long output_tokens;
    std::string policy;
    std::string thinking;
};

// Explicit choices and providers without an effort control remain authoritative.
// Deliberate work retains the source-output ceiling. Only evidenced persistent
// behavior failures request high effort; task size alone is not such evidence.
inline coding_turn_budget_t coding_turn_budget(const std::string &effort,
    const std::string &thinking, long long output_tokens, bool supports_effort,
    bool coding, bool deliberate, bool deep = false)
{
    coding_turn_budget_t budget{effort, output_tokens, "configured", thinking};
    // This internal choice is resolved before provider serialization. It
    // overrides the manual effort and toggle without changing their defaults.
    if (thinking == "adaptive") {
        budget.thinking = !coding || deliberate ? "enabled" : "disabled";
        budget.effort = supports_effort ?
            (budget.thinking == "disabled" ? "none" : deep ? "high" : "low") : "";
        budget.policy = budget.thinking == "disabled" ?
            "adaptive_thinking_off" : "adaptive_thinking_on";
        if (coding && !deliberate) budget.output_tokens = std::min(output_tokens, 65536LL);
        return budget;
    }
    if (thinking == "disabled") {
        budget.effort = supports_effort ? "none" : "";
        return budget;
    }
    if (!coding || !supports_effort || !effort.empty())
        return budget;
    budget.effort = deep ? "high" : "low";
    budget.policy = deep ? "evidenced_deep_repair" : "enabled_low";
    if (!deliberate) budget.output_tokens = std::min(output_tokens, 65536LL);
    return budget;
}

}
}
