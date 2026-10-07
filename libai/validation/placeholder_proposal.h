#pragma once
#include <string>
namespace webcool { namespace ai {
inline std::string placeholder_marker(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}
// Deliberately narrow: do not reject fixture text, TODO comments, identifier
// names, string literals, empty files, or general descriptions containing this word.
inline bool explicit_placeholder_proposal(const std::string& path,
    const std::string& content, const std::string& reason) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = placeholder_marker(path.substr(dot));
    if (ext != ".cpp" && ext != ".cc" && ext != ".cxx" && ext != ".c"
        && ext != ".h" && ext != ".hpp" && ext != ".hxx") return false;
    return placeholder_marker(content) == "placeholder"
        || (content.size() <= 512 && placeholder_marker(reason) == "placeholder");
}
} }
