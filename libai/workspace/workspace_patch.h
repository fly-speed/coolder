#pragma once

#include <string>

namespace webcool
{
namespace ai
{

// Patch preview tied to the original and proposed content digests.
struct workspace_patch_preview_t {
	// Random one-time plan ID; hashes bind the preview to both file versions.
	std::string id;
	// Path of the file or resource associated with this record.
	std::string path;
	// SHA-256 digest captured before applying the change.
	std::string original_sha256;
	// SHA-256 digest of the proposed replacement content.
	std::string proposed_sha256;
	// Number of source lines added by the proposed edit.
	long long added_lines;
	// Number of source lines removed by the proposed edit.
	long long removed_lines;
	// Bounded textual diff used for review.
	std::string diff;
};

// Summary of a confirmed patch application.
struct workspace_patch_result_t {
	// Identifier used to look up this record.
	std::string id;
	// Path of the file or resource associated with this record.
	std::string path;
	// SHA-256 digest of the associated content.
	std::string sha256;
};

// Per-user compare-and-swap file replacement. Model proposals cannot call this
// directly: the browser first creates a diff preview, then confirms its ID.
class workspace_patch_store_t {
public:
	// Bind the workspace patch store to the supplied storage scope.
	explicit workspace_patch_store_t(const std::string &user_root);

	// Create a new persistent record; report failures through err.
	bool create(const std::string &relative_file,
	    const std::string &proposed_content,
	    workspace_patch_preview_t &preview, std::string &err) const;
	// Apply the identified, previously reviewed filesystem preview.
	bool apply(const std::string &patch_id,
	    workspace_patch_result_t &result, std::string &err) const;

private:
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
