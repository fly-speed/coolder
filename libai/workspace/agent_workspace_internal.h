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

namespace webcool { namespace ai { namespace workspace_detail {

// These limits bound disk traversal, memory consumption and the amount of
// untrusted project data that can be returned to a model in one tool call.
const size_t kMaxReadBytes = 1024 * 1024;
const size_t kMaxListEntries = 2000;
const size_t kMaxSearchMatches = 200;
const size_t kMaxSearchFiles = 10000;
const int kMaxSearchDepth = 24;
const size_t kMaxOutlineItems = 400;

using ::webcool::ai::file_ops::join_path;
using ::webcool::ai::file_ops::replace_file;

enum path_component_state_t {
	PATH_COMPONENT_NORMAL,
	PATH_COMPONENT_LINK,
	PATH_COMPONENT_MISSING,
	PATH_COMPONENT_ERROR
};

std::string ascii_lower(const std::string& value);
bool has_path_component(const std::string& path, const std::string& wanted);
bool path_has_prefix(const std::string& path, const std::string& prefix);
bool has_suffix(const std::string& value, const char* suffix);
bool path_is_within(const std::string& root, const std::string& candidate);
path_component_state_t inspect_path_component(const std::string& path);
bool is_link_like(const std::string& path);
path_component_state_t inspect_path_components(const std::string& root,
	const std::string& relative);
bool has_binary_bytes(const std::string& content);
bool install_new_file(const std::string& temporary, const std::string& target);
bool write_temporary_text(const std::string& target, const std::string& content,
	std::string& temporary, std::string& err);
bool read_limited(const std::string& path, std::string& content,
	bool& truncated, std::string& err);
std::string random_hex_id();
bool dependency_contains(const std::string& parent, const std::string& path);
bool dependency_selected(const std::vector<prebuilt_dependency_t>& deps, const std::string& path);

// Bound uninterrupted filesystem work. A running fiber must yield to its
// scheduler; a worker without fibers sleeps instead of busy-waiting.
class fingerprint_budget_t {
    std::chrono::steady_clock::time_point slice_ = std::chrono::steady_clock::now();
public:
    void checkpoint() {
        if (std::chrono::steady_clock::now() - slice_ < std::chrono::milliseconds(5)) return;
        if (acl_fiber_running()) acl::fiber::delay(5);
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        slice_ = std::chrono::steady_clock::now();
    }
};

#ifndef _WIN32
std::string fingerprint_stat(const struct stat& value);
struct fingerprint_fd_t {
    int fd;
    explicit fingerprint_fd_t(int value) : fd(value) {}
    ~fingerprint_fd_t() { if (fd >= 0) close(fd); }
    fingerprint_fd_t(const fingerprint_fd_t&) = delete;
};
struct fingerprint_record_t {
    std::string digest;
    bool binary_prefix = false;
};

extern thread_local std::map<std::string, fingerprint_record_t> fingerprint_cache;
#endif

} } }
