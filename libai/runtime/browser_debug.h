#pragma once
#include <string>
#include <memory>
#include <vector>
namespace libai {class execution_store_t;}
namespace acl {class json_node;}
namespace webcool
{
namespace ai
{
// Trusted host receipt diagnostic; never register as a model tool.
bool browser_debug_inspect(const std::string &owner, const std::string &key,
    acl::json_node &out, std::string &error);
// Record a trusted host reconciliation under the execution lease and revision.
bool browser_debug_confirm(const std::string &owner, const std::string &arguments,
    acl::json_node &out, std::string &error);
// Keep child receipt leases held until the parent checkpoint confirmation commits.
bool browser_debug_lock_parent_receipts(const std::string &owner, const std::string &project,
    const std::string &key, const std::string &run, const std::string &operation, const std::string &outcome,
    std::vector<std::shared_ptr<libai::execution_store_t>> &leases, std::string &error);
// Process-local capability broker. Tokens never enter model context or logs.
std::string browser_debug_create(
    const std::string &owner, const std::string &project, bool images);
// Return the user's project-scoped browser connection status.
std::string browser_debug_status(
    const std::string &owner, const std::string &project, bool revoke = false);
// Process a browser bridge message and return its protocol response.
std::string browser_debug_exchange(const std::string &body);
// Run an authorized browser observation or interaction for the selected run.
std::string browser_debug_tool(const std::string &owner,
    const std::string &project, const std::string &run, const std::string &name,
    const std::string &selector, const std::string &content,
    const std::string &idempotency_key = "");
// Non-secret session identity, only for a live session available to this run.
std::string browser_debug_evidence_id(const std::string &owner,
    const std::string &project, const std::string &run);
// Take the next retained screenshot from the owning browser-debug run.
std::string browser_debug_take_image(const std::string &owner,
    const std::string &project, const std::string &run);
}
}
