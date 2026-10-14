#pragma once
#include "agent_types.h"
#include "json_guard.h"
#include "vendor/rapidjson/writer.h"
#include <mutex>
#include <limits>
#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace libai {
// Host codecs must authenticate scope/revision along with encrypted state.
// Plaintext is an explicit choice, never the constructor default.
struct execution_file_codec_t {
    using transform_t = std::function<std::string(const std::string &scope,
        const std::string &revision, const std::string &data)>;
    transform_t encode, decode;
    static execution_file_codec_t plaintext()
    {
        execution_file_codec_t codec;
        codec.encode = codec.decode = [](const std::string &, const std::string &,
            const std::string &data) { return data; };
        return codec;
    }
};
// POSIX local-filesystem store: pinned private directory, flock lease, atomic
// replacement and file/directory fsync. One active scope per instance.
// Codecs must not reenter close. Do not fork with a live lease or remove/replace
// lock files. No network-FS guarantee.
class file_execution_store_t : public execution_store_t {
    struct fd_t {
        int value;
        explicit fd_t(int fd = -1) : value(fd) {}
        ~fd_t() { if (value >= 0) ::close(value); }
        fd_t(const fd_t &) = delete;
        fd_t &operator=(const fd_t &) = delete;
    };
    int root_ = -1, lease_ = -1;
    std::string scope_;
    execution_file_codec_t codec_;
    std::mutex mutex_;
    unsigned long temporary_ = 0;
    static std::size_t state_limit() { return 2 * 1024 * 1024; }
    static std::size_t wire_limit() { return 4 * 1024 * 1024; }
    static void fail(const char *message) { throw std::runtime_error(message); }
    static void scope_valid(const std::string &scope)
    {
        if (scope.empty() || scope.size() > 128) fail("invalid execution scope");
        for (unsigned char c : scope)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')) fail("invalid execution scope");
    }
    static void private_file(int fd)
    {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0600 || st.st_nlink != 1) fail("execution file is not private");
    }
    static uint64_t revision_number(const std::string &revision)
    {
        if (revision.empty() || revision[0] == '0' || revision.size() > 20) fail("invalid execution revision");
        uint64_t value = 0;
        for (char c : revision) {
            if (c < '0' || c > '9' || value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
                fail("invalid execution revision");
            value = value * 10 + (c - '0');
        }
        return value;
    }
    static void sync_file(int fd)
    {
        while (fsync(fd)) { if (errno != EINTR) fail("execution fsync failed"); }
    }
    static std::string hexadecimal(const std::string &bytes)
    {
        const char *digits = "0123456789abcdef";
        std::string hex; hex.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) { hex += digits[c >> 4]; hex += digits[c & 15]; }
        return hex;
    }
    static unsigned digit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        fail("invalid execution hexadecimal payload"); return 0;
    }
    static std::string unhex(const std::string &hex)
    {
        if (hex.empty() || hex.size() % 2 || hex.size() > wire_limit() * 2)
            fail("invalid execution hexadecimal size");
        std::string bytes; bytes.reserve(hex.size() / 2);
        for (std::size_t i = 0; i < hex.size(); i += 2)
            bytes += static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1]));
        return bytes;
    }
    static std::string envelope(const std::string &scope, const std::string &revision,
        const std::string &payload)
    {
        namespace json = governance_detail::json;
        json::StringBuffer buffer; json::Writer<json::StringBuffer> writer(buffer);
        auto hex = hexadecimal(payload);
        writer.StartObject(); writer.Key("version"); writer.Uint(1);
        writer.Key("scope"); writer.String(scope.data(), static_cast<json::SizeType>(scope.size()));
        writer.Key("revision"); writer.String(revision.data(), static_cast<json::SizeType>(revision.size()));
        writer.Key("encoding"); writer.String("hex");
        writer.Key("payload"); writer.String(hex.data(), static_cast<json::SizeType>(hex.size()));
        writer.EndObject();
        return std::string(buffer.GetString(), buffer.GetSize()) + "\n";
    }
    static std::string field(const governance_detail::json::Document &value, const char *name)
    {
        if (!value.HasMember(name) || !value[name].IsString()) fail("invalid execution envelope field");
        return std::string(value[name].GetString(), value[name].GetStringLength());
    }
    execution_snapshot_t read(const std::string &scope)
    {
        fd_t fd(openat(root_, (scope + ".state").c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
        if (fd.value < 0) {
            if (errno == ENOENT) return execution_snapshot_t();
            fail("execution state open failed");
        }
        private_file(fd.value);
        std::string wire;
        char buffer[4096];
        for (;;) {
            ssize_t count = ::read(fd.value, buffer, sizeof(buffer));
            if (count < 0) { if (errno == EINTR) continue; fail("execution state read failed"); }
            if (!count) break;
            if (wire.size() + static_cast<std::size_t>(count) > wire_limit() * 2 + 256) fail("execution file too large");
            wire.append(buffer, static_cast<std::size_t>(count));
        }
        execution_snapshot_t snapshot;
        std::string encoded;
        if (!wire.empty() && wire[0] >= '1' && wire[0] <= '9') {
            // Legacy revision-line files are read without mutation; next successful
            // commit writes JSON atomically under the same scope lock.
            auto separator = wire.find('\n');
            if (separator == std::string::npos || wire.size() > wire_limit() + 21)
                fail("corrupt legacy execution envelope");
            snapshot.revision = wire.substr(0, separator);
            encoded = wire.substr(separator + 1);
        } else {
            governance_detail::json::Document value;
            governance_detail::parse_json_bounded(wire, value, wire_limit() * 2 + 256);
            if (!value.IsObject() || value.MemberCount() != 5 || !value.HasMember("version") ||
                !value["version"].IsUint() || value["version"].GetUint() != 1)
                fail("unsupported execution envelope");
            if (field(value,"scope") != scope || field(value,"encoding") != "hex")
                fail("execution envelope scope/encoding mismatch");
            snapshot.revision = field(value,"revision");
            encoded = unhex(field(value,"payload"));
        }
        revision_number(snapshot.revision);
        if (encoded.empty() || encoded.size() > wire_limit()) fail("invalid encoded execution size");
        snapshot.state_json = codec_.decode(scope, snapshot.revision, encoded);
        if (snapshot.state_json.empty() || snapshot.state_json.size() > state_limit()) fail("invalid execution state size");
        return snapshot;
    }
    std::unique_lock<std::mutex> guard()
    {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) fail("execution store is busy");
        return lock;
    }
public:
    // Root must already exist, be owned by this uid, and have mode 0700.
    explicit file_execution_store_t(const std::string &root, execution_file_codec_t codec)
        : codec_(std::move(codec))
    {
        if (root.empty() || root.front() != '/' || root.find('\0') != std::string::npos) fail("execution directory must be absolute");
        if (!codec_.encode || !codec_.decode) fail("execution codec is required");
        std::string directory_path = root;
        while (directory_path.size() > 1 && directory_path.back() == '/') directory_path.pop_back();
        fd_t directory(::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        struct stat st;
        if (directory.value < 0 || fstat(directory.value, &st) || !S_ISDIR(st.st_mode) ||
            st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) fail("execution directory is not private");
        root_ = directory.value; directory.value = -1;
    }
    ~file_execution_store_t() override
    {
        if (lease_ >= 0) ::close(lease_);
        if (root_ >= 0) ::close(root_);
    }
    execution_snapshot_t open(const std::string &scope) override
    {
        auto lock = guard(); scope_valid(scope);
        if (lease_ >= 0) fail("execution store already leased");
        fd_t lease(openat(root_, (scope + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (lease.value < 0) fail("execution lease open failed");
        private_file(lease.value);
        if (flock(lease.value, LOCK_EX | LOCK_NB)) fail("execution lease unavailable");
        auto snapshot = read(scope); // fd guard releases on all read/codec failures.
        scope_ = scope; lease_ = lease.value; lease.value = -1;
        return snapshot;
    }
    std::string commit(const std::string &scope, const std::string &expected, const std::string &state) override
    {
        auto lock = guard();
        if (lease_ < 0 || scope != scope_) fail("execution commit is not leased");
        if (state.empty() || state.size() > state_limit()) fail("invalid execution state size");
        auto current = read(scope);
        if (current.revision != expected) fail("execution revision conflict");
        uint64_t number = expected.empty() ? 0 : revision_number(expected);
        if (number == std::numeric_limits<uint64_t>::max()) fail("execution revision exhausted");
        auto revision = std::to_string(number + 1);
        auto encoded = codec_.encode(scope, revision, state);
        if (encoded.empty() || encoded.size() > wire_limit()) fail("invalid encoded execution size");
        auto wire = envelope(scope, revision, encoded);
        std::string path;
        fd_t file;
        for (int attempt = 0; attempt < 128; ++attempt) {
            path = scope + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(++temporary_);
            file.value = openat(root_, path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (file.value >= 0) break;
            if (errno != EEXIST) fail("execution temporary open failed");
        }
        if (file.value < 0) fail("execution temporary collisions");
        try {
            std::size_t offset = 0;
            while (offset < wire.size()) {
                auto count = ::write(file.value, wire.data() + offset, wire.size() - offset);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) fail("execution write failed");
                offset += static_cast<std::size_t>(count);
            }
            sync_file(file.value);
            int completed = file.value; file.value = -1;
            if (::close(completed)) fail("execution close failed");
            if (renameat(root_, path.c_str(), root_, (scope + ".state").c_str())) fail("execution replace failed");
            sync_file(root_);
        } catch (...) { unlinkat(root_, path.c_str(), 0); throw; }
        return revision;
    }
    void close(const std::string &scope) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scope != scope_) return;
        if (lease_ >= 0) ::close(lease_);
        lease_ = -1; scope_.clear();
    }
};
}
#endif
