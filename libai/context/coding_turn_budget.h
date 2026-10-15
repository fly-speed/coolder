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
// Ordinary coding keeps thinking enabled with enough room for a source batch;
// failed checks and large tasks retain the full configured generation ceiling.
inline coding_turn_budget_t coding_turn_budget(const std::string &effort,
    const std::string &thinking, long long output_tokens, bool supports_effort,
    bool coding, bool difficult)
{
    coding_turn_budget_t budget{effort, output_tokens, "configured", thinking};
    // This internal choice is resolved before provider serialization. It
    // overrides the manual effort and toggle without changing their defaults.
    if (thinking == "adaptive") {
        budget.thinking = !coding || difficult ? "enabled" : "disabled";
        budget.effort = supports_effort ?
            (budget.thinking == "disabled" ? "none" : "high") : "";
        budget.policy = budget.thinking == "disabled" ?
            "adaptive_thinking_off" : "adaptive_thinking_on";
        if (coding && !difficult) budget.output_tokens = std::min(output_tokens, 65536LL);
        return budget;
    }
    if (!coding || !supports_effort || !effort.empty() || thinking == "disabled")
        return budget;
    budget.effort = difficult ? "high" : "low";
    budget.policy = difficult ? "adaptive_repair_or_large" : "adaptive_ordinary";
    if (!difficult) budget.output_tokens = std::min(output_tokens, 65536LL);
    return budget;
}

}
}
