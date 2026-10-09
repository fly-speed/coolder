#pragma once
#include "stdafx.h"
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
extern webcool::mutex g_provider_store_mutex;
// Private metadata directory name used by the provider store.
extern const char *kAgentDir;
// Filename of the encrypted provider configuration records.
extern const char *kProviderFile;
// Filename of the provider connection-health records.
extern const char *kProviderHealthFile;
// Filename of the installation credential encryption key.
extern const char *kMasterKeyFile;
// Version marker required when parsing persisted records.
extern const char *kHeader;
// Record-format marker used to validate health header.
extern const char *kHealthHeader;
// Record-format marker used to validate master key header.
extern const char *kMasterKeyHeader;

// Current and fallback credential keys, cleansed when ownership ends.
struct master_keyring_t {
	// Key bytes used to encrypt newly written provider credentials.
	std::vector<unsigned char> active;
	// Previous encryption key retained while rotating stored credentials.
	std::vector<unsigned char> secondary;
	// Persisted key-rotation phase used to resume interrupted rotation.
	std::string phase;

	// Erase encryption key bytes before releasing their storage.
	~master_keyring_t()
	{
		if (!active.empty())
			OPENSSL_cleanse(&active[0], active.size());
		if (!secondary.empty())
			OPENSSL_cleanse(&secondary[0], secondary.size());
	}
};

// Scope guard that erases sensitive bytes from a borrowed buffer.
struct cleanse_vector_t {
	// Initialize cleanse vector state from the supplied arguments.
	explicit cleanse_vector_t(std::vector<unsigned char> &value)
	        : value_(value)
	{
	}
	// Erase the borrowed sensitive buffer before leaving its owning
	// scope.
	~cleanse_vector_t()
	{
		if (!value_.empty())
			OPENSSL_cleanse(&value_[0], value_.size());
	}
	// Borrowed sensitive byte buffer erased by this scope guard.
	std::vector<unsigned char> &value_;
};

using ::webcool::ai::record_codec::hex_encode;

using ::webcool::ai::record_codec::hex_value;

using ::webcool::ai::record_codec::hex_decode;

using ::webcool::ai::record_codec::split_tabs;

// Lowercase ASCII letters without locale-dependent conversions.
std::string lower_ascii(std::string value);

using ::webcool::ai::identifiers::valid_id;

using ::webcool::ai::identifiers::new_id;

// Create or validate a private storage directory with the required
// permissions.
bool ensure_private_dir(const std::string &path, std::string &err);

// Persist a credential master key with the required file protections.
bool write_master_key_file(const std::string &upload_root,
    const std::vector<unsigned char> &active,
    const std::vector<unsigned char> &secondary, const std::string &phase,
    std::string &err);

// Decode a stored master-key representation into key bytes.
bool decode_key_text(
    const std::string &encoded, std::vector<unsigned char> &key);

// Load the active and fallback credential encryption keys.
bool read_master_keyring(const std::string &upload_root,
    master_keyring_t &keyring, std::string &err);

// Encrypt a credential using the supplied key and authenticated context.
bool encrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &plaintext, std::string &encoded,
    std::string &err);

// Encrypt a provider credential using the installation keyring.
bool encrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &plaintext, std::string &encoded, std::string &err);

// Authenticate and decrypt a credential with the supplied key.
bool decrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &encoded, std::string &plaintext,
    std::string &err);

// Recover a provider credential using the available keyring.
bool decrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &encoded, std::string &plaintext, std::string &err);

// Return the user's private agent metadata directory.
std::string agent_dir(const std::string &user_root);

// Return the path of the user's persisted provider configurations.
std::string provider_file(const std::string &user_root);

// Return the path of the user's provider health records.
std::string provider_health_file(const std::string &user_root);

// User-store information needed while rotating provider credentials.
struct rotation_user_t {
	// Authenticated account name associated with this object.
	std::string username;
	// Filesystem root belonging to the authenticated user.
	std::string user_root;
};

// Validate a username before opening its credential store.
bool valid_rotation_username(const std::string &username);

using ::webcool::ai::file_ops::safe_directory;

// Verify that a storage path refers to an acceptable regular file.
bool safe_regular_file(const std::string &path);

using ::webcool::ai::file_ops::path_entry_exists;

// A restart checkpoint is encrypted with the current provider keyring. Retiring
// either key while a checkpoint exists could make an interrupted run
// unrecoverable, so master-key rotation is blocked until those runs finish or
// are cancelled. The scan treats any entry as material and fails closed.
bool checkpoint_directory_has_entries(
    const std::string &user_root, bool &has_entries, std::string &err);

// Enumerate user credential stores participating in master-key rotation.
bool collect_rotation_users(const std::string &upload_root,
    std::vector<rotation_user_t> &users, std::string &err);

// Parse a complete nonnegative integer without accepting trailing text.
bool parse_nonnegative_number(const std::string &text, long long &value);

// Test failures sometimes contain line breaks or very large upstream response
// fragments. Keep only a compact display-safe summary in the user's telemetry.
std::string sanitize_test_error(const std::string &input);

// Load stored records with synchronization supplied by the caller.
bool load_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err);

// Load provider health records under caller-managed synchronization.
bool load_health_unlocked(const std::string &user_root,
    std::vector<provider_config_t> &providers, std::string &err);

// Write provider health records under caller-managed synchronization.
bool save_health_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err);

// Write stored records with synchronization supplied by the caller.
bool save_unlocked(const std::string &user_root,
    const std::vector<provider_config_t> &providers, std::string &err);

// Check whether a provider protocol has an implemented adapter.
bool protocol_supported(const std::string &protocol);

// Split and validate the configured provider URL for transport use.
bool parse_base_url(const std::string &url, bool &https, std::string &hostname,
    std::string &err);

// Recognize local-only hosts for provider endpoint policy.
bool is_loopback_host(const std::string &host);

}
}
}
