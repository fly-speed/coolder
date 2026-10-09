#pragma once

#include "agent_project_store.h"

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// Produces a deterministic, dependency-free starting architecture for a newly
// scaffolded project. It is intentionally a template rather than an AI result:
// users can inspect and version it before any model proposes source changes.
void build_project_plan_template(const std::string &project_path,
				 const std::string &language,
				 const std::string &platform, std::string &goal,
				 std::vector<agent_project_module_t> &modules,
				 std::vector<agent_project_task_t> &tasks);

// Produces an inspectable planning proposal for an existing project. "large"
// adds UI, integration, operations and release gates, while "standard" keeps
// the compact scaffold plan. The caller must explicitly save the proposal;
// this function never changes project metadata or source files.
bool build_project_plan_proposal(const std::string &project_path,
				 const std::string &language,
				 const std::string &platform,
				 const std::string &requested_goal,
				 const std::string &scale, std::string &goal,
				 std::vector<agent_project_module_t> &modules,
				 std::vector<agent_project_task_t> &tasks,
				 std::string &err);

} // namespace ai
} // namespace webcool
