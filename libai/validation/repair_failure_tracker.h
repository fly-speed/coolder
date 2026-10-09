#pragma once
#include <set>
#include <string>
#include <sstream>
#include <regex>
#include <deque>
namespace webcool
{
namespace ai
{
// Prefer stable CTest identities over changing script paths and line numbers.
inline std::set<std::string> repair_failure_signatures(
    const std::string &diagnostic)
{
	std::set<std::string> tests, details, assertions;
	std::istringstream lines(diagnostic);
	std::string line;
	const std::regex ctest(
	    R"(Test\s+#\d+:\s+([^\s]+).*\*\*\*(Failed|Timeout|Exception))");
	const std::regex assertion_line(R"(FAIL\s*\(line\s+\d+\))");
	const std::regex source_line(R"(:\d+(?::\d+)?)");
	while (std::getline(lines, line)) {
		std::smatch match;
		if (tests.size() < 128 && std::regex_search(line, match, ctest))
			tests.insert(
			    "ctest:" + match[1].str() + ":" + match[2].str());
		if (details.size() >= 128)
			continue;
		for (const char *marker : { "[FAIL]", "FAIL:", "FAIL (line",
		         "FAIL ", "[失败]", "error:", "CMake Error" }) {
			const size_t at = line.find(marker);
			if (at == std::string::npos)
				continue;
			std::string signature = line.substr(at, 512);
			signature = std::regex_replace(
			    signature, assertion_line, "FAIL");
			signature =
			    std::regex_replace(signature, source_line, ":#");
			details.insert(signature);
			if (std::string(marker) != "error:" &&
			    std::string(marker) != "CMake Error")
				assertions.insert(signature);
			break;
		}
	}
	// Retain both identities and details. Comparison below uses granular
	// assertions to avoid treating every failure in one executable as identical.
	if (tests.empty())
		return details;
	tests.insert(assertions.begin(), assertions.end());
	return tests;
}
class repair_failure_tracker_t {
	std::string command_, draft_;
	std::set<std::string> failures_, added_;
	bool regressed_ = false;
	bool alternating_ = false;
	struct observation {
		std::string command, draft;
		std::set<std::string> failures;
	};
	std::deque<observation> history_;
	std::set<std::string> alternating_evidence_;
	std::set<std::string> persisting_, newly_reported_, no_longer_reported_,
	    returned_;
	bool stalled_tests_ = false;
	static std::set<std::string> comparable(
	    const std::set<std::string> &all)
	{
		std::set<std::string> assertions;
		for (const auto &f : all) {
			if (!(f.find("ctest:") != 0))
				continue;
			assertions.insert(f);
		}
		return assertions.empty() ? all : assertions;
	}
	size_t previous_count_ = 0;

