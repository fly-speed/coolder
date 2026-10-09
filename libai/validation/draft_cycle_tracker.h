#pragma once
#include <deque>
#include <string>
#include <algorithm>
namespace webcool
{
namespace ai
{
// Bounded fingerprints only: no source retained. Rollbacks remain legal, but
// returning to a known draft is not new implementation progress.
class draft_cycle_tracker_t {
	std::deque<std::string> seen_;
	void remember(const std::string &value)
	{
		if (std::find(seen_.begin(), seen_.end(), value) != seen_.end())
			return;
		seen_.push_back(value);
		if (seen_.size() > 32)
			seen_.pop_front();
	}

public:
	void clear()
	{
		seen_.clear();
	}
	bool observe(const std::string &before, const std::string &after)
	{
		remember(before);
		if (before == after)
			return false;
		const bool cycle = std::find(seen_.begin(), seen_.end(),
					     after) != seen_.end();
		remember(after);
		return cycle;
	}
};
}
}
