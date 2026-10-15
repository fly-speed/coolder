#pragma once
#include "../common/json_value.h"
#include "../validation/repair_scope.h"

namespace libai { namespace ai {
// Reuse only source-determined compiler failures. Timeouts, cancellation,
// broker/policy failures and failed tests can change without a source edit.
inline bool deterministic_compile_failure(const std::string &wire)
{
    using namespace json_value;
    acl::json json(wire.c_str());
    if (!json.finish() || text_boolean(json["validation_passed"], true)) return false;
    auto *commands = array_value(json["commands"]);
    bool diagnosed = false;
    for (auto *entry = commands ? commands->first_child() : nullptr; entry; entry = commands->next_child()) {
        auto *item = entry->is_object() ? entry : entry->get_obj();
        if (!item || text_boolean((*item)["passed"], true)) continue;
        if (!boolean((*item)["started"]) || boolean((*item)["timed_out"]) ||
            boolean((*item)["cancelled"]) || !nullable_text((*item)["error"]).empty()) return false;
        if (!compile_command(nullable_text((*item)["command_id"]))) return false;
        auto *diagnostics = array_value((*item)["diagnostics"]);
        for (auto *d = diagnostics ? diagnostics->first_child() : nullptr; d; d = diagnostics->next_child()) {
            auto *o = d->is_object() ? d : d->get_obj();
            if (o && !nullable_text((*o)["path"]).empty() && number((*o)["line"]) > 0) diagnosed = true;
        }
        if (nullable_text((*item)["diagnostic"]).find("no Go files in ") != std::string::npos) diagnosed = true;
    }
    return diagnosed;
}
} }
