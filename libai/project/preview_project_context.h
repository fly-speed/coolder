#pragma once
#include <string>
namespace webcool
{
namespace ai
{
// Only registered projects qualify. The capability is rooted at the previewed
// file's directory, never at an arbitrary client-supplied workspace path.
std::string preview_project_directory(
    const std::string &user_root, const std::string &document, bool local);
bool preview_read_tool(const std::string &name);
std::string preview_project_read(const std::string &directory,
    const std::string &tool, const std::string &path, const std::string &query);
}
}
