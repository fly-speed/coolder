#include "stdafx.h"
#include "ai_provider_store_internal.h"
#include "../common/file_ops.h"
#include "../common/identifiers.h"
#include "../common/record_codec.h"
#include "ai_provider_store.h"
#include "../common/ai_error_log.h"
#include "storage_paths.h"
#ifdef _WIN32
#include "../common/platform_compat.h"
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include "../common/webcool_mutex.h"

namespace webcool
{
namespace ai
{
namespace provider_store_detail
{

// providers.v1 contains metadata plus AES-256-GCM ciphertext. The 32-byte
// installation master key is separate from every user's provider file, and
// additional authenticated data binds ciphertext to username + provider ID.
webcool::mutex g_provider_store_mutex;
const char *kAgentDir = ".webcool_agent";
const char *kProviderFile = "providers.v1";
const char *kProviderHealthFile = "provider_health.v1";
const char *kMasterKeyFile = "webcool_auth/ai_master.key";
const char *kHeader = "WEBCOOL_AI_PROVIDERS_V1";
const char *kHealthHeader = "WEBCOOL_AI_PROVIDER_HEALTH_V1";
const char *kMasterKeyHeader = "WEBCOOL_AI_MASTER_KEY_V2";

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

std::string lower_ascii(std::string value)
{
	for (size_t i = 0; i < value.size(); ++i) {
		value[i] = static_cast<char>(
		    ::tolower(static_cast<unsigned char>(value[i])));
	}
	return value;
}

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

std::string agent_dir(const std::string &user_root)
{
	return storage_detail::join_path(user_root, kAgentDir);
}

std::string provider_file(const std::string &user_root)
{
	return storage_detail::join_path(agent_dir(user_root), kProviderFile);
}

std::string provider_health_file(const std::string &user_root)
{
	return storage_detail::join_path(
	    agent_dir(user_root), kProviderHealthFile);
}

bool protocol_supported(const std::string &protocol)
{
	return protocol == "openai_images" || protocol == "openai_responses" ||
	    protocol == "openai_chat" || protocol == "openai_compatible" ||
	    protocol == "ollama" || protocol == "anthropic_messages" ||
	    protocol == "gemini_native";
}

bool parse_base_url(const std::string &url, bool &https, std::string &hostname,
    std::string &err)
{
	const std::string lower = lower_ascii(url);
	size_t begin = 0;
	if (lower.compare(0, 8, "https://") == 0) {
		https = true;
		begin = 8;
	} else if (lower.compare(0, 7, "http://") == 0) {
		https = false;
		begin = 7;
	} else {
		err = "AI provider URL must use HTTP or HTTPS";
		return false;
	}
	const size_t slash = url.find('/', begin);
	std::string authority = url.substr(begin,
	    slash == std::string::npos ? std::string::npos : slash - begin);
	if (authority.empty() || authority.find('@') != std::string::npos ||
	    authority.find_first_of(" \t\r\n?#") != std::string::npos) {
		err = "AI provider URL has an invalid host";
		return false;
	}
	if (authority[0] == '[') {
		const size_t close = authority.find(']');
		if (close == std::string::npos) {
			err = "AI provider URL has an invalid IPv6 host";
			return false;
		}
		hostname = lower_ascii(authority.substr(1, close - 1));
	} else {
		const size_t colon = authority.find(':');
		hostname = lower_ascii(authority.substr(0, colon));
	}
	if (!hostname.empty())
		return true;
	err = "AI provider URL has an invalid host";
	return false;
}

bool is_loopback_host(const std::string &host)
{
	return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

}
using namespace provider_store_detail;
// namespace

provider_store_t::provider_store_t(const std::string &upload_root,
    const std::string &user_root, const std::string &username)
        : upload_root_(upload_root)
        , user_root_(user_root)
        , username_(username)
{
}

bool provider_store_t::list(
    std::vector<provider_config_t> &providers, std::string &err) const
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "list", err);
	}
	if (load_health_unlocked(user_root_, providers, err))
		return true;
	return ai_error("provider.store", "list-health", err);
}

