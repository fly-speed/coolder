#pragma once
#include "export.h"
#include <string>
namespace libai
{
namespace cpp
{
// Root-confined JSON coding API shared by all language bindings. Host owns authorization.
class LIBAI_CPP_API CodingService {
public:
	// Bind operations to a trusted, existing application storage/workspace root.
	explicit CodingService(std::string root);
	// Report whether the linked library was built with the desktop coding component.
	static bool available() noexcept;
	// Return {ok,...} JSON; invalid arguments and failed operations return {ok:false,error}.
	// Portable builds throw std::runtime_error; operations are listed by capabilities.
	std::string call(const std::string &operation,
	    const std::string &arguments = "{}") const;

private:
	// Host-selected filesystem scope; model arguments cannot change it.
	std::string root_;
};
}
}
