#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace libai { namespace ai {
inline std::string glm_model_name(std::string model)
{
    std::transform(model.begin(), model.end(), model.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    const auto slash = model.find_last_of('/');
    return slash == std::string::npos ? model : model.substr(slash + 1);
}
inline bool glm_model(const std::string &model)
{
    return glm_model_name(model).compare(0, 4, "glm-") == 0;
}
inline bool glm_effort_model(const std::string &model)
{
    const auto name = glm_model_name(model);
    return name == "glm-5.2" || name.compare(0, 8, "glm-5.2-") == 0 ||
        name == "glm-5.3" || name.compare(0, 8, "glm-5.3-") == 0;
}
// GLM 5.3 always thinks. Adaptive's off/none means its lightest valid
// reasoning level, never an invalid disabled/none request. Earlier GLM models
// use enabled thinking under the host's GLM UI policy as well.
inline void normalize_glm_reasoning(const std::string &model,
    std::string &thinking, std::string &effort)
{
    if (!glm_model(model)) return;
    const bool light = thinking == "disabled" || effort == "none" || effort == "minimal";
    thinking = "enabled";
    if (!glm_effort_model(model)) { effort.clear(); return; }
    if (light) effort = "low";
    if (effort == "xhigh") effort = "max";
    if (effort == "medium") effort = "high";
    // GLM 5.2 maps low to high. Avoid relying on gateway-specific aliases.
    if (glm_model_name(model).compare(0, 7, "glm-5.2") == 0 && effort == "low") effort = "high";
}
} }