bool provider_store_t::save(
    const provider_input_t &input, provider_config_t &saved, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-save", err);
	}
	provider_config_t *target = NULL;
	if (!input.id.empty()) {
		if (!valid_id(input.id)) {
			err = "invalid AI provider id";
			return ai_error(
			    "provider.store", "validate-save-id", err);
		}
		for (size_t i = 0; i < providers.size(); ++i) {
			if (!(providers[i].id == input.id))
				continue;
			target = &providers[i];
		}
		if (target == NULL) {
			err = "AI provider not found";
			return ai_error("provider.store", "find-for-save", err);
		}
	} else {
		provider_config_t item;
		item.id = new_id();
		if (item.id.empty()) {
			err = "cannot generate AI provider id";
			return ai_error("provider.store", "generate-id", err);
		}
		providers.push_back(item);
		target = &providers.back();
	}
	target->name = input.name;
	target->protocol = input.protocol;
	target->base_url = input.base_url;
	target->model = input.model;
	target->enabled = input.enabled;
	target->allow_file_content = input.allow_file_content;
	target->is_default = input.is_default;
	target->responses_store = input.responses_store;
	target->responses_background = input.responses_background;
	target->responses_compact = input.responses_compact;
	target->responses_strict_tools = input.responses_strict_tools;
	target->responses_min_reasoning_effort =
	    input.responses_min_reasoning_effort;
	target->responses_reasoning_summary = input.responses_reasoning_summary;
	target->responses_text_verbosity = input.responses_text_verbosity;
	target->responses_service_tier = input.responses_service_tier;
	target->responses_cache_ttl = input.responses_cache_ttl;
	target->qwen_session_cache = input.qwen_session_cache;
	target->openai_organization = input.openai_organization;
	target->openai_project = input.openai_project;
	if (input.clear_api_key) {
		target->api_key_ciphertext.clear();
		target->api_key_hint.clear();
	} else if (!input.api_key.empty()) {
		// Plaintext exists only for this call. Encryption happens before the
		// provider record is serialized, and no logging call receives the key.
		if (!encrypt_secret(upload_root_, username_ + ":" + target->id,
		        input.api_key, target->api_key_ciphertext, err)) {
			return ai_error(
			    "provider.store", "encrypt-api-key", err);
		}
		const size_t hint_len =
		    std::min<size_t>(4, input.api_key.size());
		target->api_key_hint =
		    input.api_key.substr(input.api_key.size() - hint_len);
	}
	if (target->is_default) {
		for (size_t i = 0; i < providers.size(); ++i) {
			if (!(&providers[i] != target))
				continue;
			providers[i].is_default = false;
		}
	}
	if (!save_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save", err);
	}
	saved = *target;
	return true;
}

bool provider_store_t::remove(const std::string &id, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!valid_id(id)) {
		err = "invalid AI provider id";
		return ai_error("provider.store", "validate-remove-id", err);
	}
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-remove", err);
	}
	if (!load_health_unlocked(user_root_, providers, err)) {
		return ai_error(
		    "provider.store", "load-health-for-remove", err);
	}
	const size_t before = providers.size();
	providers.erase(
	    std::remove_if(providers.begin(), providers.end(),
	        [&id](const provider_config_t &item) { return item.id == id; }),
	    providers.end());
	if (providers.size() == before) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-remove", err);
	}
	if (!save_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "save-remove", err);
	}
	// Rewrite the small telemetry file as well so deleting a provider does not
	// leave its diagnostic history behind in the user's directory.
	if (save_health_unlocked(user_root_, providers, err))
		return true;
	return ai_error("provider.store", "save-health-remove", err);
}

bool provider_store_t::reveal_api_key(const provider_config_t &provider,
    std::string &api_key, std::string &err) const
{
	// Rotation changes the key journal and provider files as one logical unit.
	// Serialize decryptions with it so a request cannot observe a half-written
	// cross-file transition.
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "reload-for-decrypt", err);
	}
	const provider_config_t *current = NULL;
	for (size_t i = 0; i < providers.size(); ++i) {
		if (!(providers[i].id == provider.id))
			continue;
		current = &providers[i];
		break;
	}
	if (current == NULL) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-decrypt", err);
	}
	if (decrypt_secret(upload_root_, username_ + ":" + current->id,
	        current->api_key_ciphertext, api_key, err))
		return true;
	return ai_error("provider.store", "decrypt-api-key", err);
}

static void cleanse_rotation_plaintext(std::string &plaintext)
{
	if (!plaintext.empty())
		OPENSSL_cleanse(&plaintext[0], plaintext.size());
}

