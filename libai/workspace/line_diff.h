#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace webcool {
namespace ai {

// One item in a shortest line edit script. Equal items carry both source
// indexes; removed/added items carry the index from their respective side.
// Keeping indexes here lets preview renderers report exact source line numbers.
enum line_diff_kind_t {
	line_diff_equal,
	line_diff_removed,
	line_diff_added
};

struct line_diff_op_t {
	line_diff_kind_t kind;
	size_t old_index;
	size_t new_index;

	line_diff_op_t(line_diff_kind_t value, size_t old_value, size_t new_value)
		: kind(value), old_index(old_value), new_index(new_value) {}
};

// Build a Myers shortest edit script after trimming the unchanged prefix and
// suffix. Normal source edits have a small edit distance, so this is both fast
// and precise. The bounded fallback prevents an almost entirely rewritten
// 1 MiB file from consuming unbounded memory while preparing a preview.
inline void build_line_diff(const std::vector<std::string>& old_lines,
	const std::vector<std::string>& new_lines,
	std::vector<line_diff_op_t>& operations)
{
	operations.clear();
	size_t prefix = 0;
	while (prefix < old_lines.size() && prefix < new_lines.size()
		&& old_lines[prefix] == new_lines[prefix]) ++prefix;
	size_t suffix = 0;
	while (suffix < old_lines.size() - prefix
		&& suffix < new_lines.size() - prefix
		&& old_lines[old_lines.size() - 1 - suffix]
			== new_lines[new_lines.size() - 1 - suffix]) ++suffix;
	for (size_t i = 0; i < prefix; ++i) {
		operations.push_back(line_diff_op_t(line_diff_equal, i, i));
	}

	const size_t old_count = old_lines.size() - prefix - suffix;
	const size_t new_count = new_lines.size() - prefix - suffix;
	const size_t total = old_count + new_count;
	const size_t kMaxEditDistance = 1024;
	const size_t limit = std::min(total, kMaxEditDistance);
	const long long width = static_cast<long long>(limit * 2 + 3);
	const long long offset = static_cast<long long>(limit + 1);
	std::vector<std::vector<long long> > trace;
	std::vector<long long> frontier(static_cast<size_t>(width), -1);
	frontier[static_cast<size_t>(offset + 1)] = 0;
	bool found = total == 0;
	size_t found_distance = 0;

	for (size_t distance = 0; !found && distance <= limit; ++distance) {
		trace.push_back(frontier);
		const long long d = static_cast<long long>(distance);
		for (long long diagonal = -d; diagonal <= d; diagonal += 2) {
			const size_t slot = static_cast<size_t>(offset + diagonal);
			long long x;
			if (diagonal == -d || (diagonal != d
				&& frontier[slot - 1] < frontier[slot + 1])) {
				x = frontier[slot + 1];
			} else {
				x = frontier[slot - 1] + 1;
			}
			long long y = x - diagonal;
			while (x < static_cast<long long>(old_count)
				&& y < static_cast<long long>(new_count)
				&& old_lines[prefix + static_cast<size_t>(x)]
					== new_lines[prefix + static_cast<size_t>(y)]) {
				++x;
				++y;
			}
			frontier[slot] = x;
			if (x >= static_cast<long long>(old_count)
				&& y >= static_cast<long long>(new_count)) {
				found = true;
				found_distance = distance;
				break;
			}
		}
	}

	std::vector<line_diff_op_t> middle;
	if (found && total != 0) {
		long long x = static_cast<long long>(old_count);
		long long y = static_cast<long long>(new_count);
		for (long long distance = static_cast<long long>(found_distance);
			distance >= 0; --distance) {
			const std::vector<long long>& previous =
				trace[static_cast<size_t>(distance)];
			const long long diagonal = x - y;
			const size_t slot = static_cast<size_t>(offset + diagonal);
			const long long previous_diagonal = (diagonal == -distance
				|| (diagonal != distance
					&& previous[slot - 1] < previous[slot + 1]))
				? diagonal + 1 : diagonal - 1;
			const long long previous_x = previous[static_cast<size_t>(
				offset + previous_diagonal)];
			const long long previous_y = previous_x - previous_diagonal;
			while (x > previous_x && y > previous_y) {
				--x;
				--y;
				middle.push_back(line_diff_op_t(line_diff_equal,
					prefix + static_cast<size_t>(x),
					prefix + static_cast<size_t>(y)));
			}
			if (distance == 0) break;
			if (x == previous_x) {
				--y;
				middle.push_back(line_diff_op_t(line_diff_added,
					prefix + static_cast<size_t>(x),
					prefix + static_cast<size_t>(y)));
			} else {
				--x;
				middle.push_back(line_diff_op_t(line_diff_removed,
					prefix + static_cast<size_t>(x),
					prefix + static_cast<size_t>(y)));
			}
		}
		std::reverse(middle.begin(), middle.end());
	} else if (!found) {
		// A very large rewrite is intentionally represented as one replacement.
		// This fallback is reached only when the shortest script exceeds the
		// safety bound; smaller edits still receive exact line-level hunks.
		for (size_t i = 0; i < old_count; ++i) {
			middle.push_back(line_diff_op_t(line_diff_removed,
				prefix + i, prefix));
		}
		for (size_t i = 0; i < new_count; ++i) {
			middle.push_back(line_diff_op_t(line_diff_added,
				prefix + old_count, prefix + i));
		}
	}
	operations.insert(operations.end(), middle.begin(), middle.end());
	for (size_t i = 0; i < suffix; ++i) {
		operations.push_back(line_diff_op_t(line_diff_equal,
			old_lines.size() - suffix + i,
			new_lines.size() - suffix + i));
	}
}

} // namespace ai
} // namespace webcool
