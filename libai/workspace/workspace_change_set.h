#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct workspace_change_input_t {
	// write (default), delete, move, mkdir, or replace_empty_file_with_directory.
	// The replacement operation only converts a reviewed zero-byte placeholder
	// into a directory, keeping this special repair deterministic and reversible.
	std::string operation;
	std::string path;
	std::string target_path;
	std::string content;
	std::string reason;
	// Optional compare condition used by post-generation line review. It prevents
	// a review decision from overwriting edits made after the AI transaction.
	bool enforce_expected_current = false;
	bool expected_current_absent = false;
	std::string expected_current_content;
};

struct workspace_change_preview_item_t {
	std::string operation;
	std::string path;
	std::string target_path;
	std::string reason;
	std::string original_sha256;
	std::string proposed_sha256;
	bool creates_file = false;
	bool deletes_file = false;
	bool creates_directory = false;
	long long added_lines = 0;
	long long removed_lines = 0;
	std::string diff;
	// Kept in memory for trusted callers that need a post-commit selective review.
	// It is never stored in the short-lived pending transaction a second time.
	std::string original_content;
};

struct workspace_change_set_preview_t {
	std::string id;
	std::vector<workspace_change_preview_item_t> items;
};

struct workspace_change_result_t {
	std::string operation;
	std::string path;
	std::string target_path;
	std::string sha256;
	bool created = false;
	bool deleted = false;
};

// A short-lived, one-time transaction plan. Plans are isolated by user root and
// change-set ID, so several browsers belonging to the same user can hold review
// previews concurrently. apply() validates the complete snapshot first and
// rolls back earlier writes if a later write fails, so callers never
// intentionally leave a partially applied set.
class workspace_change_set_store_t {
public:
	explicit workspace_change_set_store_t(const std::string &user_root);

	bool create(const std::vector<workspace_change_input_t> &changes,
	    workspace_change_set_preview_t &preview, std::string &err) const;
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

	static long long lifetime_seconds();

private:
	bool create_locked(const std::vector<workspace_change_input_t> &changes,
	    workspace_change_set_preview_t &preview, std::string &err) const;
	bool apply_locked(const std::string &id,
	    std::vector<workspace_change_result_t> &results,
	    std::string &err) const;
	std::string user_root_;
};

} // namespace ai
} // namespace webcool
