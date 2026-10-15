#pragma once
#include <string>

namespace libai { namespace ai {

// General test-design guidance, not a keyword-selected domain checklist or
// trusted coverage evidence. The caller excludes analysis and verification tasks.
inline std::string coding_behavior_checks(const std::string &)
{
    return "\n<behavior_contract>Derive observable checks from the request using real entry points: normal success, "
        "first and repeated updates, failure/recovery. Fix one initial-state/action/result contract before implementing its callers and tests; "
        "reuse current source decisions unless the user contract or observed evidence requires a correction. When correcting lifecycle semantics, "
        "update related setup, callers and tests together, including other layers. Assert valid state remains valid until a real failure. "
        "For shared participants, join/create at least two before advancing; verify nonconflicting initial resources "
        "and progress for each without pausing one to hide conflicts. Check shared authoritative state and isolation. "
        "For runtime settings, start the actual scheduler, change configuration, then compare downstream updates "
        "over equal elapsed time. A getter/echo, invocation counter or changed position alone cannot verify a running timer. "
        "For pause/resume or no-op behavior, assert actual downstream state, not a counter incremented before the early return. "
        "Check actual serialization, identity and input/state/output boundaries: consumer-visible field names "
        "and real transport inputs, not constants, typed round trips or fallback paths. Compare measured results "
        "explicitly; do not discard them. Prefer supported protocol libraries to hand-written framing. "
        "Use native tests, deterministic fixtures and permitted network capabilities; report unavailable browser "
        "or runtime checks. Apply only relevant checks, with the implementation. These guidelines and source review "
        "signals are not executed evidence or proof of coverage.</behavior_contract>\n";
}
} }
