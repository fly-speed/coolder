#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace webcool
{
namespace ai
{
// Only coverage metadata, never source text. Reset when model history is compacted.
class agent_read_coverage_t {
	struct file_t {
		std::string version;
		std::vector<std::pair<size_t, size_t>> ranges;
	};
	std::map<std::string, file_t> files_;

public:
	void clear()
	{
		files_.clear();
	}
	size_t observe(const std::string &path, const std::string &version,
		       size_t begin, size_t end)
	{
		if (end <= begin || version.empty())
			return end > begin ? end - begin : 0;
		if (files_.size() >= 512 && files_.find(path) == files_.end())
			files_.clear();
		file_t &file = files_[path];
		if (file.version != version) {
			file.version = version;
			file.ranges.clear();
		}
		size_t added = end - begin;
		for (const auto &range : file.ranges) {
			const size_t left = std::max(begin, range.first);
			const size_t right = std::min(end, range.second);
			if (right > left)
				added -= right - left;
		}
		file.ranges.push_back(std::make_pair(begin, end));
		std::sort(file.ranges.begin(), file.ranges.end());
		std::vector<std::pair<size_t, size_t>> merged;
		for (const auto &range : file.ranges) {
			if (merged.empty() ||
			    range.first > merged.back().second)
				merged.push_back(range);
			else
				merged.back().second = std::max(
					merged.back().second, range.second);
		}
		file.ranges.swap(merged);
		if (file.ranges.size() > 512)
			file.ranges.clear();
		return added;
	}
};
}
}
