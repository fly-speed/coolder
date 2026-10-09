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
	// Lookup key for the associated resource or record.
	std::string key;
	// Time the connection was last returned for sequential reuse.
	std::chrono::steady_clock::time_point released;
	// Owned TLS configuration kept alive while the connection is
	// reusable.
	std::unique_ptr<acl::openssl_conf> ssl;
	// Underlying HTTP connection owned by this transport session.
	std::unique_ptr<acl::http_request> connection;
};

}
}
