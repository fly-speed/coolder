#include "bridge_internal.h"
#include <cstdlib>
#include <cstring>
#ifdef LIBAI_CODING_FULL
#include <libai/coding/coding_service.h>
#endif
extern "C" {
int libai_coding_available(void)
{
#ifdef LIBAI_CODING_FULL
	return 1;
#else
	return 0;
#endif
}
char *libai_coding_call(
    const char *root, const char *operation, const char *arguments)
{
	bridge_error("");
#ifdef LIBAI_CODING_FULL
	try {
		if (!root || !operation || !arguments) {
			bridge_error("Coding arguments must be non-null");
			return nullptr;
		}
		std::string result = libai::ai::coding_service_call(
		    root, operation, arguments);
		char *copy =
		    static_cast<char *>(std::malloc(result.size() + 1));
		if (!copy) {
			bridge_error("Allocation failed");
			return nullptr;
		}
		std::memcpy(copy, result.c_str(), result.size() + 1);
		return copy;
	} catch (const std::exception &e) {
		bridge_error(e.what());
	} catch (...) {
		bridge_error("Coding operation failed");
	}
#else
	(void)root;
	(void)operation;
	(void)arguments;
	bridge_error("CodingService requires LIBAI_CODING_FULL=ON");
#endif
	return nullptr;
}
}