bool provider_store_t::rotate_master_key(const std::string &upload_root,
    master_key_rotation_result_t &result, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	result = master_key_rotation_result_t();
	master_keyring_t keyring;
	if (!read_master_keyring(upload_root, keyring, err)) {
		return ai_error("provider.master-key", "load", err);
	}
	result.recovered_interrupted_rotation = !keyring.phase.empty();
	// Enumerate before creating the dual-key journal. This avoids beginning a
	// new rotation when an encrypted agent checkpoint still depends on the
	// current active key. An already prepared journal remains readable and will
	// resume after the recoverable runs have been closed.
	std::vector<rotation_user_t> users;
	if (!collect_rotation_users(upload_root, users, err)) {
		return ai_error("provider.master-key", "enumerate-users", err);
	}

	std::vector<unsigned char> target_key;
	if (keyring.phase == "prepared") {
		target_key = keyring.secondary;
	} else if (keyring.phase == "committed") {
		target_key = keyring.active;
	} else {
		target_key.assign(32, 0);
		if (RAND_bytes(&target_key[0],
		        static_cast<int>(target_key.size())) != 1) {
			err = "cannot generate replacement AI master key";
			return ai_error("provider.master-key", "generate", err);
		}
		// Installing the dual-key journal before touching provider files makes
		// every subsequent mixed state readable and safely resumable.
		if (!write_master_key_file(upload_root, keyring.active,
		        target_key, "prepared", err)) {
			return ai_error(
			    "provider.master-key", "prepare-journal", err);
		}
		keyring.secondary = target_key;
		keyring.phase = "prepared";
	}
	cleanse_vector_t target_guard(target_key);

	for (size_t i = 0; i < users.size(); ++i) {
		std::vector<provider_config_t> providers;
		if (!load_unlocked(users[i].user_root, providers, err)) {
			return ai_error(
			    "provider.master-key", "load-provider-file", err);
		}
		++result.users_scanned;
		bool changed = false;
		for (size_t j = 0; j < providers.size(); ++j) {
			provider_config_t &provider = providers[j];
			if (provider.api_key_ciphertext.empty())
				continue;
			std::string plaintext;
			std::string decrypt_err;
			const std::string aad =
			    users[i].username + ":" + provider.id;
			bool decrypted = decrypt_secret_with_key(keyring.active,
			    aad, provider.api_key_ciphertext, plaintext,
			    decrypt_err);
			if (!decrypted && !keyring.secondary.empty()) {
				decrypted =
				    decrypt_secret_with_key(keyring.secondary,
				        aad, provider.api_key_ciphertext,
				        plaintext, decrypt_err);
			}
			if (!decrypted) {
				cleanse_rotation_plaintext(plaintext);
				err =
				    "cannot decrypt an AI provider during master key rotation";
				return ai_error("provider.master-key",
				    "decrypt-provider", err);
			}
			std::string rotated;
			const bool encrypted = encrypt_secret_with_key(
			    target_key, aad, plaintext, rotated, err);
			if (!plaintext.empty())
				OPENSSL_cleanse(
				    &plaintext[0], plaintext.size());
			if (!encrypted) {
				return ai_error("provider.master-key",
				    "reencrypt-provider", err);
			}
			provider.api_key_ciphertext.swap(rotated);
			changed = true;
			++result.providers_reencrypted;
		}
		if (!(changed &&
		        !save_unlocked(users[i].user_root, providers, err)))
			continue;
		return ai_error(
		    "provider.master-key", "save-provider-file", err);
	}

	// All provider files now use target_key. Mark it active while retaining
	// the old key until the explicit verification pass below completes.
	if ((keyring.phase == "prepared") &&
	    (!write_master_key_file(
	        upload_root, target_key, keyring.active, "committed", err))) {
		return ai_error("provider.master-key", "commit-journal", err);
	}

	for (size_t i = 0; i < users.size(); ++i) {
		std::vector<provider_config_t> providers;
		if (!load_unlocked(users[i].user_root, providers, err)) {
			return ai_error(
			    "provider.master-key", "verify-load", err);
		}
		for (size_t j = 0; j < providers.size(); ++j) {
			if (providers[j].api_key_ciphertext.empty())
				continue;
			std::string plaintext;
			const std::string aad =
			    users[i].username + ":" + providers[j].id;
			const bool verified = decrypt_secret_with_key(
			    target_key, aad, providers[j].api_key_ciphertext,
			    plaintext, err);
			if (!plaintext.empty())
				OPENSSL_cleanse(
				    &plaintext[0], plaintext.size());
			if (verified)
				continue;
			err =
			    "AI provider verification failed after master key rotation";
			return ai_error(
			    "provider.master-key", "verify-provider", err);
		}
	}

	// Only after every ciphertext verifies under the replacement key do we
	// remove the old key from disk and finish the rotation.
	std::vector<unsigned char> no_secondary;
	if (write_master_key_file(
	        upload_root, target_key, no_secondary, "", err))
		return true;
	return ai_error("provider.master-key", "retire-old-key", err);
}

