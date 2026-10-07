#pragma once
#include <map>
#include <set>
#include <string>
#include <regex>

namespace webcool { namespace ai {
// Only narrow the review when every failed CTest has one unambiguous source
// filename. Custom runners/ambiguous names retain the conservative fallback.
inline std::set<std::string> repair_test_sources(const std::string& diagnostic,
    const std::set<std::string>& sources) {
    std::set<std::string> selected;
    const std::regex failure(R"(Test\s+#\d+:\s+([^\s]+).*\*\*\*(Failed|Timeout|Exception))");
    for (auto i = std::sregex_iterator(diagnostic.begin(), diagnostic.end(), failure);
         i != std::sregex_iterator(); ++i) {
        const std::string name = (*i)[1].str();
        std::set<std::string> matches;
        for (const auto& path : sources) {
            const size_t start = path.find_last_of('/');
            const std::string file = path.substr(start == std::string::npos ? 0 : start + 1);
            const std::string stem = file.substr(0, file.find_last_of('.'));
            if (stem == name || stem == name + "_test" || stem == "test_" + name)
                matches.insert(path);
        }
        if (matches.size() != 1) return sources;
        selected.insert(*matches.begin());
    }
    return selected.empty() ? sources : selected;
}
} }
