#pragma once

#include <string>
#include <map>
#include <functional>
#include <vector>

namespace webcool
{
namespace ai
{

// Declared dependency inputs, build settings and exported artifacts.
struct prebuilt_dependency_t {
	// Root path of the declared dependency or workspace resource.
	std::string root;
	// Declared relative artifact paths exported by the dependency.
	std::vector<std::string> artifacts;
	// build_system: Build system selected for the dependency.
	// build_type: Requested build configuration, such as Release.
	std::string build_system, build_type = "Release";
	// has_cmake_options: Whether the dependency explicitly declares CMake
	// options.
	// has_make_options: Whether the dependency explicitly declares Make
	// options.
	bool has_cmake_options = false, has_make_options = false;
	// make_target: Make target used to build the declared dependency.
	// install_target: Make target used to install the dependency
	// artifacts.
	// prefix_variable: Make variable naming the private installation
	// prefix.
	std::string make_target = "all", install_target = "install",
	            prefix_variable = "PREFIX";
	// Explicit build definitions supplied for the dependency.
	std::map<std::string, std::string> definitions;
};

// Filesystem metadata returned by a bounded workspace directory listing.
struct workspace_entry_t {
	// Always relative to the authenticated user's workspace root.
	std::string path;
	// Whether this workspace entry represents a directory.
	bool directory;
	// Filesystem entry size in bytes.
	long long size;
	// Seconds since Unix epoch from lstat(). Indexing uses size + mtime to
	// identify changed files without reading every source file into memory.
	long long modified_at;
};

// One matching source line returned by a workspace text search.
struct workspace_match_t {
	// Search results deliberately contain one bounded display line, not a file.
	std::string path;
	// One-based source line number.
	unsigned long line;
	// Text content presented or retained by this record.
	std::string text;
};

// Navigation hint extracted from a source declaration or import line.
struct workspace_outline_item_t {
	// A bounded declaration/import line from one text file. This is a navigation
	// hint, not a parser AST; callers still read the exact file before editing.
	unsigned long line;
	// Category used to classify this entry.
	std::string kind;
	// Text content presented or retained by this record.
	std::string text;
};

// Filesystem capability restricted to one authenticated user's storage tree.
// Every operation normalizes the relative input, rejects internal metadata and
// verifies the resolved path again so symlinks cannot escape the user root.
class agent_workspace_t {
public:
#ifndef _WIN32
	// Copy into a fresh private tree through pinned directory descriptors.
	// The callback yields between filesystem operations; no symlinks followed.
	bool copy_build_tree_to(const std::string &source_directory,
	    const agent_workspace_t &destination, size_t &skipped_files,
	    long long &dependency_bytes,
	    const std::function<void()> &checkpoint, std::string &err,
	    const std::vector<std::string> &excluded = {}) const;
#endif
	// Initialize workspace state; disable external roots for a confined project.
	explicit agent_workspace_t(
	    const std::string &user_root, bool allow_external_roots = true);
	// Read and validate the project's declared dependency specifications.
	bool prebuilt_dependencies(const std::string &directory,
	    std::vector<prebuilt_dependency_t> &dependencies,
	    std::string &err) const;
	// Server-provided immutable package mounts. Only read/list/search consult
	// these capabilities; all write operations retain ordinary path isolation.
	void set_readonly_mounts(
	    const std::map<std::string, std::string> &mounts)
	{
		readonly_mounts_ = mounts;
	}