bool provider_store_t::seal_user_data(const std::string &upload_root,
    const std::string &username, const std::string &binding,
    const std::string &plaintext, std::string &encoded, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (username.empty() || binding.empty()) {
		err = "invalid AI user data encryption binding";
		return ai_error("provider.user-data", "validate-seal", err);
	}
	if (encrypt_secret(upload_root, "user-data:" + username + ":" + binding,
	        plaintext, encoded, err))
		return true;
	return ai_error("provider.user-data", "seal", err);
}

bool provider_store_t::open_user_data(const std::string &upload_root,
    const std::string &username, const std::string &binding,
    const std::string &encoded, std::string &plaintext, std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (username.empty() || binding.empty()) {
		err = "invalid AI user data encryption binding";
		return ai_error("provider.user-data", "validate-open", err);
	}
	if (decrypt_secret(upload_root, "user-data:" + username + ":" + binding,
	        encoded, plaintext, err))
		return true;
	return ai_error("provider.user-data", "open", err);
}

bool provider_store_t::record_test_result(const std::string &id, bool succeeded,
    int http_status, long long latency_ms, const std::string &error_summary,
    std::string &err)
{
	std::lock_guard<webcool::mutex> guard(g_provider_store_mutex);
	if (!valid_id(id)) {
		err = "invalid AI provider id";
		return ai_error(
		    "provider.store", "validate-test-result-id", err);
	}
	std::vector<provider_config_t> providers;
	if (!load_unlocked(user_root_, providers, err)) {
		return ai_error("provider.store", "load-for-test-result", err);
	}
	if (!load_health_unlocked(user_root_, providers, err)) {
		return ai_error(
		    "provider.store", "load-health-for-test-result", err);
	}
	provider_config_t *target = NULL;
	for (size_t i = 0; i < providers.size(); ++i) {
		if (!(providers[i].id == id))
			continue;
		target = &providers[i];
		break;
	}
	if (target == NULL) {
		err = "AI provider not found";
		return ai_error("provider.store", "find-for-test-result", err);
	}
	target->last_test_status = succeeded ? "ok" : "error";
	target->last_test_at = static_cast<long long>(std::time(NULL));
	target->last_test_http_status = http_status < 0 ? 0 : http_status;
	target->last_test_latency_ms = latency_ms < 0 ? 0 : latency_ms;
	target->last_test_error =
	    succeeded ? "" : sanitize_test_error(error_summary);
	if (save_health_unlocked(user_root_, providers, err))
		return true;
	return ai_error("provider.store", "save-test-result", err);
}

