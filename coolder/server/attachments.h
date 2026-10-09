#pragma once
#include "host.h"
#include "auth.h"
namespace coolder
{
// Handle authorized attachment access or removal for the current account.
bool attachment_route(
    request_t &, response_t &, const account_t &, bool discard);
}
