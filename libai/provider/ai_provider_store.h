#pragma once

#include <string>
#include <vector>

namespace webcool
{
namespace ai
{

// User provider settings with credential metadata and protocol controls.
struct provider_config_t {
	// Trusted store-owned path; never accepted from HTTP provider settings.
	std::string output_limit_cache_directory;
	// Per-run override; deliberately not persisted in provider settings.
	std::string response_state_mode = "auto";
	// Identifier used to look up this record.
	std::string id;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Wire protocol used to communicate with the provider.
	std::string protocol;
	// Configured base URL for the provider's HTTP API.
	std::string base_url;
	// Provider model name used for generation.
	std::string model;
	// AES-256-GCM ciphertext bound to the user and provider ID. It is never
	// returned by the HTTP actions; only the non-secret hint is exposed.
	std::string api_key_ciphertext;
	// Masked credential hint suitable for display.
	std::string api_key_hint;
	// Whether this capability is enabled by its configuration.
	bool enabled = true;
	// Whether source file contents may be sent to the provider.
	bool allow_file_content = false;
	// Whether this provider is the user's default selection.
	bool is_default = false;
	// Responses-specific controls are persisted per user/provider. Defaults keep
	// existing records compatible while making privacy and performance choices
	// explicit instead of relying on changing service defaults.
	bool responses_store = true;
	// Whether Responses requests use background execution.
	bool responses_background = false;
	// Whether Responses context compaction is enabled.
	bool responses_compact = true;
	// Whether Responses tool schemas request strict validation.
	bool responses_strict_tools = true;
	// Minimum configured reasoning effort for Responses.
	std::string responses_min_reasoning_effort = "auto";
	// Configured Responses reasoning-summary format.
	std::string responses_reasoning_summary = "auto";
	// Configured Responses output verbosity.
	std::string responses_text_verbosity = "medium";
	// Configured Responses service tier.
	std::string responses_service_tier = "auto";
	// Configured retention setting for Responses prompt caching.
	std::string responses_cache_ttl = "30m";
	// Qwen's optional five-minute Responses session cache has a cache-creation
	// surcharge, so it is an explicit opt-in rather than a silent optimization.
	bool qwen_session_cache = false;
	// Optional OpenAI routing headers. They are useful when one API key can
	// access several organizations or projects, and are never treated as auth
	// secrets in browser responses or logs.
	std::string openai_organization;
	// Optional OpenAI project header value.
	std::string openai_project;
	// Connection-test telemetry is stored separately from the encrypted static
	// configuration. It contains no prompt, response body, or credential.
	std::string last_test_status;
	// Last connection-test time as seconds since the Unix epoch.
	long long last_test_at = 0;
	// HTTP status observed during the most recent connection test.
	int last_test_http_status = 0;
	// Duration of the most recent connection test in milliseconds.
	long long last_test_latency_ms = 0;
	// Diagnostic from the most recent connection test.
	std::string last_test_error;
};

// Browser input used for validation and encryption. api_key exists only during
// the current save/test operation and must never be logged.
struct provider_input_t {
	// Identifier used to look up this record.
	std::string id;
	// Name used to identify this item in its containing collection.
	std::string name;
	// Wire protocol used to communicate with the provider.
	std::string protocol;
	// Configured base URL for the provider's HTTP API.
	std::string base_url;
	// Provider model name used for generation.
	std::string model;
	// Provider credential; do not include it in logs or UI responses.
	std::string api_key;
	// Whether this capability is enabled by its configuration.
	bool enabled = true;
	// Whether source file contents may be sent to the provider.
	bool allow_file_content = false;
	// Whether this provider is the user's default selection.
	bool is_default = false;
	// Whether Responses requests permit provider-side retention.
	bool responses_store = true;
	// Whether Responses requests use background execution.
	bool responses_background = false;
	// Whether Responses context compaction is enabled.
	bool responses_compact = true;
	// Whether Responses tool schemas request strict validation.
	bool responses_strict_tools = true;
	// Minimum configured reasoning effort for Responses.
	std::string responses_min_reasoning_effort = "auto";
	// Configured Responses reasoning-summary format.
	std::string responses_reasoning_summary = "auto";
	// Configured Responses output verbosity.
	std::string responses_text_verbosity = "medium";
	// Configured Responses service tier.
	std::string responses_service_tier = "auto";
	// Configured retention setting for Responses prompt caching.
	std::string responses_cache_ttl = "30m";
	// Whether the Qwen session cache optimization is enabled.
	bool qwen_session_cache = false;
	// Optional OpenAI organization header value.
	std::string openai_organization;
	// Optional OpenAI project header value.
	std::string openai_project;
	// Whether saving the configuration removes its existing credential.
	bool clear_api_key = false;
};

// Summary counters for installation-wide credential key rotation.
struct master_key_rotation_result_t {
	// Number of user stores inspected during key rotation.
	long long users_scanned = 0;
	// Number of provider credentials rewritten with the new key.
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
	// Bind the provider store to the supplied storage scope.
	provider_store_t(const std::string &upload_root,
	    const std::string &user_root, const std::string &username);

	// Read the stored records in this scope; report failures through err.
	bool list(
	    std::vector<provider_config_t> &providers, std::string &err) const;
	// Persist the supplied record; report failures through err.
	bool save(const provider_input_t &input, provider_config_t &saved,
	    std::string &err);
	// Remove the identified saved record; report failures through err.
	bool remove(const std::string &id, std::string &err);
	// Decrypt the selected provider's credential for an authorized
	// request.
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
	// Authenticate and decrypt data bound to the specified user and
	// purpose.
	static bool open_user_data(const std::string &upload_root,
	    const std::string &username, const std::string &binding,
	    const std::string &encoded, std::string &plaintext,
	    std::string &err);

	// Validate provider settings and endpoint security before
	// persistence.
	static bool validate_input(const provider_input_t &input,
	    bool allow_insecure_remote, std::string &err);

private:
	// Root of the installation's uploaded and per-user data.
	std::string upload_root_;
	// Filesystem root belonging to the authenticated user.
	std::string user_root_;
	// Authenticated account name associated with this object.
	std::string username_;
};

} // namespace ai
} // namespace webcool
