#pragma once

#include <mutex>

#if !defined(WEBCOOL_USE_STD_MUTEX)
#include "fiber/lib_fiber.hpp"
#endif

namespace webcool {

class mutex {
public:
	mutex() = default;
	~mutex() = default;

	mutex(const mutex&) = delete;
	mutex& operator=(const mutex&) = delete;

	void lock() {
#if defined(WEBCOOL_USE_STD_MUTEX)
		impl_.lock();
#else
		(void) impl_.lock();
#endif
	}

	bool try_lock() {
#if defined(WEBCOOL_USE_STD_MUTEX)
		return impl_.try_lock();
#else
		return impl_.trylock();
#endif
	}

	void unlock() {
#if defined(WEBCOOL_USE_STD_MUTEX)
		impl_.unlock();
#else
		(void) impl_.unlock();
#endif
	}

private:
#if defined(WEBCOOL_USE_STD_MUTEX)
	std::mutex impl_;
#else
	acl::fiber_mutex impl_;
#endif
};

} // namespace webcool
