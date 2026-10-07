#pragma once

#include <string>

namespace webcool {
namespace ai {

// Append-only project-local archive of the exact JSON bodies sent to an AI
// provider. HTTP headers and API keys never enter this store.
class agent_request_store_t {
public:
	agent_request_store_t(const std::string& user_root,
		const std::string& project_path, const std::string& run_id);

	// Chooses the next monotonically increasing filename without overwriting a
	// request retained by an earlier process or restart. relative_path receives
	// the project-relative path of the committed JSON document.
	bool append(const std::string& payload, std::string& relative_path,
		std::string& err) const;
	bool operation_log(std::string& content, bool write, std::string& err) const;

private:
	std::string user_root_;
	std::string project_path_;
	std::string run_id_;
};

} // namespace ai
} // namespace webcool
