#pragma once
#include "auth.h"

namespace coolder
{
// Handle project membership and bounded collaborative file operations.
bool project_sharing_route(request_t &req, response_t &res,
    const std::string &method, const account_t &actor);
}
