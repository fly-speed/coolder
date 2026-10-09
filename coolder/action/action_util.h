#pragma once
#include "actions.h"

namespace action
{

bool normalize_relative_path(
    const char *, std::string &, std::string &, bool allow_empty = false);

std::string join_upload_path(const std::string &, const std::string &);

const char *shared_folder_name();

bool ensure_shared_upload_dir(std::string &);

bool sendJson(response_t &, int, const acl::json_node &, bool = true);

bool sendJson(response_t &, int, const acl::string &, bool = true);

void json_error_at(
    response_t &, int, const char *, bool, const char *, int, const char *);
}
#define json_error(res, status, msg, keep) \
	action::json_error_at(             \
	    res, status, msg, keep, __FILE__, __LINE__, __FUNCTION__)
