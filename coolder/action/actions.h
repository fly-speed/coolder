#pragma once
#include "stdafx.h"
#include <string>

namespace action
{

// HTTP handler for admin AI policy.
class AdminAiPolicyAction {
public:
	// Handle the HTTP request and write the corresponding JSON response.
	static bool run(request_t &, response_t &, const std::string &);
};

// Return the upload root configured for this server runtime.
std::string runtime_upload_dir_get();

// Resolve the authenticated username and administrator role from the request.
bool auth_current_user(
    const request_t &, const std::string &, std::string &, bool &);

// Resolve the request's authorized per-user data directory.
bool authenticated_user_upload_dir(
    const request_t &, const std::string &, std::string &, std::string &);

// Resolve the administrator data directory for the configured installation.
bool auth_administrator_upload_dir(
    const std::string &, std::string &, std::string &, std::string &);

// Send the authentication-required response for an unauthenticated request.
bool auth_send_required(const request_t &, response_t &);

// Check whether policy permits local-disk project access for this account.
bool local_disk_access_allowed(const std::string &, bool, std::string &);

// Check a local project path against its ownership and access binding.
bool local_dir_lock_path_allows(const std::string &, const std::string &,
    const std::string &, bool &, std::string &, std::string &);

// Per-user provider selections retained across browser sessions.
struct user_prefs_t {
	// Interface language captured for this request or run.
	std::string ui_language = "zh";
	// Preferred provider for coding-agent runs.
	std::string ai_coding_provider_id;
	// Preferred provider for document operations.
	std::string document_ai_provider_id;
	// Preferred provider for assistant conversations.
	std::string ai_assistant_provider_id;
};

// Return the default per-user provider preferences.
user_prefs_t default_user_prefs();

// Load provider preferences for the identified user.
bool load_user_prefs(
    const std::string &, const std::string &, user_prefs_t &, std::string &);

// Persist validated provider preferences for the identified user.
bool save_user_prefs(const std::string &, const std::string &,
    const user_prefs_t &, std::string &);
}
