#include "stdafx.h"
#include "ai_provider_store_internal.h"
namespace webcool
{
namespace ai
{
namespace provider_store_detail
{
bool ensure_private_dir(const std::string &path, std::string &err)
{
	if (!storage_detail::make_dir_recursive(path.c_str())) {
		err = "cannot create AI settings directory";
		return false;
	}
#ifdef _WIN32
	std::wstring wide;
	if (!webcool_utf8_path_to_wide(path.c_str(), wide)) {
		err = "cannot validate AI settings directory";
		return false;
	}
	const DWORD attributes = GetFileAttributesW(wide.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES ||
	    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
	    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
		err = "AI settings path is not a safe directory";
		return false;
	}
#else
	struct stat st;
	if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
	    S_ISLNK(st.st_mode)) {
		err = "AI settings path is not a safe directory";
		return false;
	}
	if (chmod(path.c_str(), 0700) != 0) {
		err = std::string("cannot protect AI settings directory: ") +
		    strerror(errno);
		return false;
	}
#endif
	return true;
}

bool write_master_key_file(const std::string &upload_root,
    const std::vector<unsigned char> &active,
    const std::vector<unsigned char> &secondary, const std::string &phase,
    std::string &err)
{
	if (active.size() != 32 ||
	    (!secondary.empty() && secondary.size() != 32) ||
	    (!secondary.empty() && phase != "prepared" &&
	        phase != "committed")) {
		err = "invalid AI master key journal state";
		return false;
	}
	const std::string auth_dir =
	    storage_detail::join_path(upload_root, "webcool_auth");
	if (!ensure_private_dir(auth_dir, err))
		return false;
	const std::string path =
	    storage_detail::join_path(upload_root, kMasterKeyFile);
	const std::string tmp = path + ".tmp";
	std::ofstream out(
	    tmp.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
	if (!out.good()) {
		err = "cannot write AI master key";
		return false;
	}
	if (secondary.empty()) {
		out << hex_encode(&active[0], active.size()) << '\n';
	} else {
		out << kMasterKeyHeader << '\n'
		    << "active=" << hex_encode(&active[0], active.size())
		    << '\n'
		    << "secondary="
		    << hex_encode(&secondary[0], secondary.size()) << '\n'
		    << "phase=" << phase << '\n';
	}
	out.close();
	if (!out.good()) {
		err = "cannot flush AI master key";
		return false;
	}
#ifndef _WIN32
	if (chmod(tmp.c_str(), 0600) != 0) {
		unlink(tmp.c_str());
		err = "cannot protect AI master key";
		return false;
	}
#endif
	if (!(rename(tmp.c_str(), path.c_str()) != 0))
		return true;
	err = std::string("cannot install AI master key: ") + strerror(errno);
	return false;
}

bool decode_key_text(
    const std::string &encoded, std::vector<unsigned char> &key)
{
	std::string decoded;
	if (!hex_decode(encoded, decoded) || decoded.size() != 32) {
		if (decoded.empty())
			return false;
		OPENSSL_cleanse(&decoded[0], decoded.size());
		return false;
	}
	key.assign(decoded.begin(), decoded.end());
	OPENSSL_cleanse(&decoded[0], decoded.size());
	return true;
}

bool read_master_keyring(
    const std::string &upload_root, master_keyring_t &keyring, std::string &err)
{
	const std::string auth_dir =
	    storage_detail::join_path(upload_root, "webcool_auth");
	if (!ensure_private_dir(auth_dir, err))
		return false;
	const std::string path =
	    storage_detail::join_path(upload_root, kMasterKeyFile);
	std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
	if (!in.good()) {
		keyring.active.assign(32, 0);
		if (!(RAND_bytes(&keyring.active[0],
		          static_cast<int>(keyring.active.size())) != 1))
			return write_master_key_file(upload_root,
			    keyring.active, keyring.secondary, keyring.phase,
			    err);
		err = "cannot generate AI master key";
		return false;
	}
	std::string first;
	if (!std::getline(in, first)) {
		err = "invalid AI master key";
		return false;
	}
	if (first != kMasterKeyHeader) {
		if (decode_key_text(first, keyring.active))
			return true;
		err = "invalid AI master key";
		return false;
	}
	std::string active_line;
	std::string secondary_line;
	std::string phase_line;
	std::string extra;
	if (!std::getline(in, active_line) ||
	    !std::getline(in, secondary_line) ||
	    !std::getline(in, phase_line) || std::getline(in, extra) ||
	    active_line.compare(0, 7, "active=") != 0 ||
	    secondary_line.compare(0, 10, "secondary=") != 0 ||
	    phase_line.compare(0, 6, "phase=") != 0 ||
	    !decode_key_text(active_line.substr(7), keyring.active) ||
	    !decode_key_text(secondary_line.substr(10), keyring.secondary)) {
		err = "invalid AI master key journal";
		return false;
	}
	keyring.phase = phase_line.substr(6);
	if (!(keyring.phase != "prepared" && keyring.phase != "committed"))
		return true;
	err = "invalid AI master key journal phase";
	return false;
}

bool encrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &plaintext, std::string &encoded,
    std::string &err)
{
	if (key.size() != 32) {
		err = "invalid AI encryption key";
		return false;
	}
	unsigned char nonce[12];
	if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
		err = "cannot generate API key nonce";
		return false;
	}
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		err = "cannot initialize API key encryption";
		return false;
	}
	std::vector<unsigned char> ciphertext(plaintext.size() + 16);
	unsigned char tag[16];
	int count = 0;
	int total = 0;
	bool ok =
	    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
	    EVP_CIPHER_CTX_ctrl(
	        ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), NULL) == 1 &&
	    EVP_EncryptInit_ex(ctx, NULL, NULL, &key[0], nonce) == 1;
	if (ok && !aad.empty()) {
		ok = EVP_EncryptUpdate(ctx, NULL, &count,
		         reinterpret_cast<const unsigned char *>(aad.data()),
		         static_cast<int>(aad.size())) == 1;
	}
	if (ok && !plaintext.empty()) {
		ok = EVP_EncryptUpdate(ctx, &ciphertext[0], &count,
		         reinterpret_cast<const unsigned char *>(
		             plaintext.data()),
		         static_cast<int>(plaintext.size())) == 1;
		total = count;
	}
	if (ok) {
		ok = EVP_EncryptFinal_ex(ctx, &ciphertext[0] + total, &count) ==
		    1;
		total += count;
	}
	if (ok)
		ok = EVP_CIPHER_CTX_ctrl(
		         ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) == 1;
	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		err = "cannot encrypt API key";
		return false;
	}
	std::string packed("v1:");
	packed += hex_encode(nonce, sizeof(nonce));
	packed += ":";
	packed += hex_encode(tag, sizeof(tag));
	packed += ":";
	packed += hex_encode(&ciphertext[0], static_cast<size_t>(total));
	encoded.swap(packed);
	return true;
}

