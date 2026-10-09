#pragma once
#include "stdafx.h"
#include <string>

namespace coolder
{

// Installation directory holding server settings and account metadata.
extern std::string data_dir;
// Configured root for account workspace storage.
extern std::string workspace_dir;
// Directory containing the served browser assets.
extern std::string html_dir;
// Host and port advertised by the local HTTP listener.
extern std::string authority;

// Route an authenticated HTTP request to its server or AI action handler.
bool dispatch(request_t &, response_t &, const std::string &method);

// Write the HTTP status, content type and response body.
bool reply(response_t &, int, const std::string &,
    const char *type = "application/json; charset=utf-8");
}
