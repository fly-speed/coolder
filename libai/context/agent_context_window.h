#pragma once

#include "../prompt/prompt_templates.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include <string>

namespace webcool
{
namespace ai
{

// A bounded working set, keyed by file version and byte offset rather than
// transport batches. Evict whole files so one page cannot evict its siblings.
class agent_read_context_t {
public:
	explicit agent_read_context_t(size_t budget)
	        : budget_(budget)
	        , bytes_(0)
	{
	}
	void erase(const std::string &path)
	{
		auto found = files_.find(path);
		if (found == files_.end())
			return;
		for (const auto &page : found->second.pages)
			bytes_ -= page.second.size();
		files_.erase(found);
		for (auto i = order_.begin(); i != order_.end(); ++i) {
			if (!(*i == path))
				continue;

			order_.erase(i);
			break;
		}
	}
	void clear()
	{
		files_.clear();
		order_.clear();
		priority_.clear();
		bytes_ = 0;
	}
	void prioritize(const std::vector<std::string> &paths)
	{
		priority_ = std::set<std::string>(paths.begin(), paths.end());
	}
	bool put(const std::string &path, const std::string &version,
	    size_t offset, const std::string &record);
	std::string records() const;
	bool contains(const std::string &path, const std::string &version,
	    size_t offset) const
	{
		const auto found = files_.find(path);
		return found != files_.end() &&
		    found->second.version == version &&
		    found->second.pages.count(offset) != 0;
	}
	size_t bytes() const
	{
		return bytes_;
	}
	size_t files() const
	{
		return files_.size();
	}
	std::map<std::string, std::string> versions() const
	{
		std::map<std::string, std::string> result;
		for (const auto &file : files_)
			result[file.first] = file.second.version;
		return result;
	}

private:
	struct file_t {
		std::string version;
		std::map<size_t, std::string> pages;
	};
	size_t budget_, bytes_;
	std::map<std::string, file_t> files_;
	std::deque<std::string> order_;
	std::set<std::string> priority_;
};

inline bool agent_read_context_t::put(const std::string &path,
    const std::string &version, size_t offset, const std::string &record)
{
	if (path.empty() || version.empty())
		return false;
	auto found = files_.find(path);
	if (found != files_.end() && found->second.version != version)
		erase(path);
	if (record.size() > budget_) {
		erase(path);
		return false;
	}
	// Refresh recency at file granularity, never by pushing duplicate pages.
	for (auto i = order_.begin(); i != order_.end(); ++i) {
		if (!(*i == path))
			continue;

		order_.erase(i);
		break;
	}
	order_.push_back(path);
	file_t &file = files_[path];
	file.version = version;
	std::string &page = file.pages[offset];
	bytes_ -= page.size();
	page = record;
	bytes_ += page.size();
	while (bytes_ > budget_ && !order_.empty()) {
		const auto victim = std::find_if(order_.begin(), order_.end(),
		    [&](const std::string &candidate) {
			return priority_.count(candidate) == 0;
		});
		erase(victim == order_.end() ? order_.front() : *victim);
	}
	return files_.find(path) != files_.end();
}

inline std::string agent_read_context_t::records() const
{
	std::string result = "[";
	for (const auto &path : order_)
		for (const auto &page : files_.at(path).pages) {
			if (result.size() > 1)
				result += ",";
			result += page.second;
		}
	return result + "]";
}

// Retain complete recent exchanges, not just tool names. The low-water mark
// leaves space for new observations after compaction. Never cut a JSON result.
class agent_context_window_t {
public:
	explicit agent_context_window_t(size_t budget)
	        : budget_(budget)
	        , bytes_(0)
	{
	}
	void append(const std::string &exchange)
	{
		exchanges_.push_back(exchange);
		bytes_ += exchange.size();
		while (bytes_ > budget_ && exchanges_.size() > 1) {
			bytes_ -= exchanges_.front().size();
			exchanges_.pop_front();
		}
	}
	std::string text() const
	{
		std::string result;
		for (std::deque<std::string>::const_iterator i =
		         exchanges_.begin();
		     i != exchanges_.end(); ++i)
			result += *i;
		return result;
	}
	size_t size() const
	{
		return exchanges_.size();
	}
	static bool compaction_saves_enough(size_t current, size_t candidate)
	{
		// Rewriting the prefix has a cache cost; require at least 20% reduction.
		return current > 0 && candidate <= current - current / 5 &&
		    candidate < current;
	}
	static size_t next_compaction_limit(size_t retained, size_t configured,
	    size_t last_exchange, size_t hard_limit)
	{
		// Fit at least one batch like the last one after compaction. Doubling
		// its size also leaves room for accompanying findings and diagnostics.
		const size_t batch_room = last_exchange > hard_limit / 2 ?
		    hard_limit :
		    last_exchange * 2;
		const size_t room = std::max(configured / 2, batch_room);
		const size_t available =
		    hard_limit - std::min(retained, hard_limit);
		return std::min(hard_limit,
		    std::max(configured,
		        std::min(retained, hard_limit) +
		            std::min(room, available)));
	}
	static bool needs_compaction(
	    size_t current, size_t addition, size_t limit)
	{
		return current > limit || addition > limit - current;
	}

private:
	size_t budget_;
	size_t bytes_;
	std::deque<std::string> exchanges_;
};

inline const char *read_batch_argument_guidance(bool chinese = false)
{
	return prompt_text(prompt_id::read_batch_arguments, chinese);
}

}
}
