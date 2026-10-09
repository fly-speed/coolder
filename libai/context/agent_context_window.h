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
	// Initialize agent read context state from the supplied arguments.
	explicit agent_read_context_t(size_t budget)
	        : budget_(budget)
	        , bytes_(0)
	{
	}
	// Remove all retained source pages for the specified path.
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
	// Reset accumulated entries and counters for reuse.
	void clear()
	{
		files_.clear();
		order_.clear();
		priority_.clear();
		bytes_ = 0;
	}
	// Prefer the supplied paths when choosing source context to retain.
	void prioritize(const std::vector<std::string> &paths)
	{
		priority_ = std::set<std::string>(paths.begin(), paths.end());
	}
	// Retain a version-bound source page, evicting whole files to meet
	// the budget.
	bool put(const std::string &path, const std::string &version,
	    size_t offset, const std::string &record);
	// Serialize the currently retained source pages in recency order.
	std::string records() const;
	// Check whether the exact file version and byte-offset page is
	// retained.
	bool contains(const std::string &path, const std::string &version,
	    size_t offset) const
	{
		const auto found = files_.find(path);
		return found != files_.end() &&
		    found->second.version == version &&
		    found->second.pages.count(offset) != 0;
	}
	// Return the current retained byte count.
	size_t bytes() const
	{
		return bytes_;
	}
	// Return the number of files represented in retained source context.
	size_t files() const
	{
		return files_.size();
	}
	// Return the retained source version for each cached path.
	std::map<std::string, std::string> versions() const
	{
		std::map<std::string, std::string> result;
		for (const auto &file : files_)
			result[file.first] = file.second.version;
		return result;
	}

private:
	// Cached source version and the context records retained for that
	// file.
	struct file_t {
		// Content identity shared by all retained pages for this
		// file.
		std::string version;
		// Source pages indexed by their byte offset.
		std::map<size_t, std::string> pages;
	};
	// budget_: Maximum number of bytes retained across source pages.
	// bytes_: Number of bytes accounted for by this operation.
	size_t budget_, bytes_;
	// Source records indexed by path for bounded context retention.
	std::map<std::string, file_t> files_;
	// Recency order used to evict complete source records.
	std::deque<std::string> order_;
	// Source paths preferred when context eviction is necessary.
	std::set<std::string> priority_;
};

// Retain a version-bound source page, evicting whole files to meet the
// budget.
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

// Serialize the currently retained source pages in recency order.
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
	// Initialize agent context window state from the supplied arguments.
	explicit agent_context_window_t(size_t budget)
	        : budget_(budget)
	        , bytes_(0)
	{
	}
	// Retain a complete exchange and evict older exchanges to meet the
	// byte budget.
	void append(const std::string &exchange)
	{
		exchanges_.push_back(exchange);
		bytes_ += exchange.size();
		while (bytes_ > budget_ && exchanges_.size() > 1) {
			bytes_ -= exchanges_.front().size();
			exchanges_.pop_front();
		}
	}
	// Read a JSON scalar as text, using the supplied fallback when
	// applicable.
	std::string text() const
	{
		std::string result;
		for (std::deque<std::string>::const_iterator i =
		         exchanges_.begin();
		     i != exchanges_.end(); ++i)
			result += *i;
		return result;
	}
	// Return the current size of the retained context window.
	size_t size() const
	{
		return exchanges_.size();
	}
	// Check whether the compacted context frees sufficient prompt space.
	static bool compaction_saves_enough(size_t current, size_t candidate)
	{
		// Rewriting the prefix has a cache cost; require at least 20% reduction.
		return current > 0 && candidate <= current - current / 5 &&
		    candidate < current;
	}
	// Choose the next bounded context compaction threshold.
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
	// Check whether adding the next item would exceed the context limit.
	static bool needs_compaction(
	    size_t current, size_t addition, size_t limit)
	{
		return current > limit || addition > limit - current;
	}

private:
	// Maximum number of bytes retained across complete exchanges.
	size_t budget_;
	// Number of bytes accounted for by this operation.
	size_t bytes_;
	// Complete recent exchanges retained within the context byte budget.
	std::deque<std::string> exchanges_;
};

// Describe how to correct the malformed read-batch arguments.
inline const char *read_batch_argument_guidance(bool chinese = false)
{
	return prompt_text(prompt_id::read_batch_arguments, chinese);
}

}
}
