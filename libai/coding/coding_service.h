#pragma once
#include <string>

namespace webcool
{
namespace ai
{
// Invoke a root-confined reusable coding capability using UTF-8 JSON objects.
// Authentication/authorization belong to the embedding host. Unknown operations fail.
std::string coding_service_call(const std::string &root,
    const std::string &operation, const std::string &arguments);
}
}
