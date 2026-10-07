#include "stdafx.h"
// Run submission, status, event streams, cancellation and pause HTTP actions.
#include "ai_agent_actions_internal.h"

namespace action {
using namespace agent_detail;

class runtime_subscription_guard_t {
public:
	explicit runtime_subscription_guard_t(
		const std::shared_ptr<agent_runtime_task_t>& task)
	: task_(task), active_(task && begin_runtime_subscription(task)) {}

	~runtime_subscription_guard_t() {
		if (!active_) return;
		std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
		if (task_->event_subscribers > 0) --task_->event_subscribers;
	}

	bool acquired() const { return !task_ || active_; }

private:
	std::shared_ptr<agent_runtime_task_t> task_;
	bool active_;
};

namespace {

// Shared data for one synchronous submission. Worker captures below own copies.
// Stores are created only after their authenticated roots/session are resolved.
// Validation helpers return false after sending a response; the action then returns true.
struct run_start_state_t {
	std::string upload_root{};
	std::string username{};
	std::string user_root{};
	std::string ui_language{};
	std::string agent_id{};
	std::string provider_id{};
	std::string prompt{};
	std::string raw_path{};
	std::string project_id{};
	std::string attachment_draft{};
	std::string execution_mode{};
	std::string session_id{};
	std::string requested_effort{};
	std::string document_path{};
	std::string raw_document{};
	std::string conversation_id{};
	std::string project_path{};
	std::string provider_username{};
	std::string provider_root{};
	std::string previous_summary{};
	std::string initial_prompt{};
	std::string recovered_reasoning{};
	std::string attachment_text_context{};
	std::string api_key{};
	std::string run_id{};
	std::string thinking_mode{};
	std::string reasoning_effort{};
	std::string usage_err{};
	std::string err{};
	bool remember_session{};
	bool resume_after_restart{};
	bool thinking_enabled{};
	bool resume_from_progress{};
	bool assistant_chat{};
	bool text_preview{};
	bool usage_ok{};
	bool resumed_from_progress{};
	size_t max_tool_calls{};
	size_t max_no_progress_tool_calls{};
	size_t tool_context_compaction_bytes{};
	size_t completed_tool_calls{};
	long long max_tokens{};
	long long profiled_max_tokens{};
	unsigned long policy_max_tokens{};
	acl::json* body{};
	const webcool::ai::agent_definition_t* agent{};
	webcool::ai::agent_project_record_t project{};
	webcool::ai::sandbox_limits_t sandbox_limits{};
	std::vector<webcool::ai::provider_config_t> providers{};
	const webcool::ai::provider_config_t* provider{};
	webcool::ai::provider_config_t effective_provider{};
	webcool::ai::provider_usage_probe_t usage_probe{};
	std::vector<webcool::ai::completion_image_t> request_images{};
	std::shared_ptr<agent_runtime_task_t> runtime_task{};
	std::unique_ptr<webcool::ai::provider_store_t> store;
	std::unique_ptr<webcool::ai::agent_progress_store_t> progress_store;
	std::unique_ptr<webcool::ai::agent_run_store_t> run_store;
};

bool authenticate_start_request(run_start_state_t& state, request_t& req, response_t& res)
{
	state.upload_root = runtime_upload_dir_get();
	bool admin = false;
	if (!auth_current_user(req, state.upload_root, state.username, admin)) {
		auth_send_required(req, res);
		return false;
	}
	if (!authenticated_user_upload_dir(req, state.upload_root, state.user_root, state.err)) {
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.body = req.getJson(256 * 1024);
	if (state.body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return false;
	}

	return true;
}

bool parse_start_request(run_start_state_t& state, request_t& req, response_t& res)
{
	state.ui_language = json_text((*state.body)["ui_language"]);
	if (!state.ui_language.empty() && state.ui_language != "zh" && state.ui_language != "en") {
		json_error(res, 400, "ui_language must be zh or en", req.isKeepAlive());
		return false;
	}
	if (state.ui_language.empty()) {
		user_prefs_t prefs = default_user_prefs();
		std::string prefs_err;
		load_user_prefs(state.upload_root, state.username, prefs, prefs_err);
		state.ui_language = prefs.ui_language == "en" ? "en" : "zh";
	}
	state.agent_id = json_text((*state.body)["agent_id"]);
	state.provider_id = json_text((*state.body)["provider_id"]);
	state.prompt = json_text((*state.body)["prompt"]);
	state.raw_path = json_text((*state.body)["path"]);
	state.project_id = json_text((*state.body)["project_id"]);
	state.attachment_draft = json_text((*state.body)["attachment_draft"]);
	state.execution_mode = json_text((*state.body)["execution_mode"])
		.empty() ? "standard" : json_text((*state.body)["execution_mode"]);
	state.session_id = json_text((*state.body)["session_id"]);
	state.remember_session = json_bool((*state.body)["remember_session"], false);
	state.resume_after_restart = json_bool(
		(*state.body)["resume_after_restart"], false);
	state.thinking_enabled = json_bool((*state.body)["thinking_enabled"], true);
	state.requested_effort = json_text((*state.body)["reasoning_effort"]);
	if (!state.requested_effort.empty() && state.requested_effort != "low"
		&& state.requested_effort != "high" && state.requested_effort != "max") {
		json_error(res, 400, "reasoning_effort must be low, high or max", req.isKeepAlive());
		return false;
	}
	state.resume_from_progress = json_bool(
		(*state.body)["resume_from_progress"], false);
	state.assistant_chat = json_bool((*state.body)["assistant_chat"], false);
	state.text_preview = state.assistant_chat || json_bool((*state.body)["text_preview"], false);
	if (state.text_preview && (state.remember_session || !state.session_id.empty()
		|| state.resume_from_progress || state.resume_after_restart || !state.project_id.empty())) {
		json_error(res, 400, "text preview must use an independent non-resumable run", req.isKeepAlive());
		return false;
	}
	state.raw_document = json_text((*state.body)["document_path"]);
	state.conversation_id = json_text((*state.body)["conversation_id"]);
	if (state.assistant_chat
		&& !webcool::ai::assistant_session_id_valid(state.conversation_id))
	{
		json_error(res, 400, "invalid assistant conversation id", req.isKeepAlive());
		return false;
	}
	if (state.raw_document.size() > 4096 || state.conversation_id.size() > 256
		|| state.raw_document.find('\0') != std::string::npos
		|| state.conversation_id.find('\0') != std::string::npos
		|| state.raw_document.find_first_of("\r\n") != std::string::npos
		|| state.conversation_id.find_first_of("\r\n") != std::string::npos) {
		json_error(res, 400, "invalid AI resource scope", req.isKeepAlive());
		return false;
	}
	if (state.text_preview && !state.assistant_chat && !state.raw_document.empty()) {
		if (json_bool((*state.body)["document_local"], false)) {
			if (state.raw_document[0] != '/' && state.raw_document[0] != '\\'
				&& !(state.raw_document.size() > 2 && state.raw_document[1] == ':'
					&& (state.raw_document[2] == '/' || state.raw_document[2] == '\\'))) {
				json_error(res, 400, "local document scope must be an absolute path", req.isKeepAlive());
				return false;
			}
			state.document_path = state.raw_document;
		} else {
			std::string relative_document;
			if (!normalize_relative_path(state.raw_document.c_str(), relative_document, state.err)) {
				json_error(res, 400, state.err.c_str(), req.isKeepAlive());
				return false;
			}
			state.document_path = join_upload_path(state.user_root, relative_document);
		}
	}

	return true;
}

bool configure_start_execution(run_start_state_t& state, request_t& req, response_t& res)
{
	// Capture one immutable policy snapshot so a concurrent administrator update
	// cannot mix limits from two different policy versions in the same request.
	const webcool::ai::ai_admin_policy_t runtime_policy =
		webcool::ai::ai_runtime_policy_get();
	state.policy_max_tokens = runtime_policy.max_output_tokens;
	state.sandbox_limits =
		runtime_policy.sandbox_limits;
	// The browser has no separate per-run token input, so the administrator's
	// value is the effective default as well as the enforced ceiling. Keeping a
	// hidden 16K default here made larger saved policy values ineffective and made
	// max_output_tokens failures impossible for an administrator to resolve.
	// Quick mode still applies its explicit 8K cap in build_agent_execution_profile.
	const long long default_max_tokens =
		static_cast<long long>(state.policy_max_tokens);
	const long long requested_max_tokens = json_number(
		(*state.body)["max_output_tokens"], default_max_tokens);
	webcool::ai::agent_execution_profile_t execution_profile;
	if (!webcool::ai::build_agent_execution_profile(state.execution_mode,
		static_cast<size_t>(webcool::ai::ai_tool_call_limit_for_mode(
			runtime_policy, state.execution_mode)),
		static_cast<size_t>(runtime_policy.max_no_progress_tool_calls),
		static_cast<size_t>(runtime_policy.tool_context_compaction_kib) * 1024,
		static_cast<long long>(state.policy_max_tokens),
		static_cast<long long>(runtime_policy.quick_mode_output_tokens),
		requested_max_tokens,
		execution_profile, state.err))
	{
		json_error(res, 400, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.max_tool_calls = execution_profile.max_tool_calls;
	state.max_no_progress_tool_calls =
		execution_profile.max_no_progress_calls;
	state.tool_context_compaction_bytes =
		execution_profile.context_compaction_bytes;
	state.max_tokens = execution_profile.max_output_tokens;
	state.profiled_max_tokens = state.max_tokens;
	state.agent =
		webcool::ai::agent_registry_t::instance().find(
			state.agent_id.empty() ? "coding" : state.agent_id);
	if (state.agent == NULL || !state.agent->enabled) {
		json_error(res, 404, "agent type not found or disabled", req.isKeepAlive());
		return false;
	}
	if (state.agent->id != "coding") {
		json_error(res, 400, "this agent type has no run handler", req.isKeepAlive());
		return false;
	}
	if ((!state.resume_from_progress && state.prompt.empty()) || state.prompt.size() > 32 * 1024) {
		json_error(res, 400, "prompt is required and must not exceed 32 KiB",
			req.isKeepAlive());
		return false;
	}

	return true;
}

bool resolve_start_project(run_start_state_t& state, request_t& req, response_t& res, operation_trace_t& trace)
{
	trace.phase("resolve_project");
	if (!webcool::ai::agent_workspace_t::normalize_path(state.raw_path, state.project_path,
		true, state.err))
	{
		json_error(res, 400, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!state.project_id.empty()) {
		webcool::ai::agent_project_store_t project_store(state.user_root);
		if (!project_store.get(state.project_id, state.project, state.err)) {
			json_error(res, state.err == "agent project not found" ? 404 : 500,
				state.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (state.project.project_path != state.project_path) {
			json_error(res, 409, "selected project does not match the workspace path",
				req.isKeepAlive());
			return false;
		}
	}


	return true;
}

bool select_start_provider(run_start_state_t& state, request_t& req, response_t& res, operation_trace_t& trace)
{
	trace.phase("provider_and_session");
	if (!auth_administrator_upload_dir(state.upload_root, state.provider_username,
		state.provider_root, state.err))
	{
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.store.reset(new webcool::ai::provider_store_t(state.upload_root, state.provider_root,
		state.provider_username));
	if (!state.store->list(state.providers, state.err)) {
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.provider =
		select_provider(state.providers, state.provider_id);
	if (state.provider == NULL) {
		json_error(res, 400, "no enabled AI provider matches this run",
			req.isKeepAlive());
		return false;
	}
	const bool image_provider = state.provider->protocol == "openai_images";
	const bool image_request = json_bool((*state.body)["image_generation"], false);
	if (image_request != image_provider || (image_provider && !state.assistant_chat)) {
		json_error(res, 400, "image generation requires an image provider in AI Assistant", req.isKeepAlive());
		return false;
	}
	if (image_provider) {
		const std::string size = json_text((*state.body)["image_size"]);
		if (size != "auto" && size != "1024x1024" && size != "1536x1024" && size != "1024x1536") {
			json_error(res, 400, "unsupported image size", req.isKeepAlive());
			return false;
		}
		std::vector<std::string> image_attachments;
		if (!parse_string_array((*state.body)["attachments"], 16, image_attachments)
			|| !image_attachments.empty() || !state.attachment_draft.empty())
		{
			json_error(res, 400, "image mode currently supports text prompts only", req.isKeepAlive());
			return false;
		}
	}
	// Record the provider that the server actually accepted. This complements
	// the UI change handler and also covers API clients or an untouched default
	// selection. Preference persistence must never prevent a valid AI run.
	user_prefs_t prefs;
	std::string prefs_err;
	if (load_user_prefs(state.upload_root, state.username, prefs, prefs_err)) {
		if (state.assistant_chat) {
			prefs.ai_assistant_provider_id = state.provider->id;
		} else if (state.text_preview) {
			prefs.document_ai_provider_id = state.provider->id;
		} else {
			prefs.ai_coding_provider_id = state.provider->id;
		}
		(void) save_user_prefs(state.upload_root, state.username, prefs, prefs_err);
	}
	if (provider_requires_extended_reasoning_budget(*state.provider)
		&& state.max_tokens < 16384)
	{
		// Quick mode normally caps a turn at 8K. Kimi's reasoning-only coding
		// models need room to reach the tool call; never exceed administrator
		// policy, and record the effective value in the per-run operation log.
		state.max_tokens = std::min<long long>(
			static_cast<long long>(state.policy_max_tokens), 16384);
	}
	// The selected project is the scope of this authorization; sensitive paths
	// remain blocked by agent_workspace_t even though ordinary source content is
	// available to the model for this coding run.
	state.effective_provider = *state.provider;
	state.effective_provider.allow_file_content = true;
	std::string response_state_mode = json_text((*state.body)["response_state_mode"]);
	if (response_state_mode.empty()) response_state_mode =
		json_bool((*state.body)["stateless_requests"], false) ? "stateless" : "auto";
	if (!webcool::ai::provider_client_t::configure_response_state(state.effective_provider,
		response_state_mode, state.err)) {
		json_error(res, 400, state.err.c_str(), req.isKeepAlive());
		return false;
	}

	return true;
}

bool preflight_start_provider(run_start_state_t& state, request_t& req, response_t& res)
{
	// Run provider preflight before creating a session, loading attachments or
	// registering a background task. A failed limit check therefore leaves no
	// empty conversation or partial run metadata behind.
	std::string usage_api_key;
	if (!state.store->reveal_api_key(*state.provider, usage_api_key, state.err)) {
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.usage_ok = webcool::ai::provider_client_t::probe_usage_limits(
		*state.provider, usage_api_key, state.usage_probe, state.usage_err);
	std::fill(usage_api_key.begin(), usage_api_key.end(), '\0');
	if (!state.usage_ok && (state.usage_probe.http_status == 401
		|| state.usage_probe.http_status == 403 || state.usage_probe.http_status == 429))
	{
		json_error(res, state.usage_probe.http_status == 429 ? 429 : 502,
			state.usage_err.c_str(), req.isKeepAlive());
		return false;
	}
	if (state.usage_ok && state.usage_probe.balance_known && !state.usage_probe.can_start) {
		json_error(res, 402,
			"AI provider account balance is exhausted; recharge before starting",
			req.isKeepAlive());
		return false;
	}
	if (!state.usage_ok) {
		webcool::ai::ai_log_error("agent.runtime", "usage-preflight-advisory",
			state.usage_err);
	}
	// DeepSeek V4 defaults to high-effort thinking when the request omits its
	// vendor toggle. Preserve existing behavior by defaulting the browser option
	// to enabled, while making an unchecked option an explicit disable request.
	state.thinking_mode = provider_supports_thinking_disable(*state.provider)
		? (state.thinking_enabled ? "enabled" : "disabled") : "";
	state.reasoning_effort =
		webcool::ai::provider_client_t::supports_reasoning_effort(*state.provider)
			&& state.thinking_mode != "disabled" ? state.requested_effort : "";

	return true;
}

bool restore_start_session(run_start_state_t& state, request_t& req, response_t& res)
{
	webcool::ai::agent_session_store_t session_store(state.user_root);
	webcool::ai::agent_session_record_t session;
	if (!state.session_id.empty()) {
		if (!session_store.get(state.session_id, session, state.err)) {
			const int status = state.err == "agent session not found" ? 404 : 400;
			json_error(res, status, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (session.agent_id != state.agent->id
			|| session.project_path != state.project_path
			|| session.provider_id != state.provider->id)
		{
			json_error(res, 409,
				"agent session does not match this provider and project",
				req.isKeepAlive());
			return false;
		}
		state.remember_session = true;
		state.previous_summary = session.summary;
	} else if (state.remember_session) {
		const std::string title = webcool::ai::agent_session_task_title(state.prompt);
		if (!session_store.create(title, state.agent->id, state.provider->id,
			state.project_path, session, state.err))
		{
			json_error(res, 500, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		state.session_id = session.id;
	}
	state.progress_store.reset(new webcool::ai::agent_progress_store_t(state.user_root, state.project_path,
		state.session_id));
	if (state.resume_from_progress) {
		// Never trust a browser-cached prompt as the recovery key. The checkpoint
		// is private to this authenticated project/session and contains the exact
		// original request that produced the saved transcript.
		webcool::ai::agent_progress_t saved_progress;
		bool progress_found = false;
		if (!state.progress_store->load(saved_progress, progress_found, state.err)) {
			json_error(res, 500, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (!progress_found) {
			state.err = "no recoverable coding-agent progress exists for this session";
			webcool::ai::ai_log_error("agent.runtime", "resume-progress-missing", state.err);
			json_error(res, 409, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (saved_progress.provider_id != state.provider->id
			|| saved_progress.project_path != state.project_path
			|| saved_progress.session_id != state.session_id)
		{
			state.err = "coding-agent progress does not match the selected provider/project/session";
			webcool::ai::ai_log_error("agent.runtime", "resume-progress-mismatch", state.err);
			json_error(res, 409, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		state.prompt = saved_progress.original_prompt;
	}

	return true;
}

bool prepare_start_context(run_start_state_t& state, request_t& req, response_t& res, operation_trace_t& trace)
{
	webcool::ai::agent_workspace_t workspace(state.user_root);
	if (!load_temporary_attachments((*state.body)["attachments"], state.attachment_draft,
		state.user_root, state.request_images, state.attachment_text_context, state.err))
	{
		json_error(res, 400, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	trace.phase("load_recovery_progress");
	state.completed_tool_calls = 0;
	state.resumed_from_progress = false;
	if (!state.text_preview && !load_matching_coding_progress(*state.progress_store, state.effective_provider,
		state.project_path, state.prompt, state.initial_prompt, state.recovered_reasoning, state.completed_tool_calls,
		state.resumed_from_progress, state.err)) {
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	if (!state.resumed_from_progress) {
		trace.phase("compose_initial_context");
		if (!state.text_preview && !compose_initial_prompt(workspace, state.effective_provider,
			state.user_root, state.project_path, state.prompt, state.session_id, state.execution_mode,
			state.previous_summary, state.initial_prompt, state.err, state.ui_language != "en",
			state.project_id.empty() ? NULL : &state.project, &trace)) {
			json_error(res, 400, state.err.c_str(), req.isKeepAlive());
			return false;
		}
		if (state.text_preview) state.initial_prompt = state.prompt;
		state.initial_prompt += state.attachment_text_context;
	}


	return true;
}

bool register_start_task(run_start_state_t& state, request_t& req, response_t& res, operation_trace_t& trace)
{
	trace.phase("create_run_and_checkpoint");
	if (!state.store->reveal_api_key(*state.provider, state.api_key, state.err)) {
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	state.run_id = new_run_id();
	if (state.run_id.empty()) {
		std::fill(state.api_key.begin(), state.api_key.end(), '\0');
		json_error(res, 500, "cannot generate agent run id", req.isKeepAlive());
		return false;
	}
	state.run_store.reset(new webcool::ai::agent_run_store_t(state.user_root));
	state.runtime_task.reset(new agent_runtime_task_t());
	state.runtime_task->id = state.run_id;
	state.runtime_task->upload_root = state.upload_root;
	state.runtime_task->user_root = state.user_root;
	state.runtime_task->username = state.username;
	state.runtime_task->project_path = state.project_path;
	trace.bind(state.runtime_task);
	state.runtime_task->original_prompt = state.prompt;
	state.runtime_task->ui_language = state.ui_language;
	state.runtime_task->initial_session_title = webcool::ai::agent_session_task_title(state.prompt);
	state.runtime_task->session_id = state.session_id;
	state.runtime_task->remember_session = state.remember_session;
	state.runtime_task->text_preview = state.text_preview;
	state.runtime_task->browser_debug_consent = json_bool((*state.body)["browser_debug_consent"], false);
	if (state.text_preview && !state.assistant_chat && state.effective_provider.allow_file_content) {
		state.runtime_task->preview_context_directory = webcool::ai::preview_project_directory(
			state.user_root, state.raw_document, json_bool((*state.body)["document_local"], false));
	}
	state.runtime_task->assistant_chat = state.assistant_chat;
	state.runtime_task->image_size = json_text((*state.body)["image_size"]);
	state.runtime_task->document_path = state.document_path;
	state.runtime_task->conversation_id = state.assistant_chat ? state.conversation_id : "";
	state.runtime_task->restart_recovery_enabled = state.resume_after_restart;
	state.runtime_task->resumed_from_progress = state.resumed_from_progress;
	state.runtime_task->progress_file = state.progress_store->relative_path();
	if (state.resumed_from_progress) state.runtime_task->phase = "recovering_progress";
	if (!register_runtime_task(state.runtime_task, state.err)) {
		std::fill(state.api_key.begin(), state.api_key.end(), '\0');
		json_error(res, 429, state.err.c_str(), req.isKeepAlive());
		return false;
	}
	webcool::ai::agent_run_record_t run_record;
	run_record.id = state.run_id;
	run_record.agent_id = state.agent->id;
	run_record.agent_version = state.agent->version;
	run_record.status = "running";
	run_record.provider_id = state.provider->id;
	run_record.model = state.provider->model;
	run_record.project_path = state.project_path;
	run_record.started_at = static_cast<long long>(time(NULL));
	if (!state.run_store->create(run_record, state.err)) {
		remove_runtime_task(state.user_root, state.run_id);
		std::fill(state.api_key.begin(), state.api_key.end(), '\0');
		json_error(res, 500, state.err.c_str(), req.isKeepAlive());
		return false;
	}

	if (state.assistant_chat) {
		webcool::ai::assistant_message_t message;
		message.role = "user";
		message.run_id = state.run_id;
		message.text = json_text((*state.body)["displayed_message"]);
		if (message.text.empty()) {
			acl::json chat(state.prompt.c_str());
			message.text = chat.finish() ? json_text(chat["user_message"]) : "";
			if (message.text.empty()) message.text = state.prompt;
		}
		if (message.text.size() > 128 * 1024
			|| !webcool::ai::assistant_session_store_t(state.user_root).append(
				state.conversation_id, message, state.err))
		{
			remove_runtime_task(state.user_root, state.run_id);
			std::fill(state.api_key.begin(), state.api_key.end(), '\0');
			std::string audit_err;
			state.run_store->fail(state.run_id, "cannot persist assistant message",
				audit_err);
			json_error(res, 500,
				state.err.empty() ? "assistant message too large" : state.err.c_str(),
				req.isKeepAlive());
			return false;
		}
	}

	return true;
}

void log_start_task(run_start_state_t& state)
{
	acl::json started_json;
	acl::json_node& started = started_json.create_node();
	started.add_text("event", "run_started");
	started.add_text("provider_id", state.provider->id.c_str());
	started.add_text("provider_protocol", state.provider->protocol.c_str());
	started.add_text("model", state.provider->model.c_str());
	started.add_text("execution_mode", state.execution_mode.c_str());
	started.add_text("thinking_mode", state.thinking_mode.empty()
		? "provider_default" : state.thinking_mode.c_str());
	started.add_text("reasoning_effort", state.reasoning_effort.empty()
		? "provider_default" : state.reasoning_effort.c_str());
	started.add_number("max_tool_calls",
		static_cast<long long>(state.max_tool_calls));
	started.add_number("max_no_progress_tool_calls",
		static_cast<long long>(state.max_no_progress_tool_calls));
	started.add_number("max_output_tokens", state.max_tokens);
	started.add_number("profiled_max_output_tokens", state.profiled_max_tokens);
	started.add_bool("reasoning_budget_floor_applied",
		state.max_tokens != state.profiled_max_tokens);
	started.add_number("context_compaction_bytes",
		static_cast<long long>(state.tool_context_compaction_bytes));
	started.add_bool("resumed_from_progress", state.resumed_from_progress);
	started.add_bool("restart_recovery_enabled", state.resume_after_restart);
	started.add_bool("attachments_present", !state.request_images.empty()
		|| !state.attachment_text_context.empty());
	append_runtime_operation_event(state.runtime_task, started);
}

bool save_start_checkpoint(run_start_state_t& state, request_t& req, response_t& res)
{
	if (state.resume_after_restart) {
		webcool::ai::agent_checkpoint_t checkpoint;
		checkpoint.run_id = state.run_id;
		checkpoint.provider_id = state.provider->id;
		checkpoint.project_path = state.project_path;
		checkpoint.prompt = state.prompt;
		checkpoint.ui_language = state.ui_language;
		checkpoint.session_id = state.session_id;
			checkpoint.max_output_tokens = state.max_tokens;
			checkpoint.thinking_mode = state.thinking_mode;
			checkpoint.reasoning_effort = state.reasoning_effort;
			checkpoint.response_state_mode = state.effective_provider.response_state_mode;
			checkpoint.execution_mode = state.execution_mode;
			checkpoint.remember_session = state.remember_session;
		webcool::ai::agent_checkpoint_store_t checkpoint_store(state.upload_root,
			state.user_root, state.username);
		if (!checkpoint_store.save(checkpoint, state.err)) {
			std::string audit_err;
			if (!state.run_store->fail(state.run_id,
				"cannot create encrypted restart checkpoint", audit_err))
			{
				webcool::ai::ai_log_error("agent.runtime",
					"audit-checkpoint-failure", audit_err);
			}
			remove_runtime_task(state.user_root, state.run_id);
			std::fill(state.api_key.begin(), state.api_key.end(), '\0');
			json_error(res, 500, state.err.c_str(), req.isKeepAlive());
			return false;
		}
	}

	return true;
}

void launch_start_worker(run_start_state_t& state)
{
	auto& user_root = state.user_root;
	auto& prompt = state.prompt;
	auto& attachment_draft = state.attachment_draft;
	auto& project_path = state.project_path;
	auto& initial_prompt = state.initial_prompt;
	auto& recovered_reasoning = state.recovered_reasoning;
	auto& api_key = state.api_key;
	auto& thinking_mode = state.thinking_mode;
	auto& reasoning_effort = state.reasoning_effort;
	auto& max_tool_calls = state.max_tool_calls;
	auto& max_no_progress_tool_calls = state.max_no_progress_tool_calls;
	auto& tool_context_compaction_bytes = state.tool_context_compaction_bytes;
	auto& completed_tool_calls = state.completed_tool_calls;
	auto& max_tokens = state.max_tokens;
	auto& sandbox_limits = state.sandbox_limits;
	auto& effective_provider = state.effective_provider;
	auto& request_images = state.request_images;
	auto& runtime_task = state.runtime_task;
	webcool::ai::provider_config_t provider_copy = effective_provider;
	provider_copy.api_key_ciphertext.clear();
	const std::shared_ptr<acl::fiber> worker = acl::gofiber(
			[runtime_task, provider_copy, api_key, project_path, prompt, initial_prompt,
			recovered_reasoning, request_images, completed_tool_calls,
			max_tool_calls, max_no_progress_tool_calls,
			tool_context_compaction_bytes, sandbox_limits, max_tokens,
			thinking_mode, reasoning_effort] () mutable {
			run_async_coding_task(runtime_task, provider_copy, api_key, project_path,
				prompt, initial_prompt, recovered_reasoning, request_images,
				completed_tool_calls, max_tool_calls, max_tokens,
				max_no_progress_tool_calls, tool_context_compaction_bytes,
				sandbox_limits, thinking_mode, reasoning_effort);
	});
	attach_runtime_worker(runtime_task, worker);
	// The worker owns bounded in-memory copies now. Physically remove every file
	// in this private draft before acknowledging the run to the browser.
	remove_temporary_attachment_draft(attachment_draft, user_root);
	std::fill(api_key.begin(), api_key.end(), '\0');
}

bool send_start_response(run_start_state_t& state, request_t& req, response_t& res, operation_trace_t& trace)
{
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_bool("started", true);
	root.add_bool("running", true);
	root.add_text("run_id", state.run_id.c_str());
	root.add_text("agent_id", state.agent->id.c_str());
	root.add_text("provider_id", state.provider->id.c_str());
	root.add_text("model", state.provider->model.c_str());
	root.add_bool("resumed_from_progress", state.resumed_from_progress);
	root.add_text("progress_file", state.progress_store->relative_path().c_str());
	root.add_text("phase", "queued");
	root.add_bool("restart_recovery_enabled", state.resume_after_restart);
	acl::json_node& preflight = json.create_node();
	root.add_bool("project_context_available", !state.runtime_task->preview_context_directory.empty());
	root.add_child("usage_preflight", preflight);
	preflight.add_bool("ok", state.usage_ok);
	preflight.add_bool("supported", state.usage_probe.supported);
	preflight.add_bool("checked", state.usage_probe.checked);
	preflight.add_bool("balance_known", state.usage_probe.balance_known);
	preflight.add_bool("can_start", state.usage_probe.can_start);
	preflight.add_number("http_status", state.usage_probe.http_status);
	preflight.add_number("latency_ms", state.usage_probe.latency_ms);
	if (state.usage_probe.balance_known) {
		preflight.add_double("available_balance", state.usage_probe.available_balance);
		preflight.add_text("currency", state.usage_probe.currency.c_str());
	}
	preflight.add_text("request_limit", state.usage_probe.request_limit.c_str());
	preflight.add_text("request_remaining", state.usage_probe.request_remaining.c_str());
	preflight.add_text("request_reset", state.usage_probe.request_reset.c_str());
	preflight.add_text("token_limit", state.usage_probe.token_limit.c_str());
	preflight.add_text("token_remaining", state.usage_probe.token_remaining.c_str());
	preflight.add_text("token_reset", state.usage_probe.token_reset.c_str());
	preflight.add_text("retry_after", state.usage_probe.retry_after.c_str());
	preflight.add_text("message", (state.usage_ok ? state.usage_probe.message : state.usage_err).c_str());
	if (!state.session_id.empty()) root.add_text("session_id", state.session_id.c_str());
	trace.phase("send_response");
	trace.complete();
	return sendJson(res, 202, root, req.isKeepAlive());
}

} // namespace

bool AiAgentRunStartAction::run(request_t& req, response_t& res) {
	operation_trace_t trace("submit");
	run_start_state_t state;
	if (!authenticate_start_request(state, req, res)) return true;
	if (!parse_start_request(state, req, res)) return true;
	if (!configure_start_execution(state, req, res)) return true;
	if (!resolve_start_project(state, req, res, trace)) return true;
	if (!select_start_provider(state, req, res, trace)) return true;
	if (!preflight_start_provider(state, req, res)) return true;
	if (!restore_start_session(state, req, res)) return true;
	if (!prepare_start_context(state, req, res, trace)) return true;
	if (!register_start_task(state, req, res, trace)) return true;
	log_start_task(state);
	if (!save_start_checkpoint(state, req, res)) return true;
	launch_start_worker(state);
	return send_start_response(state, req, res, trace);
}

// Durable review artifacts outlive the bounded run audit log. Resolve an
// archived run only through an authenticated user's session and project.
bool AiAgentRunListAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_user_root(req, res, user_root)) return true;
	const char* pending_flag = req.getParameter("pending_review");
	const bool pending_only = pending_flag && std::string(pending_flag) == "1";
	webcool::ai::agent_session_record_t review_session;
	if (pending_only) {
		const char* session_id = req.getParameter("session_id");
		webcool::ai::agent_session_store_t sessions(user_root);
		std::string session_err;
		if (!session_id || !sessions.get(session_id, review_session, session_err)) {
			json_error(res, 400, "invalid review session", req.isKeepAlive());
			return true;
		}
	}
	size_t limit = 20;
	const char* raw_limit = req.getParameter("limit");
	if (raw_limit != NULL && *raw_limit != '\0') {
		char* end = NULL;
		const long parsed = strtol(raw_limit, &end, 10);
		if (end == raw_limit || *end != '\0' || parsed < 1 || parsed > 100) {
			json_error(res, 400, "limit must be between 1 and 100",
				req.isKeepAlive());
			return true;
		}
		limit = static_cast<size_t>(parsed);
	}
	webcool::ai::agent_run_store_t store(user_root);
	std::vector<webcool::ai::agent_run_record_t> records;
	std::string err;
	if (!store.list(pending_only ? 100 : limit, records, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	acl::json_node& items = json.create_array();
	root.add_child("runs", items);
	if (pending_only) {
		webcool::ai::agent_result_store_t results(user_root, review_session.project_path);
		std::vector<webcool::ai::agent_pending_result_t> pending;
		if (!results.list_pending(review_session.id, pending, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		for (const auto& saved : pending) {
			acl::json_node& item = items.add_child(false, true);
			item.add_text("run_id", saved.run_id.c_str());
			item.add_number("started_at", saved.saved_at);
			item.add_number("pending_review_count", saved.pending_count);
			const auto record = std::find_if(records.begin(), records.end(),
				[&saved](const webcool::ai::agent_run_record_t& candidate) { return candidate.id == saved.run_id; });
			item.add_text("model", record != records.end() ? record->model.c_str() : "");
		}
	} else {
		for (size_t i = 0; i < records.size(); ++i) {
			acl::json_node& item = items.add_child(false, true);
			add_run_record_json(item, records[i]);
			// Running tasks have an exact session owner; project/provider matches
			// cannot distinguish two conversations in different browser windows.
			std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
			const auto task = g_agent_runtime_tasks.find(
				runtime_task_key(user_root, records[i].id));
			if (task != g_agent_runtime_tasks.end())
				item.add_text("session_id", task->second->session_id.c_str());
		}
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunStatusAction::run(request_t& req, response_t& res) {
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username)) return true;
	const char* raw_id = req.getParameter("id");
	const std::string id = raw_id ? raw_id : "";
	const char* raw_session_id = req.getParameter("session_id");
	const std::string session_hint = raw_session_id ? raw_session_id : "";
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!load_review_run_record(user_root, id, session_hint, record, err)) {
		int status = 500;
		if (err == "invalid agent run id") status = 400;
		else if (err == "agent run not found") status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::shared_ptr<agent_runtime_task_t> runtime_task =
		find_runtime_task(user_root, id);
	if (record.status == "running" && !runtime_task) {
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
			runtime_upload_dir_get(), user_root, username);
		if (checkpoint_store.exists(id)) {
			// Recovery is lazy: the first status or SSE request after startup
			// recreates the provider fiber from the encrypted checkpoint.
			runtime_task = recover_runtime_task(runtime_upload_dir_get(),
				user_root, username, record, err);
			if (!runtime_task) {
				json_error(res, 503, err.empty()
					? "agent restart recovery is temporarily unavailable"
					: err.c_str(), req.isKeepAlive());
				return true;
			}
		} else {
			std::string update_err;
			if (!store.fail(id, "agent run interrupted by service restart",
				update_err) || !store.get(id, record, update_err))
			{
				json_error(res, 500, update_err.c_str(), req.isKeepAlive());
				return true;
			}
		}
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	add_run_record_json(root, record);
	if (runtime_task) {
		// Terminal tasks remain cached for traces and usage. Their review state
		// may have been superseded by another run; use the same durable view as
		// the pending list and review endpoint, without discarding runtime data.
		webcool::ai::agent_result_t persisted;
		bool found = false;
		if (record.status != "running" && reviewable_agent_run_status(record.status)) {
			webcool::ai::agent_result_store_t result_store(user_root, record.project_path);
			if (!result_store.load(record.id, persisted, found, err)) {
				json_error(res, 500, err.c_str(), req.isKeepAlive());
				return true;
			}
		}
		add_runtime_result_json(root, runtime_task, found ? &persisted : NULL);
	} else if (reviewable_agent_run_status(record.status)) {
		webcool::ai::agent_result_store_t result_store(user_root,
			record.project_path);
		webcool::ai::agent_result_t persisted;
		bool found = false;
		if (!result_store.load(record.id, persisted, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (found) {
			add_persisted_result_json(root, persisted,
				result_store.relative_path(record.id));
		} else {
			root.add_bool("result_available", false);
			root.add_bool("result_persisted", false);
		}
		if (record.status == "failed" || record.status == "cancelled") {
			add_durable_recovery_json(root, user_root, record, session_hint);
		}
	} else {
		add_runtime_result_json(root, runtime_task);
		add_durable_recovery_json(root, user_root, record, session_hint);
	}
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunReasoningSaveAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_user_root(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string run_id = json_text((*body)["run_id"]);
	const std::string session_id = json_text((*body)["session_id"]);
	webcool::ai::agent_run_store_t run_store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!run_store.get(run_id, record, err)) {
		json_error(res, err == "agent run not found" ? 404 : 400,
			err.c_str(), req.isKeepAlive());
		return true;
	}
	std::string reasoning;
	const std::shared_ptr<agent_runtime_task_t> runtime_task =
		find_runtime_task(user_root, run_id);
	if (runtime_task) reasoning = runtime_reasoning_for_save(runtime_task);
	if (reasoning.empty() && record.status == "completed") {
		// Completed runtime entries expire after a bounded retention period. Load
		// the durable project result so saving still works after service restart.
		webcool::ai::agent_result_store_t result_store(user_root,
			record.project_path);
		webcool::ai::agent_result_t result;
		bool found = false;
		if (!result_store.load(run_id, result, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (found) reasoning = result.reasoning;
	}
	if (reasoning.empty() && !session_id.empty()) {
		// Failed/cancelled runtimes may have no result artifact, but their bounded
		// reasoning is retained with the private session transcript. Require both
		// the run id and project path to match before using that fallback.
		webcool::ai::agent_session_store_t session_store(user_root);
		webcool::ai::agent_session_record_t session;
		if (!session_store.get(session_id, session, err)) {
			json_error(res, err == "agent session not found" ? 404 : 400,
				err.c_str(), req.isKeepAlive());
			return true;
		}
		if (session.project_path != record.project_path) {
			err = "AI run does not belong to the selected session project";
			webcool::ai::ai_log_error("agent.reasoning",
				"validate-session-project", err);
			json_error(res, 409, err.c_str(), req.isKeepAlive());
			return true;
		}
		for (size_t i = 0; i < session.messages.size(); ++i) {
			if (session.messages[i].role == "assistant"
				&& session.messages[i].run_id == run_id)
			{
				reasoning = session.messages[i].reasoning;
				break;
			}
		}
	}
	if (reasoning.empty()) {
		err = "this AI run has no reasoning text to save";
		webcool::ai::ai_log_error("agent.reasoning", "save-empty", err);
		json_error(res, 409, err.c_str(), req.isKeepAlive());
		return true;
	}
	const std::string filename = "ai-reasoning-" + run_id + ".txt";
	const std::string relative_path = record.project_path.empty()
		? filename : record.project_path + "/" + filename;
	webcool::ai::agent_workspace_t workspace(user_root);
	if (!workspace.save_generated_text(relative_path, reasoning, err)) {
		json_error(res, 500, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (runtime_task) {
		acl::json saved_json;
		acl::json_node& saved_event = saved_json.create_node();
		saved_event.add_text("event", "reasoning_log_saved");
		saved_event.add_text("path", relative_path.c_str());
		saved_event.add_number("bytes",
			static_cast<long long>(reasoning.size()));
		append_runtime_operation_event(runtime_task, saved_event);
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", run_id.c_str());
	root.add_text("path", relative_path.c_str());
	root.add_text("operation_log_path", (record.project_path.empty()
		? ".webcool_agent/ai-operations-" + run_id + ".jsonl"
		: record.project_path + "/.webcool_agent/ai-operations-" + run_id + ".jsonl").c_str());
	root.add_number("bytes", static_cast<long long>(reasoning.size()));
	return sendJson(res, 200, root, req.isKeepAlive());
}

bool AiAgentRunEventsAction::run(request_t& req, response_t& res) {
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username)) return true;
	const char* raw_id = req.getParameter("id");
	const std::string id = raw_id ? raw_id : "";
	const char* raw_session_id = req.getParameter("session_id");
	const std::string session_hint = raw_session_id ? raw_session_id : "";
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		int status = 500;
		if (err == "invalid agent run id") status = 400;
		else if (err == "agent run not found") status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	std::shared_ptr<agent_runtime_task_t> runtime_task =
		find_runtime_task(user_root, id);
	if (record.status == "running" && !runtime_task) {
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
			runtime_upload_dir_get(), user_root, username);
		if (checkpoint_store.exists(id)) {
			runtime_task = recover_runtime_task(runtime_upload_dir_get(),
				user_root, username, record, err);
			if (!runtime_task) {
				json_error(res, 503, err.empty()
					? "agent restart recovery is temporarily unavailable"
					: err.c_str(), req.isKeepAlive());
				return true;
			}
		} else {
			std::string update_err;
			if (!store.fail(id, "agent run interrupted by service restart",
				update_err) || !store.get(id, record, update_err))
			{
				json_error(res, 500, update_err.c_str(), req.isKeepAlive());
				return true;
			}
		}
	}
	runtime_subscription_guard_t subscription(runtime_task);
	if (!subscription.acquired()) {
		json_error(res, 429, "too many event subscribers for this agent run",
			req.isKeepAlive());
		return true;
	}

	res.setStatus(200);
	res.setContentType("text/event-stream; charset=utf-8");
	res.setHeader("Cache-Control", "no-cache, no-store");
	res.setHeader("X-Accel-Buffering", "no");
	res.setChunkedTransferEncoding(true);
	res.setKeepAlive(false);
	const char* retry = "retry: 2000\n\n";
	if (!res.write(retry, strlen(retry))) return true;

	unsigned long long last_version = static_cast<unsigned long long>(-1);
	unsigned long long last_staged_version =
		static_cast<unsigned long long>(-1);
	size_t heartbeat_ticks = 0;
	for (;;) {
		const unsigned long long version = runtime_event_version(runtime_task);
		if (version != last_version) {
			if (!store.get(id, record, err)) break;
			const unsigned long long staged_version =
				runtime_staged_change_version(runtime_task);
			acl::json json;
			acl::json_node& root = json.create_node();
			root.add_bool("ok", true);
			add_run_record_json(root, record);
			// File bodies, originals and diffs can total several megabytes. They
			// change only with staged_change_version, so do not resend them for
			// every reasoning/text token snapshot.
			add_runtime_result_json(root, runtime_task, NULL,
				record.status != "running"
				|| staged_version != last_staged_version);
			if (!runtime_task) {
				add_durable_recovery_json(root, user_root, record, session_hint);
			}
			const std::string data = serialize_json(root);
			std::ostringstream frame;
			frame << "id: " << version << "\n"
				<< "event: "
				<< (record.status == "running" ? "snapshot" : "complete")
				<< "\ndata: " << data << "\n\n";
			const std::string payload = frame.str();
			if (!res.write(payload.data(), payload.size())) return true;
			last_version = version;
			last_staged_version = staged_version;
			heartbeat_ticks = 0;
			if (record.status != "running") break;
		}
		acl::fiber::delay(250);
		if (++heartbeat_ticks >= 60) {
			const char* heartbeat = ": heartbeat\n\n";
			if (!res.write(heartbeat, strlen(heartbeat))) return true;
			heartbeat_ticks = 0;
		}
	}
	(void) res.write(NULL, 0);
	return true;
}

bool AiAgentRunCancelAction::run(request_t& req, response_t& res) {
	std::string user_root;
	std::string username;
	if (!current_user_root(req, res, user_root, &username)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	// Explicitly abandon a suspended session without discarding its review ledger.
	const std::string recovery_session = json_text((*body)["cancel_recovery_session_id"]);
	if (!recovery_session.empty()) {
		std::string err;
		webcool::ai::agent_session_record_t session;
		if (!webcool::ai::agent_session_store_t(user_root).get(recovery_session, session, err)) {
			json_error(res, 404, err.c_str(), req.isKeepAlive()); return true;
		}
		std::vector<webcool::ai::agent_run_record_t> runs;
		if (!webcool::ai::agent_run_store_t(user_root).list(0, runs, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive()); return true;
		}
		for (const auto& run : runs) if (run.project_path == session.project_path && run.status == "running") {
			json_error(res, 409, "project has a running task; stop it before cancelling recovery", req.isKeepAlive()); return true;
		}
		webcool::ai::agent_progress_store_t progress_store(user_root, session.project_path, session.id);
		webcool::ai::agent_progress_t progress; bool found = false;
		if (!progress_store.load(progress, found, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive()); return true;
		}
		webcool::ai::agent_checkpoint_store_t checkpoints(runtime_upload_dir_get(), user_root, username);
		for (const auto& run_id : {session.last_run_id, progress.source_run_id}) {
			if (run_id.empty()) continue;
			if (!checkpoints.remove(run_id, err)) {
				json_error(res, 500, err.c_str(), req.isKeepAlive()); return true;
			}
		}
		if (!progress_store.remove(err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive()); return true;
		}
		{
			std::lock_guard<webcool::mutex> guard(g_agent_runtime_mutex);
			for (auto& item : g_agent_runtime_tasks) {
				if (item.second->user_root == user_root && item.second->session_id == session.id)
					item.second->recovery_available = false;
			}
		}
		acl::json json; auto& root = json.create_node();
		root.add_bool("ok", true); root.add_bool("recovery_available", false);
		root.add_text("session_id", session.id.c_str());
		return sendJson(res, 200, root, req.isKeepAlive());
	}
	const std::string id = json_text((*body)["run_id"]);
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		int status = 500;
		if (err == "invalid agent run id") status = 400;
		else if (err == "agent run not found") status = 404;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (record.status != "running") {
		json_error(res, 409, "agent run is already finished", req.isKeepAlive());
		return true;
	}
	bool already_done = false;
	const bool in_memory = request_runtime_cancel(user_root, id, already_done);
	if (already_done) {
		json_error(res, 409, "agent run is already finished", req.isKeepAlive());
		return true;
	}
	if (!in_memory) {
		// A running record without an in-memory task came from an interrupted
		// service process. Remove its encrypted recovery material before closing
		// the audit record so cancellation cannot restart it later.
		webcool::ai::agent_checkpoint_store_t checkpoint_store(
			runtime_upload_dir_get(), user_root, username);
		if (!checkpoint_store.remove(id, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
		if (!store.cancel(id, err)) {
			json_error(res, 500, err.c_str(), req.isKeepAlive());
			return true;
		}
	}
	if (in_memory) {
		const std::shared_ptr<agent_runtime_task_t> runtime_task =
			find_runtime_task(user_root, id);
		append_simple_operation_event(runtime_task, "cancel_requested",
			"cancelling", "", runtime_completed_tool_count(runtime_task));
	}
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("cancel_requested", in_memory);
	root.add_text("status", in_memory ? "running" : "cancelled");
	return sendJson(res, in_memory ? 202 : 200, root, req.isKeepAlive());
}

bool AiAgentRunPauseAction::run(request_t& req, response_t& res) {
	std::string user_root;
	if (!current_user_root(req, res, user_root)) return true;
	acl::json* body = req.getJson(64 * 1024);
	if (body == NULL) {
		json_error(res, 400, "invalid JSON body", req.isKeepAlive());
		return true;
	}
	const std::string id = json_text((*body)["run_id"]);
	const acl::json_node* paused_node = (*body)["paused"];
	const bool* paused_value = paused_node == NULL ? NULL : paused_node->get_bool();
	if (paused_value == NULL) {
		json_error(res, 400, "paused must be a boolean", req.isKeepAlive());
		return true;
	}
	const bool paused = *paused_value;
	webcool::ai::agent_run_store_t store(user_root);
	webcool::ai::agent_run_record_t record;
	std::string err;
	if (!store.get(id, record, err)) {
		const int status = err == "invalid agent run id" ? 400
			: err == "agent run not found" ? 404 : 500;
		json_error(res, status, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (record.status != "running") {
		json_error(res, 409, "agent run is already finished", req.isKeepAlive());
		return true;
	}
	bool already_done = false;
	bool pause_requested = false;
	if (!request_runtime_pause(user_root, id, paused, already_done,
		pause_requested))
	{
		err = "agent run cannot be paused because its live worker is unavailable";
		webcool::ai::ai_log_error("agent.runtime", "pause-worker-missing", err);
		json_error(res, 409, err.c_str(), req.isKeepAlive());
		return true;
	}
	if (already_done) {
		json_error(res, 409, "agent run is already finished", req.isKeepAlive());
		return true;
	}
	const std::shared_ptr<agent_runtime_task_t> runtime_task =
		find_runtime_task(user_root, id);
	append_simple_operation_event(runtime_task,
		paused ? "pause_requested" : "resume_requested",
		pause_requested ? "pausing" : "resuming", "",
		runtime_completed_tool_count(runtime_task));
	acl::json json;
	acl::json_node& root = json.create_node();
	root.add_bool("ok", true);
	root.add_text("run_id", id.c_str());
	root.add_bool("pause_requested", pause_requested);
	root.add_text("phase", pause_requested ? "pausing" : "resuming");
	return sendJson(res, 202, root, req.isKeepAlive());
}

} // namespace action
