#pragma once
#include "actions.h"

namespace action
{

// Normalize a relative path before resolving it in the workspace.
bool normalize_relative_path(
    const char *, std::string &, std::string &, bool allow_empty = false);

// Return the storage path used for join upload.
std::string join_upload_path(const std::string &, const std::string &);

// Return the configured name of the shared upload directory.
const char *shared_folder_name();

// Ensure that the shared upload directory is available.
bool ensure_shared_upload_dir(std::string &);

// Write a JSON HTTP response with the requested status and keep-alive policy.
bool sendJson(response_t &, int, const acl::json_node &, bool = true);

// Write a JSON HTTP response with the requested status and keep-alive policy.
bool sendJson(response_t &, int, const acl::string &, bool = true);

// Send a JSON error and retain the source location for server diagnostics.
void json_error_at(
    response_t &, int, const char *, bool, const char *, int, const char *);
}
#define json_error(res, status, msg, keep) \
	action::json_error_at(             \
	    res, status, msg, keep, __FILE__, __LINE__, __FUNCTION__)
