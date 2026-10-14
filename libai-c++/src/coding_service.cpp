#include <libai_cpp/coding_service.h>
#include <stdexcept>
#include <utility>
#ifdef LIBAI_CODING_FULL
#include <libai/coding/coding_service.h>
#endif
namespace libai
{
namespace cpp
{
CodingService::CodingService(std::string root)
        : root_(std::move(root))
{
}
bool CodingService::available() noexcept
{
#ifdef LIBAI_CODING_FULL
	return true;
#else
	return false;
#endif
}
std::string CodingService::call(
    const std::string &operation, const std::string &arguments) const
{
#ifdef LIBAI_CODING_FULL
	return libai::ai::coding_service_call(root_, operation, arguments);
#else
	(void)operation;
	(void)arguments;
	throw std::runtime_error("CodingService requires LIBAI_CPP_FULL=ON");
#endif
}
}
}
