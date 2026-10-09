#pragma once
#include "../common/file_ops.h"
#include "../common/record_codec.h"
#include "agent_workspace.h"
#include "fiber/fiber_base.h"
#include "../common/ai_error_log.h"
#include "../agent/ai_admin_policy.h"
#include <mutex>

#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <fstream>
#include <map>
#include <chrono>
#include <thread>

namespace webcool
{
namespace ai
{
namespace workspace_detail
{

// These limits bound disk traversal, memory consumption and the amount of
// untrusted project data that can be returned to a model in one tool call.
const size_t kMaxReadBytes = 1024 * 1024;
// Upper bound for list entries.
const size_t kMaxListEntries = 2000;
// Upper bound for search matches.
const size_t kMaxSearchMatches = 200;
// Upper bound for search files.
const size_t kMaxSearchFiles = 10000;
// Upper bound for search depth.
const int kMaxSearchDepth = 24;
// Upper bound for outline items.
const size_t kMaxOutlineItems = 400;

using ::webcool::ai::file_ops::join_path;
using ::webcool::ai::file_ops::replace_file;

enum path_component_state_t {
	PATH_COMPONENT_NORMAL,
	PATH_COMPONENT_LINK,
	PATH_COMPONENT_MISSING,
	PATH_COMPONENT_ERROR
};

// Lowercase ASCII letters without locale-dependent conversions.
std::string ascii_lower(const std::string &value);
// Test for an exact component within a normalized path.
bool has_path_component(const std::string &path, const std::string &wanted);
// Check a prefix on a path-component boundary.
bool path_has_prefix(const std::string &path, const std::string &prefix);
// Test whether the text ends in the requested suffix.
bool has_suffix(const std::string &value, const char *suffix);
// Check whether a path lies inside the specified filesystem root.
bool path_is_within(const std::string &root, const std::string &candidate);
// Classify one filesystem path component without treating links as
// directories.
path_component_state_t inspect_path_component(const std::string &path);
// Detect symlinks or platform equivalents that require rejection.
bool is_link_like(const std::string &path);
// Check each path component while resolving a workspace-relative path.
path_component_state_t inspect_path_components(
    const std::string &root, const std::string &relative);
// Detect bytes that make a file unsuitable for text editing.
bool has_binary_bytes(const std::string &content);
// Install a prepared file only when the destination is still absent.
bool install_new_file(const std::string &temporary, const std::string &target);
// Write replacement content to a temporary file before installation.
bool write_temporary_text(const std::string &target, const std::string &content,
    std::string &temporary, std::string &err);
// Read a bounded text file and report truncation or binary rejection.
bool read_limited(const std::string &path, std::string &content,
    bool &truncated, std::string &err);
// Generate an unpredictable hexadecimal identifier for stored artifacts.
std::string random_hex_id();
// Test whether one declared dependency path contains another.
bool dependency_contains(const std::string &parent, const std::string &path);
// Check whether traversal should include a declared dependency path.
bool dependency_selected(
    const std::vector<prebuilt_dependency_t> &deps, const std::string &path);

// Bound uninterrupted filesystem work. A running fiber must yield to its
// scheduler; a worker without fibers sleeps instead of busy-waiting.
class fingerprint_budget_t {
	// Start of the current cooperative scheduling time slice.
	std::chrono::steady_clock::time_point slice_ =
	    std::chrono::steady_clock::now();

public:
	// Account for bounded filesystem work and yield when the time slice
	// expires.
	void checkpoint()
	{
		if (std::chrono::steady_clock::now() - slice_ <
		    std::chrono::milliseconds(5))
			return;
		if (acl_fiber_running())
			acl::fiber::delay(5);
		else
			std::this_thread::sleep_for(
			    std::chrono::milliseconds(5));
		slice_ = std::chrono::steady_clock::now();
	}
};

#ifndef _WIN32
// Encode filesystem metadata used by the digest cache.
std::string fingerprint_stat(const struct stat &value);
// Exclusive descriptor owner used during safe filesystem traversal.
struct fingerprint_fd_t {
	// Owned file descriptor; a negative value means no file is open.
	int fd;
	// Initialize fingerprint fd state from the supplied arguments.
	explicit fingerprint_fd_t(int value)
	        : fd(value)
	{
	}
	// Close the owned descriptor if it is still open.
	~fingerprint_fd_t()
	{
		if (fd >= 0)
			close(fd);
	}
	// Disallow copying so this object retains exclusive resource
	// ownership.
	fingerprint_fd_t(const fingerprint_fd_t &) = delete;
};
// Cached file metadata and digest used to avoid unchanged-file reads.
struct fingerprint_record_t {
	// Content digest used to detect changes.
	std::string digest;
	// Whether the inspected prefix contains bytes classified as binary.
	bool binary_prefix = false;
};

// Thread-local cache of validated filesystem metadata and content digests.
extern thread_local std::map<std::string, fingerprint_record_t>
    fingerprint_cache;
#endif

}
}
}
