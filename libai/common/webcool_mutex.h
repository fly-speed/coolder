#pragma once

#include <mutex>

#if !defined(WEBCOOL_USE_STD_MUTEX)
#include "fiber/lib_fiber.hpp"
#endif

namespace webcool
{

// Mutex adapter shared by native threads and the ACL-based runtime.
class mutex {
public:
	// Initialize the underlying mutex used by this adapter.
	mutex() = default;
	// Destroy the mutex after all users have released it.
	~mutex() = default;

	// Disallow copying so this object retains exclusive resource
	// ownership.
	mutex(const mutex &) = delete;
	// Disallow copying so this object retains exclusive resource
	// ownership.
	mutex &operator=(const mutex &) = delete;

	// Acquire exclusive ownership of the underlying mutex.
	void lock()
	{
#if defined(WEBCOOL_USE_STD_MUTEX)
		impl_.lock();
#else
		(void)impl_.lock();
#endif
	}

	// Attempt to acquire the mutex without waiting.
	bool try_lock()
	{
#if defined(WEBCOOL_USE_STD_MUTEX)
		return impl_.try_lock();
#else
		return impl_.trylock();
#endif
	}

	// Release exclusive ownership of the underlying mutex.
	void unlock()
	{
#if defined(WEBCOOL_USE_STD_MUTEX)
		impl_.unlock();
#else
		(void)impl_.unlock();
#endif
	}

private:
#if defined(WEBCOOL_USE_STD_MUTEX)
	// Private implementation state hidden from interface consumers.
	std::mutex impl_;
#else
	// Private implementation state hidden from interface consumers.
	acl::fiber_mutex impl_;
#endif
};

} // namespace webcool