bool encrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &plaintext, std::string &encoded, std::string &err)
{
	master_keyring_t keyring;
	if (read_master_keyring(upload_root, keyring, err))
		return encrypt_secret_with_key(
		    keyring.active, aad, plaintext, encoded, err);
	return false;
}

bool decrypt_secret_with_key(const std::vector<unsigned char> &key,
    const std::string &aad, const std::string &encoded, std::string &plaintext,
    std::string &err)
{
	plaintext.clear();
	if (encoded.empty())
		return true;
	if (encoded.compare(0, 3, "v1:") != 0) {
		err = "unsupported API key encryption version";
		return false;
	}
	const size_t first = encoded.find(':', 3);
	const size_t second = first == std::string::npos ?
	    std::string::npos :
	    encoded.find(':', first + 1);
	if (first == std::string::npos || second == std::string::npos) {
		err = "invalid encrypted API key";
		return false;
	}
	std::string nonce_text, tag_text, ciphertext_text;
	if (!hex_decode(encoded.substr(3, first - 3), nonce_text) ||
	    !hex_decode(
	        encoded.substr(first + 1, second - first - 1), tag_text) ||
	    !hex_decode(encoded.substr(second + 1), ciphertext_text) ||
	    nonce_text.size() != 12 || tag_text.size() != 16) {
		err = "invalid encrypted API key";
		return false;
	}
	if (key.size() != 32) {
		err = "invalid AI decryption key";
		return false;
	}
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (ctx == NULL) {
		err = "cannot initialize API key decryption";
		return false;
	}
	std::vector<unsigned char> clear(ciphertext_text.size() + 1);
	cleanse_vector_t clear_guard(clear);
	int count = 0;
	int total = 0;
	bool ok =
	    EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
	    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
	        static_cast<int>(nonce_text.size()), NULL) == 1 &&
	    EVP_DecryptInit_ex(ctx, NULL, NULL, &key[0],
	        reinterpret_cast<const unsigned char *>(nonce_text.data())) ==
	        1;
	if (ok && !aad.empty()) {
		ok = EVP_DecryptUpdate(ctx, NULL, &count,
		         reinterpret_cast<const unsigned char *>(aad.data()),
		         static_cast<int>(aad.size())) == 1;
	}
	if (ok && !ciphertext_text.empty()) {
		ok = EVP_DecryptUpdate(ctx, &clear[0], &count,
		         reinterpret_cast<const unsigned char *>(
		             ciphertext_text.data()),
		         static_cast<int>(ciphertext_text.size())) == 1;
		total = count;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
		         static_cast<int>(tag_text.size()),
		         const_cast<char *>(tag_text.data())) == 1;
	}
	if (ok) {
		ok = EVP_DecryptFinal_ex(ctx, &clear[0] + total, &count) == 1;
		total += count;
	}
	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		err = "cannot decrypt API key";
		return false;
	}
	plaintext.assign(reinterpret_cast<const char *>(&clear[0]),
	    static_cast<size_t>(total));
	return true;
}

bool decrypt_secret(const std::string &upload_root, const std::string &aad,
    const std::string &encoded, std::string &plaintext, std::string &err)
{
	master_keyring_t keyring;
	if (!read_master_keyring(upload_root, keyring, err))
		return false;
	if (decrypt_secret_with_key(
	        keyring.active, aad, encoded, plaintext, err)) {
		return true;
	}
	if (!keyring.secondary.empty() &&
	    decrypt_secret_with_key(
	        keyring.secondary, aad, encoded, plaintext, err)) {
		return true;
	}
	err = "cannot decrypt API key";
	return false;
}

}
}
}
