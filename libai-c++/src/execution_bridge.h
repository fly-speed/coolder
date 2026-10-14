#pragma once
#include <libai_cpp/execution.h>
#include <libai/agent.h>

namespace libai {
namespace cpp {
namespace detail {
// Private adapter retains the host without exporting SDK types in public headers.
class ExecutionAdapter : public ::libai::execution_store_t {
    std::shared_ptr<ExecutionStore> store_;
public:
    explicit ExecutionAdapter(std::shared_ptr<ExecutionStore> store)
        : store_(std::move(store)) {}
    ::libai::execution_snapshot_t open(const std::string &scope) override
    {
        const auto source = store_->open(scope);
        ::libai::execution_snapshot_t result;
        try {
            result.revision = source.revision;
            result.state_json = source.state_json;
        } catch (...) {
            store_->close(scope);
            throw;
        }
        return result;
    }
    std::string commit(const std::string &scope, const std::string &revision,
        const std::string &state) override
    {
        return store_->commit(scope, revision, state);
    }
    void close(const std::string &scope) noexcept override { store_->close(scope); }
};
}
}
}
