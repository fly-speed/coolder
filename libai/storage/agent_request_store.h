#pragma once

#include <string>

namespace webcool
{
namespace ai
{

// Append-only project-local archive of the exact JSON bodies sent to an AI
// provider. HTTP headers and API keys never enter this store.
class agent_request_store_t {
public:
	// Bind the agent request store to the supplied storage scope.
	agent_request_store_t(const std::string &user_root,
	    const std::string &project_path, const std::string &run_id);

	// Chooses the next monotonically increasing filename without overwriting a
	// request retained by an earlier process or restart. relative_path receives
	// the project-relative path of the committed JSON document.
	bool append(const std::string &payload, std::string &relative_path,
	    std::string &err) const;
	// Return the retained operation-log snapshot for this task.
	bool operation_log(
	    std::string &content, bool write, std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Logical path identifying the selected project.
	std::string project_path_;
	// Identifier of the associated agent run.
	std::string run_id_;
};

} // namespace ai
} // namespace webcool
