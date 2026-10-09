#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// One requested filesystem operation in a reviewed change set.
struct workspace_change_input_t {
	// write (default), delete, move, mkdir, or replace_empty_file_with_directory.
	// The replacement operation only converts a reviewed zero-byte placeholder
	// into a directory, keeping this special repair deterministic and reversible.
	std::string operation;
	// Workspace-relative path addressed by this operation.
	std::string path;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Text payload associated with this operation.
	std::string content;
	// Explanation supplied for this change or decision.
	std::string reason;
	// Optional compare condition used by post-generation line review. It prevents
	// a review decision from overwriting edits made after the AI transaction.
	bool enforce_expected_current = false;
	// Whether rollback expects the target path to be absent.
	bool expected_current_absent = false;
	// Content that must still be present before rollback.
	std::string expected_current_content;
};

// Validated operation and display metadata shown before application.
struct workspace_change_preview_item_t {
	// Requested change operation, such as write, delete or move.
	std::string operation;
	// Path of the file or resource associated with this record.
	std::string path;
	// Destination path for a move or related structural change.
	std::string target_path;
	// Explanation supplied for this change or decision.
	std::string reason;
	// SHA-256 digest captured before applying the change.
	std::string original_sha256;
	// SHA-256 digest of the proposed replacement content.
	std::string proposed_sha256;
	// Whether applying the proposal creates a previously absent file.
	bool creates_file = false;
	// Whether this planned operation removes an existing file.
	bool deletes_file = false;
	// Whether applying the proposal creates a directory.
	bool creates_directory = false;
	// Number of source lines added by the proposed edit.
	long long added_lines = 0;
	// Number of source lines removed by the proposed edit.
	long long removed_lines = 0;
	// Bounded textual diff used for review.
	std::string diff;
	// Kept in memory for trusted callers that need a post-commit selective review.
	// It is never stored in the short-lived pending transaction a second time.
	std::string original_content;
};

// Identified, bounded preview of an ordered filesystem transaction.
struct workspace_change_set_preview_t {
	// Identifier used to look up this record.
	std::string id;
	// Ordered items included in this collection or preview.
	std::vector<workspace_change_preview_item_t> items;
};

// Outcome of applying one operation from a change set.
struct workspace_change_result_t {
	// Requested change operation, such as write, delete or move.
	std::string operation;
	// Path of the file or resource associated with this record.
	std::string path;
	// Destination path for a move or related structural change.
	std::string target_path;
	// SHA-256 digest of the associated content.
	std::string sha256;
	// Whether the operation created a new filesystem object.
	bool created = false;
	// Whether the operation removed a filesystem object.
	bool deleted = false;
};

// A short-lived, one-time transaction plan. Plans are isolated by user root and
// change-set ID, so several browsers belonging to the same user can hold review
// previews concurrently. apply() validates the complete snapshot first and
// rolls back earlier writes if a later write fails, so callers never
// intentionally leave a partially applied set.
class workspace_change_set_store_t {
public:
	// Bind the workspace change set store to the supplied storage scope.
	explicit workspace_change_set_store_t(const std::string &user_root);

	// Create a new persistent record; report failures through err.
	bool create(const std::vector<workspace_change_input_t> &changes,
	    workspace_change_set_preview_t &preview, std::string &err) const;
	// Apply the identified, previously reviewed filesystem preview.
	bool apply(const std::string &id,
	    std::vector<workspace_change_result_t> &results,
	    std::string &err) const;
	// Used by the coding runtime after it has durably saved the model result.
	// Preview creation and application share one lock, so no other transaction
	// can interleave filesystem mutations between those two steps.
	bool create_and_apply(
	    const std::vector<workspace_change_input_t> &changes,
	    workspace_change_set_preview_t &preview,
	    std::vector<workspace_change_result_t> &results,
	    std::string &err) const;
	// Recovers an interrupted apply operation from the durable per-user undo
	// journal. create() and apply() invoke this automatically; the public method
	// also lets application startup and maintenance code report recovery errors.
	bool recover(std::string &err) const;

	// Return the validity period of a persisted preview or execution
	// plan.
	static long long lifetime_seconds();

private:
	// Create the record while the caller holds the store mutex.
	bool create_locked(const std::vector<workspace_change_input_t> &changes,
	    workspace_change_set_preview_t &preview, std::string &err) const;
	// Apply a stored change set while the caller holds the change-set
	// mutex.
	bool apply_locked(const std::string &id,
	    std::vector<workspace_change_result_t> &results,
	    std::string &err) const;
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
