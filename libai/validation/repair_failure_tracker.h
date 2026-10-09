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
// Compares validation failures across drafts to detect stalled repairs.
class repair_failure_tracker_t {
	// command_: Command identifier associated with this validation
	// observation.
	// draft_: Fingerprint or identity of the draft under observation.
	std::string command_, draft_;
	// failures_: Normalized failure signatures for this observation.
	// added_: Failure signatures introduced by the latest validation
	// result.
	std::set<std::string> failures_, added_;
	// Whether the latest validation introduced a regression.
	bool regressed_ = false;
	// Whether failures alternate between previously observed drafts.
	bool alternating_ = false;
	// One validation outcome associated with a command and draft
	// fingerprint.
	struct observation {
		// command: Command identifier associated with this validation
		// observation.
		// draft: Fingerprint or identity of the draft under
		// observation.
		std::string command, draft;
		// Normalized failure signatures for this observation.
		std::set<std::string> failures;
	};
	// Bounded history used to compare earlier validation observations.
	std::deque<observation> history_;
	// Failure signatures supporting the alternating-repair diagnosis.
	std::set<std::string> alternating_evidence_;
	// persisting_: Failure signatures shared with the preceding
	// observation.
	// newly_reported_: Failure signatures first reported by the latest
	// observation.
	// no_longer_reported_: Earlier failures absent from the latest
	// validation report.
	// returned_: Previously absent failures that appeared again in the
	// latest observation.
	std::set<std::string> persisting_, newly_reported_, no_longer_reported_,
	    returned_;
	// Whether the same tests continue failing across repair attempts.
	bool stalled_tests_ = false;
	// Select the failure signatures used to compare successive repair
	// outcomes.
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
	// Failure count retained from the preceding observation.
	size_t previous_count_ = 0;

	// Collect earlier command failures supporting an oscillating-repair
	// diagnosis.
	void collect_alternating_evidence(const std::string &command);
	// Record that a previously observed failure has appeared again.
	void mark_returned_failure(
	    const std::string &command, const std::string &failure);

public:
	// Reset accumulated entries and counters for reuse.
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
	// Report whether the same tests remain failing across repair
	// attempts.
	bool stalled_tests() const
	{
		return stalled_tests_;
	}
	// Return failure signatures present in consecutive validation
	// reports.
	const std::set<std::string> &persisting() const
	{
		return persisting_;
	}
	// Return signatures introduced by the latest validation report.
	const std::set<std::string> &newly_reported() const
	{
		return newly_reported_;
	}
	// Return earlier signatures absent from the latest validation report.
	const std::set<std::string> &no_longer_reported() const
	{
		return no_longer_reported_;
	}
	// Return signatures that reappeared after an intervening observation.
	const std::set<std::string> &returned_failures() const
	{
		return returned_;
	}
	// Report whether validation outcomes alternate between earlier
	// drafts.
	bool alternating() const
	{
		return alternating_;
	}
	// Return the evidence used to identify alternating failures.
	const std::set<std::string> &alternating_evidence() const
	{
		return alternating_evidence_;
	}
	// Report whether the latest validation introduced a regression.
	bool regressed() const
	{
		return regressed_;
	}
	// Return the number of failures in the preceding observation.
	size_t previous_count() const
	{
		return previous_count_;
	}
	// Return the number of failures in the current observation.
	size_t current_count() const
	{
		return comparable(failures_).size();
	}
	// Return the signatures added by the latest validation result.
	const std::set<std::string> &added_failures() const
	{
		return added_;
	}
	// Compare failures for this command and draft, including regression
	// and cycle evidence.
	bool observe(const std::string &command, const std::string &draft,
	    const std::set<std::string> &failures, bool reused);
};

// Collect earlier command failures supporting an oscillating-repair
// diagnosis.
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

// Record that a previously observed failure has appeared again.
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

// Compare failures for this command and draft, including regression and cycle
// evidence.
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
