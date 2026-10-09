#include "stdafx.h"
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "provider_request_scheduler.h"

#include "../common/webcool_mutex.h"

#include <acl_cpp/lib_acl.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>

namespace webcool
{
namespace ai
{
namespace
{

struct provider_queue_state_t {
	size_t in_flight = 0;
	size_t learned_limit = 0;
	unsigned int successful_since_limit = 0;
	long long cooldown_until_ms = 0;
	unsigned long long next_ticket = 1;
	std::deque<unsigned long long> waiters;
};

webcool::mutex g_provider_scheduler_mutex;
std::map<std::string, provider_queue_state_t> g_provider_queues;

long long monotonic_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

void erase_waiter(provider_queue_state_t &state, unsigned long long ticket)
{
	for (std::deque<unsigned long long>::iterator it =
		     state.waiters.begin();
	     it != state.waiters.end(); ++it) {
		if (*it != ticket)
			continue;
		state.waiters.erase(it);
		return;
	}
}

} // namespace

bool provider_request_scheduler_t::acquire(const std::string &opaque_key,
					   provider_request_cancel_fn cancelled,
					   void *cancel_context,
					   provider_request_permit_t &permit,
					   std::string &err)
{
	permit = provider_request_permit_t();
	if (opaque_key.empty()) {
		err = "provider scheduler key is empty";
		return false;
	}
	unsigned long long ticket = 0;
	{
		std::lock_guard<webcool::mutex> guard(
			g_provider_scheduler_mutex);
		provider_queue_state_t &state = g_provider_queues[opaque_key];
		ticket = state.next_ticket++;
		state.waiters.push_back(ticket);
	}

	for (;;) {
		if (cancelled != NULL && cancelled(cancel_context)) {
			std::lock_guard<webcool::mutex> guard(
				g_provider_scheduler_mutex);
			std::map<std::string, provider_queue_state_t>::iterator
				found = g_provider_queues.find(opaque_key);
			if (found != g_provider_queues.end())
				erase_waiter(found->second, ticket);
			err = "AI provider scheduling wait cancelled";
			return false;
		}
		unsigned long wait_ms = 25;
		{
			std::lock_guard<webcool::mutex> guard(
				g_provider_scheduler_mutex);
			provider_queue_state_t &state =
				g_provider_queues[opaque_key];
			const long long now = monotonic_ms();
			const bool first = !state.waiters.empty() &&
					   state.waiters.front() == ticket;
			const bool below_limit =
				state.learned_limit == 0 ||
				state.in_flight < state.learned_limit;
			if (first && below_limit &&
			    now >= state.cooldown_until_ms) {
				state.waiters.pop_front();
				++state.in_flight;
				permit.key = opaque_key;
				permit.ticket = ticket;
				permit.active = true;
				return true;
			}
			if (state.cooldown_until_ms > now) {
				wait_ms = static_cast<unsigned long>(
					std::min<long long>(
						250,
						state.cooldown_until_ms - now));
			}
		}
		// Provider calls execute in ACL fibers. A short cooperative delay keeps
		// unrelated users and HTTP requests runnable while this account cools down.
		acl::fiber::delay(std::max(1UL, wait_ms));
	}
}

void provider_request_scheduler_t::finish(provider_request_permit_t &permit,
					  int http_status, bool retryable,
					  unsigned long retry_after_seconds)
{
	if (!permit.active || permit.key.empty())
		return;
	std::lock_guard<webcool::mutex> guard(g_provider_scheduler_mutex);
	provider_queue_state_t &state = g_provider_queues[permit.key];
	const size_t observed_in_flight = state.in_flight;
	if (state.in_flight > 0)
		--state.in_flight;
	if (http_status == 429 && retryable) {
		// A real provider response is the only event that creates a limit. Reduce
		// concurrent pressure by one (never below one) and share Retry-After with
		// every waiting user of the same account.
		state.learned_limit =
			observed_in_flight > 1 ? observed_in_flight - 1 : 1;
		state.successful_since_limit = 0;
		const unsigned long bounded_seconds =
			std::max(1UL, std::min(60UL, retry_after_seconds));
		state.cooldown_until_ms = std::max(
			state.cooldown_until_ms,
			monotonic_ms() +
				static_cast<long long>(bounded_seconds) * 1000);
	} else if (http_status >= 200 && http_status < 300 &&
		   state.learned_limit > 0) {
		// Recover cautiously after sustained success. Four successful windows
		// remove the learned cap entirely; a later 429 will immediately relearn it.
		++state.successful_since_limit;
		if (state.successful_since_limit >= 8) {
			state.successful_since_limit = 0;
			++state.learned_limit;
			if (state.learned_limit >= 5)
				state.learned_limit = 0;
		}
	}
	permit.active = false;
	permit.key.clear();
}

provider_request_scheduler_snapshot_t
provider_request_scheduler_t::inspect(const std::string &opaque_key)
{
	provider_request_scheduler_snapshot_t snapshot;
	std::lock_guard<webcool::mutex> guard(g_provider_scheduler_mutex);
	const std::map<std::string, provider_queue_state_t>::const_iterator
		found = g_provider_queues.find(opaque_key);
	if (found == g_provider_queues.end())
		return snapshot;
	snapshot.in_flight = found->second.in_flight;
	snapshot.waiting = found->second.waiters.size();
	snapshot.learned_concurrency_limit = found->second.learned_limit;
	snapshot.cooldown_remaining_ms = std::max<long long>(
		0, found->second.cooldown_until_ms - monotonic_ms());
	return snapshot;
}

void provider_request_scheduler_t::reset_for_tests()
{
	std::lock_guard<webcool::mutex> guard(g_provider_scheduler_mutex);
	g_provider_queues.clear();
}

} // namespace ai
} // namespace webcool
