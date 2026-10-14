#pragma once
#include "json_schema.h"
#include "vendor/rapidjson/writer.h"
#include <limits>

namespace libai {
// Host-declared capabilities, independent of vendor/model-name heuristics.
struct model_profile_t {
    bool tool_calling = true;
    bool multiple_tool_calls = true;
    std::size_t max_output_tokens = std::numeric_limits<std::size_t>::max();
    std::size_t max_context_bytes = std::numeric_limits<std::size_t>::max();
    std::string json = "{\"version\":1}";
};
inline model_profile_t parse_model_profile(const std::string &wire) {
    static const json_schema_t schema(R"({"type":"object","required":["version"],"additionalProperties":false,"properties":{"version":{"type":"integer","enum":[1]},"id":{"type":"string","maxLength":128},"tool_calling":{"type":"boolean"},"multiple_tool_calls":{"type":"boolean"},"max_output_tokens":{"type":"integer","minimum":1,"maximum":1073741824},"max_context_bytes":{"type":"integer","minimum":1,"maximum":1073741824}}})");
    if (!schema.validate(wire).valid)
        throw std::invalid_argument("invalid model capability profile");
    governance_detail::json::Document value;
    governance_detail::parse_json(wire,value);
    model_profile_t profile;
    governance_detail::json::StringBuffer buffer;
    governance_detail::json::Writer<governance_detail::json::StringBuffer> writer(buffer);
    value.Accept(writer);
    profile.json.assign(buffer.GetString(), buffer.GetSize());
    if (value.HasMember("tool_calling")) profile.tool_calling=value["tool_calling"].GetBool();
    if (value.HasMember("multiple_tool_calls")) profile.multiple_tool_calls=value["multiple_tool_calls"].GetBool();
    if (value.HasMember("max_output_tokens")) profile.max_output_tokens=value["max_output_tokens"].GetUint64();
    if (value.HasMember("max_context_bytes")) profile.max_context_bytes=value["max_context_bytes"].GetUint64();
    return profile;
}
}
