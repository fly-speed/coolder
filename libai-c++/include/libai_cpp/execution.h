#pragma once
#include "export.h"
#include <memory>
#include <functional>
#include <string>
#include <chrono>

namespace libai {
namespace cpp {
// Independent snapshot owned by the host; new scopes return empty fields.
struct ExecutionSnapshot {
    std::string revision, state_json;
};
// Host implementation holds an exclusive scope lease from open to close.
// Atomic durable CAS, authorization and data protection belong to the host.
class ExecutionStore {
public:
    virtual ~ExecutionStore() {}
    // A failed acquisition must clean up only its own partially acquired resources.
    virtual ExecutionSnapshot open(const std::string &scope) = 0;
    // Commit while leased; reject stale revisions and return a fresh nonempty one.
    virtual std::string commit(const std::string &scope,
        const std::string &expected_revision, const std::string &state_json) = 0;
    // Release reliably even during exception unwinding; never throw.
    virtual void close(const std::string &scope) noexcept = 0;
};
// Independent codec callbacks. Encryption must authenticate scope/revision/data.
struct ExecutionFileCodec {
    using Transform = std::function<std::string(const std::string &scope,
        const std::string &revision, const std::string &data)>;
    Transform encode, decode;
    // Explicit plaintext choice; this provides no confidentiality or authentication.
    static LIBAI_CPP_API ExecutionFileCodec plaintext();
};
// True only where the built-in local filesystem adapter is compiled (POSIX).
LIBAI_CPP_API bool file_execution_store_available() noexcept;
// Root must be an existing private absolute directory (uid-owned, mode 0700).
// One active scope per instance. The host must explicitly choose/provide a codec.
LIBAI_CPP_API std::shared_ptr<ExecutionStore> create_file_execution_store(
    const std::string &root, ExecutionFileCodec codec);
struct ToolResult;
// Trusted host reconciliation after verifying an external receipt; never a model tool.
// outcome must be none/applied. Reject stale revision, task identity or effect key.
LIBAI_CPP_API void confirm_execution_effect(std::shared_ptr<ExecutionStore> store,
    const std::string &scope, const std::string &input, const std::string &instructions,
    const std::string &expected_revision, const std::string &key, const ToolResult &output,
    const std::string &outcome, const std::string &reference);
// Trusted legacy migration, only after independently verifying the remaining budget.
// CAS required; zero means unlimited. Does not reconcile unknown effects.
LIBAI_CPP_API void adopt_execution_deadline(std::shared_ptr<ExecutionStore> store,
    const std::string &scope, const std::string &input, const std::string &instructions,
    const std::string &expected_revision, std::chrono::milliseconds remaining,
    const std::string &reference);
}
}
