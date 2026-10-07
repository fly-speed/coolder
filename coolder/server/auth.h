#pragma once
#include "stdafx.h"
#include <string>

namespace coolder {
struct account_t {
	std::string username;
	std::string id;
	bool admin = false;
	bool enabled = true;
	std::string salt;
	std::string digest;
};

void accounts_init();
bool current_account(const request_t&, account_t&);
bool auth_route(request_t&, response_t&, const std::string& method);
std::string account_workspace(const account_t&);
std::string account_preferences(const std::string& username);
}