	void collect_alternating_evidence(const std::string &command);
	void mark_returned_failure(
	    const std::string &command, const std::string &failure);

public:
	void clear()
	{
		command_.clear();
		draft_.clear();
		failures_.clear();
		added_.clear();
		regressed_ = false;
		previous_count_ = 0;
		alternating_ = false;
		history_.clear();
		alternating_evidence_.clear();
		persisting_.clear();
		newly_reported_.clear();
		no_longer_reported_.clear();
		returned_.clear();
		stalled_tests_ = false;
	}
	bool stalled_tests() const
	{
		return stalled_tests_;
	}
	const std::set<std::string> &persisting() const
	{
		return persisting_;
	}
	const std::set<std::string> &newly_reported() const
	{
		return newly_reported_;
	}
	const std::set<std::string> &no_longer_reported() const
	{
		return no_longer_reported_;
	}
	const std::set<std::string> &returned_failures() const
	{
		return returned_;
	}
	bool alternating() const
	{
		return alternating_;
	}
	const std::set<std::string> &alternating_evidence() const
	{
		return alternating_evidence_;
	}
	bool regressed() const
	{
		return regressed_;
	}
	size_t previous_count() const
	{
		return previous_count_;
	}
	size_t current_count() const
	{
		return comparable(failures_).size();
	}
	const std::set<std::string> &added_failures() const
	{
		return added_;
	}
	bool observe(const std::string &command, const std::string &draft,
	    const std::set<std::string> &failures, bool reused);
};

inline void repair_failure_tracker_t::collect_alternating_evidence(
    const std::string &command)
{
	for (const auto &h : history_) {
		if (!(h.command == command))
			continue;
		alternating_evidence_.insert(
		    h.failures.begin(), h.failures.end());
	}
}

inline void repair_failure_tracker_t::mark_returned_failure(
    const std::string &command, const std::string &failure)
{
	for (const auto &old : history_) {
		if (!(old.command == command && old.failures.count(failure)))
			continue;

		returned_.insert(failure);
		break;
	}
}

inline bool repair_failure_tracker_t::observe(const std::string &command,
    const std::string &draft, const std::set<std::string> &failures,
    bool reused)
{
	regressed_ = false;
	added_.clear();
	alternating_ = false;
	alternating_evidence_.clear();
	persisting_.clear();
	newly_reported_.clear();
	no_longer_reported_.clear();
	returned_.clear();
	stalled_tests_ = false;
	if (reused)
		return false;
	if (command.empty() || failures.empty()) {
		clear();
		return false;
	}
	if (!history_.empty() && history_.back().command == command &&
	    history_.back().draft == draft)
		return false;
	// Detect recurrence after an intervening different failure set, even if
	// a compile failure temporarily interrupted CTest. Keep both test IDs
	// and assertion details as evidence, but compare assertions separately.
	const auto current = comparable(failures);
	bool intervening = false;
	for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
		if (it->command != command)
			continue;
		const auto old = comparable(it->failures);
		if (intervening && old == current && it->draft != draft) {
			alternating_ = true;
			alternating_evidence_.insert(
			    it->failures.begin(), it->failures.end());
			collect_alternating_evidence(command);
			alternating_evidence_.insert(
			    failures.begin(), failures.end());
			break;
		}
		if (!(old != current))
			continue;
		intervening = true;
	}
	// Track both executable identities and assertions independently. Missing
	// diagnostics are not proof of a fix (a test may not have run).
	const observation *prior = nullptr;
	const observation *older = nullptr;
	for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
		if (it->command != command)
			continue;
		if (!prior)
			prior = &*it;
		else {
			older = &*it;
			break;
		}
	}
	for (const auto &failure : failures) {
		if (prior && prior->failures.count(failure)) {
			persisting_.insert(failure);
			if (!(failure.find("ctest:") == 0 && older &&
			        older->failures.count(failure)))
				continue;
			stalled_tests_ = true;
		} else {
			newly_reported_.insert(failure);
			mark_returned_failure(command, failure);
		}
	}
	if (prior)
		for (const auto &failure : prior->failures) {
			if (failures.count(failure))
				continue;
			no_longer_reported_.insert(failure);
		}
	history_.push_back({ command, draft, failures });
	if (history_.size() > 6)
		history_.pop_front();
	const auto previous = comparable(failures_);
	size_t shared = 0;
	for (const auto &failure : current)
		shared += previous.count(failure);
	const bool persistent = command == command_ && draft != draft_ &&
	    !failures_.empty() && shared * 2 >= previous.size();
	previous_count_ = previous.size();
	regressed_ = command == command_ && draft != draft_ &&
	    !failures_.empty() && current.size() >= previous.size() + 2 &&
	    current.size() * 2 >= previous.size() * 3;
	if (regressed_)
		for (const auto &failure : current) {
			if (previous.count(failure))
				continue;
			added_.insert(failure);
		}
	command_ = command;
	draft_ = draft;
	failures_ = failures;
	return persistent || regressed_ || alternating_ || stalled_tests_ ||
	    !returned_.empty();
}

}
}