bool provider_store_t::validate_input(
    const provider_input_t &input, bool allow_insecure_remote, std::string &err)
{
	if (input.name.empty() || input.name.size() > 80) {
		err =
		    "AI provider name is required and must not exceed 80 characters";
		return ai_error("provider.store", "validate-name", err);
	}
	if (!protocol_supported(input.protocol)) {
		err = "unsupported AI provider protocol";
		return ai_error("provider.store", "validate-protocol", err);
	}
	if (input.base_url.empty() || input.base_url.size() > 2048) {
		err = "AI provider URL is required";
		return ai_error("provider.store", "validate-url-length", err);
	}
	if (input.base_url.find('?') != std::string::npos ||
	    input.base_url.find('#') != std::string::npos) {
		err = "AI provider URL must not contain a query or fragment";
		return ai_error(
		    "provider.store", "validate-url-components", err);
	}
	if (input.model.empty() || input.model.size() > 200) {
		err = "AI model name is required";
		return ai_error("provider.store", "validate-model", err);
	}
	const bool valid_min_effort =
	    input.responses_min_reasoning_effort == "auto" ||
	    input.responses_min_reasoning_effort == "none" ||
	    input.responses_min_reasoning_effort == "low";
	const bool valid_summary =
	    input.responses_reasoning_summary == "auto" ||
	    input.responses_reasoning_summary == "concise" ||
	    input.responses_reasoning_summary == "detailed" ||
	    input.responses_reasoning_summary == "none";
	const bool valid_verbosity = input.responses_text_verbosity == "low" ||
	    input.responses_text_verbosity == "medium" ||
	    input.responses_text_verbosity == "high";
	const bool valid_tier = input.responses_service_tier == "auto" ||
	    input.responses_service_tier == "default" ||
	    input.responses_service_tier == "flex" ||
	    input.responses_service_tier == "priority" ||
	    input.responses_service_tier == "fast" ||
	    input.responses_service_tier == "ultrafast";
	if (!valid_min_effort || !valid_summary || !valid_verbosity ||
	    !valid_tier ||
	    (input.responses_cache_ttl != "30m" &&
	        input.responses_cache_ttl != "none")) {
		err = "invalid OpenAI Responses advanced setting";
		return ai_error(
		    "provider.store", "validate-responses-settings", err);
	}
	if (input.protocol == "openai_responses" &&
	    input.responses_background && !input.responses_store) {
		err =
		    "OpenAI background responses require stored response state";
		return ai_error(
		    "provider.store", "validate-background-store", err);
	}
	if (input.openai_organization.size() > 200 ||
	    input.openai_project.size() > 200 ||
	    input.openai_organization.find_first_of("\r\n") !=
	        std::string::npos ||
	    input.openai_project.find_first_of("\r\n") != std::string::npos) {
		err = "invalid OpenAI organization or project header";
		return ai_error(
		    "provider.store", "validate-openai-routing", err);
	}
	if (input.api_key.size() > 8192) {
		err = "AI API key is too long";
		return ai_error(
		    "provider.store", "validate-api-key-length", err);
	}
	bool https = false;
	std::string hostname;
	if (!parse_base_url(input.base_url, https, hostname, err)) {
		return ai_error("provider.store", "parse-base-url", err);
	}
	if (!https && !is_loopback_host(hostname) && !allow_insecure_remote) {
		err = "non-local AI providers must use HTTPS";
		return ai_error("provider.store", "require-https", err);
	}
	const std::string lower_model = lower_ascii(input.model);
	const std::string lower_url = lower_ascii(input.base_url);
	const char *maas_suffix = ".maas.aliyuncs.com";
	const size_t maas_suffix_size = strlen(maas_suffix);
	const bool maas_host = hostname.size() > maas_suffix_size &&
	    hostname.compare(hostname.size() - maas_suffix_size,
	        maas_suffix_size, maas_suffix) == 0;
	const bool official_qwen_url =
	    lower_url.find("/compatible-mode/v1") != std::string::npos &&
	    (hostname == "dashscope.aliyuncs.com" ||
	        hostname == "dashscope-intl.aliyuncs.com" || maas_host);
	const bool third_party_model =
	    lower_model.compare(0, 8, "deepseek") == 0 ||
	    lower_model.compare(0, 4, "kimi") == 0;
	const bool qwen_responses = input.protocol == "openai_responses" &&
	    !third_party_model &&
	    (lower_model.compare(0, 4, "qwen") == 0 || official_qwen_url);
	if (qwen_responses &&
	    (!input.responses_store || input.responses_background ||
	        input.responses_compact || !input.openai_organization.empty() ||
	        !input.openai_project.empty())) {
		err =
		    "Qwen Responses requires stored state and does not support background, native compaction, or OpenAI routing headers";
		return ai_error(
		    "provider.store", "validate-qwen-responses", err);
	}
	if (input.qwen_session_cache && !qwen_responses) {
		err =
		    "Qwen session cache is only available for Qwen Responses providers";
		return ai_error(
		    "provider.store", "validate-qwen-session-cache", err);
	}
	if (!(input.protocol == "ollama" && !is_loopback_host(hostname) &&
	        !https && !allow_insecure_remote))
		return true;
	err = "remote Ollama endpoints must use HTTPS";
	return ai_error("provider.store", "require-ollama-https", err);
}

} // namespace ai
} // namespace webcool
