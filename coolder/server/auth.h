#pragma once
#include "stdafx.h"
#include <string>

namespace coolder
{
// Authenticated account identity and password-verification metadata.
struct account_t {
	// Authenticated account name associated with this object.
	std::string username;
	// Identifier used to look up this record.
	std::string id;
	// Whether this account has administrator privileges.
	bool admin = false;
	// Whether this capability is enabled by its configuration.
	bool enabled = true;
	// Per-account salt used when deriving the password verifier.
	std::string salt;
	// Content digest used to detect changes.
	std::string digest;
};

// Initialize account storage and load the server's authentication state.
void accounts_init();
// Resolve the authenticated account associated with the request.
bool current_account(const request_t &, account_t &);
// Dispatch authentication and account-preference HTTP endpoints.
bool auth_route(request_t &, response_t &, const std::string &method);
// Return the workspace directory assigned to the authenticated account.
std::string account_workspace(const account_t &);
// Return the preferences path owned by the named account.
std::string account_preferences(const std::string &username);
}
