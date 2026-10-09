#pragma once
#include "stdafx.h"
#include <string>

namespace action
{

class AdminAiPolicyAction {
public:
	static bool run(request_t &, response_t &, const std::string &);
};

std::string runtime_upload_dir_get();

bool auth_current_user(
    const request_t &, const std::string &, std::string &, bool &);

bool authenticated_user_upload_dir(
    const request_t &, const std::string &, std::string &, std::string &);

bool auth_administrator_upload_dir(
    const std::string &, std::string &, std::string &, std::string &);

bool auth_send_required(const request_t &, response_t &);

bool local_disk_access_allowed(const std::string &, bool, std::string &);

bool local_dir_lock_path_allows(const std::string &, const std::string &,
    const std::string &, bool &, std::string &, std::string &);

struct user_prefs_t {
	std::string ui_language = "zh";
	std::string ai_coding_provider_id;
	std::string document_ai_provider_id;
	std::string ai_assistant_provider_id;
};

user_prefs_t default_user_prefs();

bool load_user_prefs(
    const std::string &, const std::string &, user_prefs_t &, std::string &);

bool save_user_prefs(const std::string &, const std::string &,
    const user_prefs_t &, std::string &);
}
