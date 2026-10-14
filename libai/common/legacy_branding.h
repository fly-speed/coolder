#pragma once
#include <string>
#include <cstdlib>
namespace libai { namespace ai { namespace legacy_branding {
// Compatibility is restricted to protocol labels and reserved path components.
inline std::string old_label(const std::string &label) { return label.compare(0,6,"LIBAI_")==0 ? "WEBCOOL_"+label.substr(6) : label; }
inline std::string canonical_label(const std::string &label) {
    return label.compare(0,8,"WEBCOOL_")==0 ? "LIBAI_"+label.substr(8) : label;
}
inline std::string canonical_record(const std::string &text) {
    return text.compare(0,8,"WEBCOOL_")==0 ? "LIBAI_"+text.substr(8) : text;
}
inline const char *environment(const char *name) {
    const char *value=std::getenv(name);
    if(value || std::string(name).compare(0,6,"LIBAI_")!=0)return value;
    return std::getenv(("WEBCOOL_"+std::string(name).substr(6)).c_str());
}
inline std::string old_identifier(const std::string &name) {return name.compare(0,6,"libai_")==0 ? "webcool_"+name.substr(6):name;}
inline std::string old_component(const std::string &name) {
    if(name==".libai-build")return ".webcool-build";
    if(name==".libai_agent")return ".webcool_agent";
    if(name==".libai_settings")return ".webcool_settings";
    if(name=="libai_auth")return "webcool_auth";
    if(name=="libai_users")return "webcool_users";
    if(name==".libai-http-service")return ".webcool-http-service";
    if(name==".libai-http-port-env")return ".webcool-http-port-env";
    if(name==".libai-dependencies.json")return ".webcool-dependencies.json";
    return name;
}
inline std::string new_component(const std::string &name) {
    for(const char *next:{".libai-build",".libai_agent",".libai_settings","libai_auth","libai_users",".libai-http-service",".libai-http-port-env",".libai-dependencies.json"})
        if(old_component(next)==name)return next;
    return name;
}
inline std::string canonical_path(const std::string &path) {
    std::string out;std::size_t begin=0;
    while(begin<path.size()) {const auto end=path.find_first_of("/\\",begin);
        const auto part=path.substr(begin,end==std::string::npos?end:end-begin);
        const bool plain=(part=="webcool_auth"||part=="webcool_users");
        out+=(plain && begin!=0) ? part : new_component(part);
        if(end==std::string::npos)break;out+=path[end];begin=end+1;
    }return out;
}
}}}
