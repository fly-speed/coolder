#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

struct provider_config_t {
	// Trusted store-owned path; never accepted from HTTP provider settings.
	std::string output_limit_cache_directory;
	// Per-run override; deliberately not persisted in provider settings.
	std::string response_state_mode = "auto";
	std::string id;
	std::string name;
	std::string protocol;
	std::string base_url;
	std::string model;
	// AES-256-GCM ciphertext bound to the user and provider ID. It is never
	// returned by the HTTP actions; only the non-secret hint is exposed.
	std::string api_key_ciphertext;
	std::string api_key_hint;
	bool enabled = true;
	bool allow_file_content = false;
	bool is_default = false;
	// Responses-specific controls are persisted per user/provider. Defaults keep
	// existing records compatible while making privacy and performance choices
	// explicit instead of relying on changing service defaults.
	bool responses_store = true;
	bool responses_background = false;
	bool responses_compact = true;
	bool responses_strict_tools = true;
	std::string responses_min_reasoning_effort = "auto";
	std::string responses_reasoning_summary = "auto";
	std::string responses_text_verbosity = "medium";
	std::string responses_service_tier = "auto";
	std::string responses_cache_ttl = "30m";
	// Qwen's optional five-minute Responses session cache has a cache-creation
	// surcharge, so it is an explicit opt-in rather than a silent optimization.
	bool qwen_session_cache = false;
	// Optional OpenAI routing headers. They are useful when one API key can
	// access several organizations or projects, and are never treated as auth
	// secrets in browser responses or logs.
	std::string openai_organization;
	std::string openai_project;
	// Connection-test telemetry is stored separately from the encrypted static
	// configuration. It contains no prompt, response body, or credential.
	std::string last_test_status;
	long long last_test_at = 0;
	int last_test_http_status = 0;
	long long last_test_latency_ms = 0;
	std::string last_test_error;
};

// Browser input used for validation and encryption. api_key exists only during
// the current save/test operation and must never be logged.
struct provider_input_t {
	std::string id;
	std::string name;
	std::string protocol;
	std::string base_url;
	std::string model;
	std::string api_key;
	bool enabled = true;
	bool allow_file_content = false;
	bool is_default = false;
	bool responses_store = true;
	bool responses_background = false;
	bool responses_compact = true;
	bool responses_strict_tools = true;
	std::string responses_min_reasoning_effort = "auto";
	std::string responses_reasoning_summary = "auto";
	std::string responses_text_verbosity = "medium";
	std::string responses_service_tier = "auto";
	std::string responses_cache_ttl = "30m";
	bool qwen_session_cache = false;
	std::string openai_organization;
	std::string openai_project;
	bool clear_api_key = false;
};

struct master_key_rotation_result_t {
	long long users_scanned = 0;
	long long providers_reencrypted = 0;
	// True when this invocation safely resumed a previously interrupted
	// prepared/committed rotation instead of starting a new one.
	bool recovered_interrupted_rotation = false;
};

// Encrypted provider database stored under its owning account directory. The
// HTTP layer uses the administrator account as the installation-wide owner;
// the installation master key remains under upload_root/webcool_auth.
class provider_store_t {
public:
	provider_store_t(const std::string &upload_root,
	    const std::string &user_root, const std::string &username);

	bool list(
	    std::vector<provider_config_t> &providers, std::string &err) const;
	bool save(const provider_input_t &input, provider_config_t &saved,
	    std::string &err);
	bool remove(const std::string &id, std::string &err);
	bool reveal_api_key(const provider_config_t &provider,
	    std::string &api_key, std::string &err) const;
	// Persist the latest connection-test outcome in this user's own directory.
	// error_summary is sanitized and bounded before it reaches disk.
	bool record_test_result(const std::string &id, bool succeeded,
	    int http_status, long long latency_ms,
	    const std::string &error_summary, std::string &err);

	// Rotate the installation key for every providers.v1 file under the normal
	// multi-user storage root. A dual-key journal keeps mixed old/new records
	// readable if the process or machine stops during rotation.
	static bool rotate_master_key(const std::string &upload_root,
	    master_key_rotation_result_t &result, std::string &err);

	// Reuse the installation keyring for other narrowly scoped per-user secret
	// state such as restart checkpoints. binding becomes authenticated data, so
	// ciphertext cannot be moved between users, runs or data types.
	static bool seal_user_data(const std::string &upload_root,
	    const std::string &username, const std::string &binding,
	    const std::string &plaintext, std::string &encoded,
	    std::string &err);
	static bool open_user_data(const std::string &upload_root,
	    const std::string &username, const std::string &binding,
	    const std::string &encoded, std::string &plaintext,
	    std::string &err);

	static bool validate_input(const provider_input_t &input,
	    bool allow_insecure_remote, std::string &err);

private:
	std::string upload_root_;
	std::string user_root_;
	std::string username_;
};

} // namespace ai
} // namespace webcool
