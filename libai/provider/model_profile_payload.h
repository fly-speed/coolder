#pragma once
#include "ai_provider_store.h"
#include "../sdk/model_profile.h"
namespace webcool { namespace ai { namespace provider_detail {
// Validate every outgoing attempt, including transport and protocol repair retries.
inline bool model_profile_payload(const provider_config_t &provider,
    const std::string &payload,std::string &error) {
    try {
        const auto profile=libai::parse_model_profile(provider.model_profile_json);
        if (payload.size()>profile.max_context_bytes) {
            error="model profile request byte limit exceeded";return false;
        }
        return true;
    } catch (const std::exception &) {
        error="invalid stored model capability profile";return false;
    }
}
}}}
