#pragma once
#include <string>
#include <initializer_list>
#include <vector>

namespace webcool
{
namespace ai
{
// Per-run live-page evidence, never recovered from model prose or old transcripts.
// A reversible computed-style change is hypothesis evidence, not visual acceptance.
class browser_repair_evidence_t {
public:
	bool enabled = false;
	static bool requests_debug(std::string prompt)
	{
		const auto text = normalize(prompt);
		if (!contains(text,
		        { "不要调试", "不需要调试", "不要浏览器调试",
		            "不启用浏览器调试", "donotdebug", "donotusebrowser",
		            "withoutbrowserdebug" }))
			return contains(text,
			    { "调试当前浏览器", "调试当前页面", "调试浏览器",
			        "浏览器调试", "调试firefox", "调试chrome",
			        "调试edge", "调试safari", "debugthebrowser",
			        "browserdebug", "debugfirefox", "debugchrome",
			        "pairedlivebrowser", "browser.inspect",
			        "browser.snapshot" });
		return false;
	}
	// History contains user requests only, scoped to one project/conversation.
	// The current request is included exactly once by task_contract_store.
	static bool applies_history(const std::vector<std::string> &requests,
	    unsigned long threshold = 3, unsigned long *report_count = nullptr)
	{
		unsigned occurrences = 0;
		std::string engine, area, symptom;
		bool forced = false;
		for (const auto &request : requests) {
			const auto text = normalize(request);
			const auto next_engine = browser(request),
			           next_area = region(text),
			           next_symptom = symptom_kind(text);
			const bool solved =
			    contains(text,
			        { "解决了", "修好了", "已解决", "已修复",
			            "恢复正常", "已经正常", "nowfixed",
			            "resolved" }) &&
			    !contains(text,
			        { "没有解决", "没解决", "未解决", "还没",
			            "notfixed", "notresolved" });
			const bool new_issue = contains(text,
			    { "另一个", "另外一个", "新问题", "differentissue",
			        "anotherissue" });
			const bool mismatch =
			    (!engine.empty() && !next_engine.empty() &&
			        engine != "browser" &&
			        next_engine != "browser" &&
			        engine != next_engine) ||
			    (!area.empty() && !next_area.empty() &&
			        area != next_area) ||
			    (!symptom.empty() && !next_symptom.empty() &&
			        symptom != next_symptom);
			if (solved || new_issue || mismatch ||
			    (occurrences && !followup(text) &&
			        !requests_debug(text))) {
				occurrences = 0;
				engine.clear();
				area.clear();
				symptom.clear();
				forced = false;
			}
			if (solved)
				continue;
			const bool explicit_request = requests_debug(text);
			// Visual reports need not repeat a browser name: many users describe only
			// the page defect. A later explicit browser name refines this same chain.
			if (!(visual(text) || (occurrences && followup(text)) ||
			        explicit_request))
				continue;
			++occurrences;
			if (!next_engine.empty() &&
			    (next_engine != "browser" || engine.empty()))
				engine = next_engine;
			if (!next_area.empty())
				area = next_area;
			if (!next_symptom.empty())
				symptom = next_symptom;
			forced = forced || explicit_request;
		}
		if (!report_count)
			return forced || occurrences >= threshold;
		*report_count = occurrences;
		return forced || occurrences >= threshold;
	}
	static bool applies(const std::string &prompt,
	    std::vector<std::string> previous_requests = {},
	    unsigned long threshold = 3)
	{
		previous_requests.push_back(prompt);
		return applies_history(previous_requests, threshold);
	}
	bool needs_initial_observation() const
	{
		return enabled && !overview;
	}
	void bind(const std::string &id)
	{
		if (session == id)
			return;
		session = id;
		overview = false;
		baseline.clear();
		selector.clear();
		patch.clear();
		changed = false;
		undone = false;
		ready = false;
		overlay_selector.clear();
		experiment_description.clear();
	}
	void observe(const std::string &name, const std::string &query,
	    const std::string &data, const std::string &patch_id,
	    const std::string &undo_id, bool ok);
	// Derived from structured live observations, never from page text/model claims.
	void suspect_overlay(const std::string &target)
	{
		if (target.empty() || !overlay_selector.empty())
			return;
		overlay_selector = target;
		ready = false;
		baseline.clear();
		selector.clear();
	}
	void describe_experiment(const std::string &description)
	{
		experiment_description = description;
	}
	const std::string &experiment_target() const
	{
		return overlay_selector;
	}
	std::string overlay_guidance() const
	{
		if (!overlay_selector.empty())
			return "Live observations contain a visible fixed iframe with a transparent CSS background and dark color-scheme. "
			       "This is a competing occlusion hypothesis, NOT proof of an empty layout region. pointer-events:none and transparent background do not exclude opaque iframe document painting. "
			       "Compare its rectangle with the reported blank region and the canvas rectangle. Test color-scheme:normal on html,body, inspect the iframe, and undo. "
			       "The runtime measures this candidate iframe itself; changing an unrelated container's height does not establish evidence. "
			       "If supported, keep BOTH html AND body at color-scheme:normal and scope dark only to application wrappers such as the header/main content. body is NOT an isolated application wrapper: the injected iframe is its descendant. Moving color-scheme:dark from :root to body recreates the same iframe inheritance and does NOT reproduce the successful html,body experiment. Do not ship rules hiding/removing user extension frames. "
			       "A style change alone is not visual acceptance; compare the symptom before/after if screenshots are available, otherwise explicitly state visual verification is pending. ";
		return "";
	}
	const std::string &session_id() const
	{
		return session;
	}
	const std::string &active_patch() const
	{
		return patch;
	}
	bool permits_edits() const
	{
		return !enabled || (!session.empty() && ready);
	}
	std::string guidance() const
	{
		if (!enabled)
			return "";
		if (session.empty())
			return "Affected browser tab is not connected to this run. Call browser.status; if unavailable, report the pairing blocker with changes=[] instead of guessing CSS.";
		if (!overview)
			return "First call browser.snapshot or browser.overlays on the affected tab. Source reads, baseline validation and source edits are deferred until this live observation; do not spend the investigation budget guessing viewport CSS. Inspect fixed overlays and iframe color-scheme inheritance, not just canvas geometry.";
		if (ready)
			return overlay_guidance() +
			    "Completed reversible experiment: " +
			    experiment_description +
			    ". The proposed source fix must implement this tested change. Do not discard the experiment and switch to untested viewport/canvas fixes after reading source comments. Comments and previous model summaries are hypotheses, not runtime evidence. If the experiment did not improve the symptom, run another experiment or report that limitation instead of making unrelated changes. Reversible live-style evidence observed. Submit minimal source proposals. This does not prove the visual symptom is fixed; drafts are not deployed to the live tab. Applied-source Firefox verification remains pending.";
		if (patch.empty())
			return overlay_guidance() +
			    "Before source edits: capture browser.snapshot or browser.overlays; browser.inspect the affected region; test one specific hypothesis with browser.patch_style; inspect the SAME selector to observe a change; undo and inspect again to verify restoration. If no experiment explains the issue, report the evidence/blocker without edits.";
		return overlay_guidance() +
		    (undone ?
		            "Inspect the SAME selector after undo to verify restoration before source edits." :
		            "The runtime has automatically inspected the experiment target before and after the patch. Assess the symptom, then call browser.patch_style with content {\"undo\":\"all\"}; the runtime will inspect restoration automatically. Do not stack hypotheses or leave temporary styles active.");
	}

private:
	static std::string normalize(std::string text)
	{
		std::string out;
		for (char c : text) {
			if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
				continue;
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
			out += c;
		}
		return out;
	}
	static bool visual(const std::string &text)
	{
		return contains(text,
		    { "空白", "留白", "白块", "白边", "显示不全", "显示异常",
		        "显示问题", "错位", "重叠", "显示不完整", "遮挡",
		        "遮住", "裁切", "截断", "布局", "渲染", "溢出", "blank",
		        "whiteoverlay", "whitebottom", "clipped", "cutoff",
		        "cut-off", "layout", "rendering", "overflow" });
	}
	static std::string browser(std::string text);
	static std::string region(const std::string &text)
	{
		if (contains(text, { "底部", "下面", "下方", "bottom" }))
			return "bottom";
		if (contains(text, { "顶部", "上方", "top" }))
			return "top";
		if (contains(text, { "右边", "右侧", "right" }))
			return "right";
		if (!contains(text, { "左边", "左侧", "left" }))
			return "";
		return "left";
	}
	static std::string symptom_kind(const std::string &text)
	{
		if (contains(text,
		        { "空白", "留白", "白块", "白边", "显示不全",
		            "显示不完整", "裁切", "截断", "blank", "white",
		            "clipped", "cutoff" }))
			return "blank-or-clipped";
		if (!contains(
		        text, { "错位", "重叠", "overlap", "misaligned" }))
			return "";
		return "misaligned";
	}
	static bool followup(std::string text)
	{
		if (visual(text))
			return true;
		if (text.empty() || text.size() > 180)
			return false;
		if (contains(text,
		        { "sameissue", "stillnotfixed",
		            "continuefixingthis" }) ||
		    text == "continue")
			return true;
		for (auto part : { "请", "继续", "修复", "解决", "问题", "同一",
		         "还是", "仍然", "依然", "还", "仍", "没有", "未", "没",
		         "不行", "根本", "。", "，", "！", "？" }) {
			size_t pos;
			while ((pos = text.find(part)) != std::string::npos)
				text.erase(pos, std::string(part).size());
		}
		return text.empty();
	}
	static bool contains(
	    const std::string &s, std::initializer_list<const char *> words)
	{
		for (auto word : words) {
			if (!(s.find(word) != std::string::npos))
				continue;
			return true;
		}
		return false;
	}
	std::string session, baseline, selector, patch, overlay_selector,
	    experiment_description;
	bool overview = false, changed = false, undone = false, ready = false;
};

inline void browser_repair_evidence_t::observe(const std::string &name,
    const std::string &query, const std::string &data,
    const std::string &patch_id, const std::string &undo_id, bool ok)
{
	if (session.empty())
		return;
	if (!ok) {
		ready = false;
		return;
	}
	if (name == "browser.snapshot" || name == "browser.overlays")
		overview = true;
	if (name == "browser.interact") {
		// Navigation, scrolling and other interaction can change the evidence baseline.
		ready = false;
		baseline.clear();
		overview = false;
	}
	if (name == "browser.patch_style") {
		// A repeated no-op undo must not erase a completed experiment.
		if (undo_id.empty() || !patch.empty())
			ready = false;
		if (!undo_id.empty()) {
			if (!patch.empty() &&
			    (undo_id == "all" || undo_id == patch))
				undone = true;
		} else if (!patch_id.empty()) {
			// Overlapping experiments cannot establish a single reversible cause.
			if (!patch.empty())
				baseline.clear();
			patch = patch_id;
			changed = false;
			undone = false;
		}
	}
	if (name != "browser.inspect" || data.empty())
		return;
	if (patch.empty()) {
		baseline = data;
		selector = query;
		return;
	}
	if (query != selector || baseline.empty())
		return;
	if (!undone)
		changed = changed || data != baseline;
	else {
		ready = overview && changed && data == baseline;
		patch.clear();
		changed = false;
		undone = false;
		baseline = data;
	}
}

inline std::string browser_repair_evidence_t::browser(std::string text)
{
	for (char &c : text) {
		if (!(c >= 'A' && c <= 'Z'))
			continue;
		c += 'a' - 'A';
	}
	const auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
	size_t first = std::string::npos;
	std::string result;
	for (auto name : { "firefox", "火狐", "chromium", "chrome",
	         "谷歌浏览器", "edge", "safari" }) {
		size_t at = text.find(name);
		while (at != std::string::npos) {
			const auto end = at + std::string(name).size();
			if ((at == 0 || !letter(text[at - 1])) &&
			    (end == text.size() || !letter(text[end]))) {
				result = at < first ? name : result;
				first = std::min(at, first);
				break;
			}
			at = text.find(name, at + 1);
		}
	}
	if (result == "火狐")
		return "firefox";
	if (result == "chromium" || result == "谷歌浏览器")
		return "chrome";
	if (!(result.empty() && contains(text, { "浏览器", "browser" })))
		return result;
	return "browser";
}

}
}
