#pragma once
#include "line_diff.h"
#include <sstream>

namespace webcool
{
namespace ai
{
// Shared edit script, counts and context visibility. Callers own file labels,
// operation-specific summaries and their distinct truncation policies.
struct line_diff_preview_t {
	// old_lines: Number of lines represented from the original text.
	// new_lines: Number of lines represented from the revised text.
	std::vector<std::string> old_lines, new_lines;
	// Ordered filesystem operations belonging to this change set.
	std::vector<line_diff_op_t> operations;
	// Whether this result or message should be displayed.
	std::vector<bool> visible;
	// removed_lines: Number of source lines removed by the proposed edit.
	// added_lines: Number of source lines added by the proposed edit.
	size_t removed_lines = 0, added_lines = 0;

	// Split text into the line sequence used by the diff algorithm.
	static void split_lines(
	    const std::string &content, std::vector<std::string> &lines)
	{
		lines.clear();
		size_t begin = 0;
		while (begin < content.size()) {
			const size_t end = content.find('\n', begin);
			std::string line = content.substr(begin,
			    end == std::string::npos ? std::string::npos :
			                               end - begin);
			if (!line.empty() && line[line.size() - 1] == '\r')
				line.resize(line.size() - 1);
			lines.push_back(line);
			if (end == std::string::npos)
				break;
			begin = end + 1;
		}
	}
	// Initialize line diff preview state from the supplied arguments.
	line_diff_preview_t(
	    const std::string &original, const std::string &proposed)
	{
		split_lines(original, old_lines);
		split_lines(proposed, new_lines);
		build_line_diff(old_lines, new_lines, operations);
		visible.assign(operations.size(), false);
		for (size_t i = 0; i < operations.size(); ++i) {
			if (operations[i].kind == line_diff_removed)
				++removed_lines;
			if (operations[i].kind == line_diff_added)
				++added_lines;
			if (operations[i].kind == line_diff_equal)
				continue;
			const size_t begin = i > 3 ? i - 3 : 0;
			const size_t end = std::min(operations.size(), i + 4);
			for (size_t j = begin; j < end; ++j)
				visible[j] = true;
		}
	}
	// Append one insertion, deletion or context line to a diff preview.
	void append_line(
	    std::ostringstream &out, const line_diff_op_t &op) const
	{
		const char marker = op.kind == line_diff_equal ?
		    ' ' :
		    (op.kind == line_diff_removed ? '-' : '+');
		const size_t old_line =
		    op.kind == line_diff_added ? 0 : op.old_index + 1;
		const size_t new_line =
		    op.kind == line_diff_removed ? 0 : op.new_index + 1;
		const std::string &text = op.kind == line_diff_added ?
		    new_lines[op.new_index] :
		    old_lines[op.old_index];
		out << marker << ' ';
		if (old_line == 0)
			out << "    ";
		else
			out << old_line;
		out << ' ';
		if (new_line == 0)
			out << "    ";
		else
			out << new_line;
		out << " | " << text << '\n';
	}
};
}
}
