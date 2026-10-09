#pragma once
#include <string>
namespace webcool
{
namespace ai
{
// Exact user requests are authoritative; this ledger is never model-authored.
// Existing session messages bootstrap older sessions. Resume is idempotent.
bool begin_task_contract(const std::string &user_root,
    const std::string &project, const std::string &session_id,
    const std::string &run_id, const std::string &prompt, bool resume,
    std::string &contract, std::string &error);
}
}
