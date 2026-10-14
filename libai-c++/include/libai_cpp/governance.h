#pragma once
#include "export.h"
#include <string>
namespace libai { namespace cpp {
// Independent public result: no vendor parser or original core headers.
struct SchemaValidation {
    bool valid=false;
    std::string code,keyword,instance_pointer,schema_pointer;
};
LIBAI_CPP_API SchemaValidation validate_json_schema(const std::string &schema,
    const std::string &instance);
LIBAI_CPP_API std::string governance_call(const std::string &operation,
    const std::string &arguments="{}");
}}
