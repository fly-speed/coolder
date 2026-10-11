#include "../stdafx.h"
#include "webcool_mutex.h"
#include <fiber/fiber_base.h>

// ACL's errno saver initializes its fiber TLS key through pthread_once.
// Its public fiber-self query reads the key without initializing it first.
extern "C" void fiber_save_errno(int error);

namespace webcool
{
void initialize_native_runtime()
{
	// Preserve the current error and initialize TLS without scheduling other work.
	fiber_save_errno(acl_fiber_last_error());
}
}
