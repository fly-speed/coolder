#pragma once
#include "coding_internal.h"
#include "../sdk/task_contract_json.h"
namespace webcool { namespace ai { namespace coding_detail {
// Portable and desktop paths use exactly the same strict contract decoder.
inline bool task_contract_decoded(const libai::task_contract_t &c,acl::json_node &out)
{
    const bool checks[] = {
        c.loop_finished && c.loop_outcome == "completed",
        !c.require_generation || c.proves(c.generation, "completed", "host"),
        !c.require_build || c.proves(c.build, "passed", "host"),
        !c.require_tests || c.proves(c.tests, "passed", "host"),
        !c.require_review || c.proves(c.review, "accepted", "human")
    };
    const char *check_names[] = {"loop", "generation", "build", "tests", "review"};
    std::vector<std::string> unmet;
    auto &details = out.add_child("checks", true);
    for (unsigned i = 0; i < 5; ++i) {
        details.add_bool(check_names[i], checks[i]);
        if (!checks[i]) unmet.push_back(check_names[i]);
    }
    out.add_number("version", 1).add_bool("accepted", c.accepted());
    strings(out, "unmet", unmet);
    return true;
}
inline bool task_contract(acl::json_node &a,acl::json_node &out)
{
    return task_contract_decoded(libai::parse_task_contract_json(a.to_string().c_str()),out);
}
inline bool task_contract_raw(const std::string &arguments,acl::json_node &out)
{
    return task_contract_decoded(libai::parse_task_contract_json(arguments),out);
}
}}}
