#pragma once
#include "stdafx.h"
#include <string>

namespace coolder
{

extern std::string data_dir;
extern std::string workspace_dir;
extern std::string html_dir;
extern std::string authority;

bool dispatch(request_t &, response_t &, const std::string &method);

bool reply(response_t &, int, const std::string &,
	   const char *type = "application/json; charset=utf-8");
}
