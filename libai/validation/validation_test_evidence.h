#pragma once
#include <string>
namespace webcool { namespace ai {
inline bool is_test_command(const std::string& id) {
    return id == "functional.acceptance" || id == "cpp.ctest" || id == "c.ctest"
        || (id.size() >= 5 && id.compare(id.size() - 5, 5, ".test") == 0);
}
inline bool reports_no_tests(const std::string& output) {
    return output.find("No tests were found") != std::string::npos
        || output.find("running 0 tests") != std::string::npos
        || output.find("Ran 0 tests") != std::string::npos
        || output.find("# tests 0") != std::string::npos
        || output.find("[no test files]") != std::string::npos;
}
} }
