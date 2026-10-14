#include <libai_cpp/execution.h>
#include <libai/sdk/file_execution_store.h>
#include <stdexcept>

namespace libai {
namespace cpp {
ExecutionFileCodec ExecutionFileCodec::plaintext()
{
    ExecutionFileCodec codec;
    codec.encode = codec.decode = [](const std::string &, const std::string &,
        const std::string &data) { return data; };
    return codec;
}
bool file_execution_store_available() noexcept
{
#if defined(__unix__) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}
#if defined(__unix__) || defined(__APPLE__)
namespace {
// Reverse adapter keeps the POSIX SDK implementation private to the library.
class FileExecutionStore : public ExecutionStore {
    ::libai::file_execution_store_t native_;
    static ::libai::execution_file_codec_t convert(ExecutionFileCodec codec)
    {
        ::libai::execution_file_codec_t native;
        native.encode = std::move(codec.encode); native.decode = std::move(codec.decode);
        return native;
    }
public:
    FileExecutionStore(const std::string &root, ExecutionFileCodec codec)
        : native_(root, convert(std::move(codec))) {}
    ExecutionSnapshot open(const std::string &scope) override
    {
        auto source = native_.open(scope);
        ExecutionSnapshot result;
        try {
            result.revision = std::move(source.revision);
            result.state_json = std::move(source.state_json);
        } catch (...) { native_.close(scope); throw; }
        return result;
    }
    std::string commit(const std::string &scope, const std::string &revision,
        const std::string &state) override { return native_.commit(scope, revision, state); }
    void close(const std::string &scope) noexcept override { native_.close(scope); }
};
}
#endif
std::shared_ptr<ExecutionStore> create_file_execution_store(const std::string &root,
    ExecutionFileCodec codec)
{
#if defined(__unix__) || defined(__APPLE__)
    return std::make_shared<FileExecutionStore>(root, std::move(codec));
#else
    (void)root; (void)codec;
    throw std::runtime_error("file execution store requires POSIX local filesystem");
#endif
}
}
}
