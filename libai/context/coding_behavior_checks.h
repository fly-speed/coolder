#pragma once
#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace libai { namespace ai {

// Test-design hints, not trusted coverage evidence. Only validators can report
// that a generated test ran. Never add these as new user requirements.
inline std::string coding_behavior_checks(std::string request)
{
    std::transform(request.begin(), request.end(), request.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto mentions = [&](std::initializer_list<const char *> terms) {
        for (auto term : terms) if (request.find(term) != std::string::npos) return true;
        return false;
    };
    const bool shared = mentions({"多人", "联机", "multiplayer", "multi-user", "multiple players"});
    const bool timed = mentions({"游戏", "贪吃蛇", "game", "animation", "simulation"});
    std::string cases;
    if (shared)
        cases += "\nshared_lifecycle: two participants join the SAME shared session and remain valid through its first update; "
            "assert distinct valid initial state where overlap is invalid. Drive updates with one and two connections and "
            "assert that connection count does not multiply the shared clock. Include disconnect/rejoin if requested.";
    if (shared && mentions({"房间", "room"}))
        cases += "\nroom_isolation: exercise joining/switching rooms through the real application/adapter path; "
            "assert state isolation and the intended old-room membership after switching.";
    if (timed && mentions({"速度", "speed", "频率"}))
        cases += "\nobservable_speed: use an injected clock or deterministic elapsed-time driver at the actual scheduler boundary; "
            "over equal elapsed time assert slow/medium/fast produce different movement counts. "
            "Comparing constants or stored speed labels alone does not test this requirement.";
    if (mentions({"暂停", "pause", "resume"}))
        cases += "\ntoggle_resume: drive the requested input through its handler into state changes; "
            "pause stops progress, resume preserves position/score rather than restarting. "
            "Where restart-after-failure is requested, test that separate transition and repeated input/state updates.";
    if (cases.empty()) return "";
    return "\n<behavior_acceptance_cases>Use only the scenarios applicable to the user's request. "
        "Include observable behavioral tests with the FIRST complete implementation, using the native test runner; "
        "do not postpone them until a passing build. Test input -> application/scheduler -> observable state, "
        "not only constructors, helper constants, control existence, or source text. Keep clocks deterministic, "
        "avoid sleeps and respect command_network. Use an authorized functional acceptance runner for adapter behavior "
        "when available. Backend or simulated DOM tests remain distinct from real browser verification." + cases +
        "\nThese cases guide implementation; they are not proof that coverage exists or that any test passed.</behavior_acceptance_cases>\n";
}
} }
