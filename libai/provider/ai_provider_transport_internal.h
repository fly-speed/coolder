#pragma once
#include "ai_provider_client_internal.h"
#include <chrono>
#include <memory>
namespace webcool
{
namespace ai
{
// Owned by one sequential coding run on its worker. No cross-user/global pool.
class provider_transport_session_t {
public:
	std::string key;
	std::chrono::steady_clock::time_point released;
	std::unique_ptr<acl::openssl_conf> ssl;
	std::unique_ptr<acl::http_request> connection;
};

}
}