	// List bounded workspace entries and indicate failures through err.
	bool list(const std::string &relative_dir,
	    std::vector<workspace_entry_t> &entries, std::string &err,
	    bool log_missing = true) const;
	// Read a bounded workspace text file and report truncation or
	// rejection.
	bool read(const std::string &relative_file, std::string &content,
	    bool &truncated, std::string &err,
	    bool log_binary_rejection = true) const;
	// Search readable workspace text and report when result limits are
	// reached.
	bool search(const std::string &relative_dir, const std::string &needle,
	    std::vector<workspace_match_t> &matches, bool &truncated,
	    std::string &err) const;
	// Extract bounded declaration and import hints from a source file.
	bool outline(const std::string &relative_file,
	    std::vector<workspace_outline_item_t> &items, bool &truncated,
	    std::string &err) const;
	// Replace source text only if its captured baseline still matches.
	bool replace_text_if_unchanged(const std::string &relative_file,
	    const std::string &expected_sha256, const std::string &content,
	    std::string &err) const;
	// Transaction helpers used by the confirmed multi-file change-set layer.
	// Both operations are compare-and-swap primitives and never overwrite an
	// unexpected filesystem object.
	bool create_text_if_absent(const std::string &relative_file,
	    const std::string &content, std::string &err) const;
	// Marks a trusted, already-created regular text file as executable. This is
	// used by server-side project scaffolding and is not exposed as a model tool.
	bool set_text_executable(
	    const std::string &relative_file, std::string &err) const;
	// Atomically creates or replaces a server-generated text artifact. This is
	// not exposed as a model tool; callers must choose a trusted fixed path.
	bool save_generated_text(const std::string &relative_file,
	    const std::string &content, std::string &err) const;
	// Trusted streaming digest, including binary dependencies; no bytes exposed.
	bool tree_sha256(const std::string &relative_directory,
	    std::string &digest, std::string &err,
	    const std::vector<std::string> &included_paths = {}) const;
	// Compute the digest of a file while reporting read failures.
	bool file_sha256(const std::string &relative_file, std::string &digest,
	    std::string &err) const;
	// Trusted private-draft copying only; never exposed to model tools.
	bool copy_build_dependency(const std::string &source_file,
	    const agent_workspace_t &destination,
	    const std::string &destination_file, std::string &err) const;
	// Delete a file only if its digest still matches the expected
	// version.
	bool delete_text_if_unchanged(const std::string &relative_file,
	    const std::string &expected_sha256, std::string &err) const;
	// Create a workspace directory without replacing an existing entry.
	bool create_directory_if_absent(
	    const std::string &relative_directory, std::string &err) const;
	// Remove a validated workspace directory only when it is empty.
	bool delete_empty_directory(
	    const std::string &relative_directory, std::string &err) const;

	// Lexically normalizes a user/model path. This does not replace the later
	// realpath/lstat containment checks performed by resolve_existing().
	static bool normalize_path(const std::string &input,
	    std::string &normalized, bool allow_empty, std::string &err);
	// External projects keep a stable logical path in project/session metadata,
	// while this protected per-user registry resolves it to the administrator-
	// approved shared or local directory.
	static bool register_project_root(const std::string &user_root,
	    const std::string &logical_path, const std::string &absolute_root,
	    std::string &err);
	// Remove a previously registered external project-root mapping.
	static bool unregister_project_root(const std::string &user_root,
	    const std::string &logical_path, std::string &err);
	// Resolve a logical project to its authorized filesystem root. Disable
	// external roots when the base is a project rather than trusted account state.
	static bool resolve_project_root(const std::string &user_root,
	    const std::string &logical_path, std::string &absolute_root,
	    std::string &err, bool allow_external_roots = true);
	// Shared projects deliberately keep their complete AI state in the shared
	// source tree. Local-disk projects use a private per-user state root; personal
	// projects keep their historical in-project state.
	static bool resolve_project_state_root(const std::string &user_root,
	    const std::string &logical_path, std::string &absolute_root,
	    std::string &err);
	// Returns true for files that must never be exposed to a model. The built-in
	// list protects VCS metadata, environment files, private keys and local
	// databases. Administrators may add relative prefixes with
	// WEBCOOL_AI_SENSITIVE_PATHS (comma separated); built-ins cannot be disabled.
	static bool path_is_sensitive(const std::string &normalized_path);
	// Compute the content digest used to bind edits to a source version.
	static std::string content_sha256(const std::string &content);

private:
	// Resolve an existing workspace entry while enforcing root isolation.
	bool resolve_existing(const std::string &relative,
	    std::string &absolute, std::string &err) const;
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Whether the root contains a trusted account-level external mount registry.
	bool allow_external_roots_;
	// Server-approved dependency mounts consulted by read operations.
	std::map<std::string, std::string> readonly_mounts_;
};

} // namespace ai
} // namespace webcool
