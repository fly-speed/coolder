#pragma once
#include "host.h"
#include "auth.h"
namespace coolder
{
bool attachment_route(request_t &, response_t &, const account_t &,
		      bool discard);
}
