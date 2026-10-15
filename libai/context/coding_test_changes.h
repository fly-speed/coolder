#pragma once
#include "../agent/agent_protocol.h"
#include <map>

namespace libai {
namespace ai {

inline bool coding_test_source(const std::string &path) {
    const auto slash = path.find_last_of('/');
    const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    for (const char *suffix : {"_test.go", "_test.cpp", "_test.cc", "_test.c", ".test.js",
            ".test.ts", ".test.cjs", ".spec.js", ".spec.ts", "Test.java"}) {
        const std::string ending(suffix);
        if (name.size() >= ending.size() && name.compare(name.size()-ending.size(), ending.size(), ending)==0)
            return true;
    }
    return name.size() > 8 && name.compare(0, 5, "test_")==0 &&
        name.compare(name.size()-3, 3, ".py")==0;
}

// Defer only writes to recognizable test source. Build/config/fixture/deletion
// changes keep their normal automatic build; final fixed validation is unchanged.
inline bool coding_only_test_changes(const std::vector<agent_change_proposal_t> &before,
    const std::vector<agent_change_proposal_t> &after) {
    std::map<std::string, const agent_change_proposal_t *> previous;
    for (const auto &file : before) previous[file.path] = &file;
    bool changed = false;
    for (const auto &file : after) {
        auto found = previous.find(file.path);
        if (found != previous.end()) {
            const auto &old = *found->second;
            previous.erase(found);
            if (old.operation==file.operation && old.content==file.content && old.target_path==file.target_path)
                continue;
        }
        if (file.operation!="write" || !coding_test_source(file.path) || file.review_status=="rejected")
            return false;
        changed = true;
    }
    return changed && previous.empty();
}

}
}
