#include <libai/coding.h>
int main() {
 const auto* agent = webcool::ai::agent_registry_t::instance().find("coding");
 if (!agent || agent->tools.empty()) return 1;
 if (!webcool::ai::coding::reviewable_agent_run_status("completed")) return 2;
 if (webcool::ai::coding::reviewable_agent_run_status("invalid")) return 3;
 return 0;
}
