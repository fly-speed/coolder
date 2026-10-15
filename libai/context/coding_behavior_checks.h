#pragma once
#include <string>

namespace libai { namespace ai {

// General test-design guidance, not a keyword-selected domain checklist or
// trusted coverage evidence. The caller excludes analysis and verification tasks.
inline std::string coding_behavior_checks(const std::string &)
{
    return "\n<behavior_contract>Derive observable checks from the request using real entry points: normal success, "
        "first and repeated updates, failure/recovery. Assert valid state remains valid until a real failure. "
        "For shared participants, join/create at least two before advancing; verify nonconflicting initial resources "
        "and progress for each without pausing one to hide conflicts. Check shared authoritative state and isolation. "
        "For runtime settings, start the actual scheduler, change configuration, then compare downstream updates "
        "over equal elapsed time. A getter/echo or changed position alone cannot verify a running timer. "
        "Check actual serialization, identity and input/state/output boundaries: consumer-visible field names "
        "and real transport inputs, not constants, typed round trips or fallback paths. Compare measured results "
        "explicitly; do not discard them. Prefer supported protocol libraries to hand-written framing. "
        "Use native tests, deterministic fixtures and permitted network capabilities; report unavailable browser "
        "or runtime checks. Apply only relevant checks, with the implementation. These guidelines and source review "
        "signals are not executed evidence or proof of coverage.</behavior_contract>\n";
}
} }
