#pragma once

#include <string>

namespace webcool
{
namespace ai
{

struct workspace_patch_preview_t {
	// Random one-time plan ID; hashes bind the preview to both file versions.
	std::string id;
	std::string path;
	std::string original_sha256;
	std::string proposed_sha256;
	long long added_lines;
	long long removed_lines;
	std::string diff;
};

struct workspace_patch_result_t {
	std::string id;
	std::string path;
	std::string sha256;
};

// Per-user compare-and-swap file replacement. Model proposals cannot call this
// directly: the browser first creates a diff preview, then confirms its ID.
class workspace_patch_store_t {
public:
	explicit workspace_patch_store_t(const std::string &user_root);

	bool create(const std::string &relative_file,
		    const std::string &proposed_content,
		    workspace_patch_preview_t &preview, std::string &err) const;
	bool apply(const std::string &patch_id,
		   workspace_patch_result_t &result, std::string &err) const;

private:
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
