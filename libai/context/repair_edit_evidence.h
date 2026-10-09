#pragma once
#include "../common/utf8_text.h"
#include <algorithm>
#include <sstream>
#include <string>
namespace webcool
{
namespace ai
{
struct edit_excerpt_t {
	size_t offset;
	std::string content;
	bool truncated;
};
inline edit_excerpt_t edit_excerpt(
    const std::string &source, size_t at, size_t budget)
{
	size_t start = source.size() <= budget ? 0 : (at > 128 ? at - 128 : 0);
	start = std::min(start, source.size());
	while (start > 0 && start < source.size() &&
	    (static_cast<unsigned char>(source[start]) & 0xc0) == 0x80)
		--start;
	const std::string content = utf8_prefix(source.substr(start), budget);
	return { start, content,
		start != 0 || content.size() != source.size() };
}
inline size_t first_edit_offset(
    const std::string &before, const std::string &after)
{
	size_t at = 0;
	while (
	    at < before.size() && at < after.size() && before[at] == after[at])
		++at;
	return at;
}
// Preserve original failure lines and nearby expected/actual blocks; never
// synthesize a test input or claim an inferred expected value is observed.
inline std::string failure_examples(
    const std::string &diagnostic, size_t budget = 3072)
{
	std::istringstream lines(diagnostic);
	std::string line, out;
	int following = 0;
	while (std::getline(lines, line)) {
		bool marker = false;
		for (const char *token : { "FAIL ", "FAIL:", "[FAIL]", "[失败]",
		         "mismatch", "expected", "actual", "期望", "实际",
		         "error:", "CMake Error" }) {
			if (!(line.find(token) != std::string::npos))
				continue;

			marker = true;
			break;
		}
		if (marker)
			following = 8;
		if (following > 0) {
			out += line + "\n";
			--following;
		}
		if (out.size() >= budget)
			break;
	}
	return utf8_prefix(out.empty() ? diagnostic : out, budget);
}
}
}
