#pragma once
#include <string>
namespace webcool
{
namespace ai
{
// Process-local capability broker. Tokens never enter model context or logs.
std::string browser_debug_create(const std::string &owner,
				 const std::string &project, bool images);
std::string browser_debug_status(const std::string &owner,
				 const std::string &project,
				 bool revoke = false);
std::string browser_debug_exchange(const std::string &body);
std::string browser_debug_tool(const std::string &owner,
			       const std::string &project,
			       const std::string &run, const std::string &name,
			       const std::string &selector,
			       const std::string &content);
// Non-secret session identity, only for a live session available to this run.
std::string browser_debug_evidence_id(const std::string &owner,
				      const std::string &project,
				      const std::string &run);
std::string browser_debug_take_image(const std::string &owner,
				     const std::string &project,
				     const std::string &run);
}
}
