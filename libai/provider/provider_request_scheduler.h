#pragma once

#include <cstddef>
#include <string>

namespace webcool
{
namespace ai
{

// The scheduler never receives an API key. Callers pass an opaque account
// fingerprint so requests sharing one provider account coordinate without
// exposing credentials in memory snapshots, diagnostics or logs.
struct provider_request_permit_t {
	// Lookup key for the associated resource or record.
	std::string key;
	// Queue ticket used to preserve request ordering.
	unsigned long long ticket = 0;
	// Whether this object currently holds an active permit.
	bool active = false;
};

// Read-only view of provider admission and cooldown state.
struct provider_request_scheduler_snapshot_t {
	// Requests currently holding an execution permit.
	size_t in_flight = 0;
	// Requests queued for an execution permit.
	size_t waiting = 0;
	// Zero means no learned limit. WebCool deliberately does not invent an RPM
	// or concurrency cap when a provider (notably Kimi) reports none.
	size_t learned_concurrency_limit = 0;
	// Remaining provider cooldown time in milliseconds.
	long long cooldown_remaining_ms = 0;
};

typedef bool (*provider_request_cancel_fn)(void *context);

// Process-wide provider-account coordinator. It is independent from user and
// project admission control: unrelated users still run concurrently until the
// provider itself proves that a shared account needs back-pressure.
class provider_request_scheduler_t {
public:
	// Wait cooperatively for a provider permit, stopping when
	// cancellation is requested.
	static bool acquire(const std::string &opaque_key,
	    provider_request_cancel_fn cancelled, void *cancel_context,
	    provider_request_permit_t &permit, std::string &err);
	// Release the permit and update learned concurrency or cooldown from
	// the outcome.
	static void finish(provider_request_permit_t &permit, int http_status,
	    bool retryable, unsigned long retry_after_seconds);
	// Return a synchronized snapshot of this provider queue and its
	// cooldown.
	static provider_request_scheduler_snapshot_t inspect(
	    const std::string &opaque_key);

	// Test-only reset; production code has no reason to discard learned
	// back-pressure while the server process remains alive.
	static void reset_for_tests();
};

} // namespace ai
} // namespace webcool
