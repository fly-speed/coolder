#pragma once

#include <cstddef>
#include <string>

namespace webcool {
namespace ai {

// The scheduler never receives an API key. Callers pass an opaque account
// fingerprint so requests sharing one provider account coordinate without
// exposing credentials in memory snapshots, diagnostics or logs.
struct provider_request_permit_t {
	std::string key;
	unsigned long long ticket = 0;
	bool active = false;
};

struct provider_request_scheduler_snapshot_t {
	size_t in_flight = 0;
	size_t waiting = 0;
	// Zero means no learned limit. WebCool deliberately does not invent an RPM
	// or concurrency cap when a provider (notably Kimi) reports none.
	size_t learned_concurrency_limit = 0;
	long long cooldown_remaining_ms = 0;
};

typedef bool (*provider_request_cancel_fn)(void* context);

// Process-wide provider-account coordinator. It is independent from user and
// project admission control: unrelated users still run concurrently until the
// provider itself proves that a shared account needs back-pressure.
class provider_request_scheduler_t {
public:
	static bool acquire(const std::string& opaque_key,
		provider_request_cancel_fn cancelled, void* cancel_context,
		provider_request_permit_t& permit, std::string& err);
	static void finish(provider_request_permit_t& permit, int http_status,
		bool retryable, unsigned long retry_after_seconds);
	static provider_request_scheduler_snapshot_t inspect(
		const std::string& opaque_key);

	// Test-only reset; production code has no reason to discard learned
	// back-pressure while the server process remains alive.
	static void reset_for_tests();
};

} // namespace ai
} // namespace webcool
