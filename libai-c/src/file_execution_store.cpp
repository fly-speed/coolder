#include "bridge_internal.h"
#include <libai/sdk/file_execution_store.h>
#include <mutex>

struct libai_file_execution_store {
    std::shared_ptr<libai::execution_store_t> value;
    std::mutex mutex;
    std::string scope;
};
extern "C" {
int libai_file_execution_store_available(void)
{
#if defined(__unix__) || defined(__APPLE__)
    return 1;
#else
    return 0;
#endif
}
libai_file_execution_store *libai_file_execution_store_create_plaintext(const char *root)
{
    try {
#if defined(__unix__) || defined(__APPLE__)
        std::unique_ptr<libai_file_execution_store> store(new libai_file_execution_store);
        store->value = std::make_shared<libai::file_execution_store_t>(bridge_text(root),
            libai::execution_file_codec_t::plaintext());
        return store.release();
#else
        (void)root; throw std::runtime_error("file execution store requires POSIX");
#endif
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file store creation failed"); }
    return nullptr;
}
char *libai_file_execution_store_open(libai_file_execution_store *store, const char *scope)
{
    try {
        if (!store) throw std::invalid_argument("file store is null");
        std::lock_guard<std::mutex> lock(store->mutex);
        if (!store->scope.empty()) throw std::logic_error("file store is leased");
        auto name = bridge_text(scope);
        auto snapshot = store->value->open(name);
        try {
            store->scope = name;
            return bridge_owned(bridge_execution_snapshot_json(snapshot));
        } catch (...) { store->value->close(name); store->scope.clear(); throw; }
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file store open failed"); }
    return nullptr;
}
int libai_file_execution_store_open_values(libai_file_execution_store *store, const char *scope,
    char **revision, char **state_json)
{
    if (revision) *revision = nullptr;
    if (state_json) *state_json = nullptr;
    try {
        if (!store || !revision || !state_json || revision == state_json)
            throw std::invalid_argument("distinct snapshot outputs are required");
        std::lock_guard<std::mutex> lock(store->mutex);
        if (!store->scope.empty()) throw std::logic_error("file store is leased");
        auto name = bridge_text(scope);
        auto snapshot = store->value->open(name);
        try {
            if (!snapshot.state_json.empty()) {
                libai::governance_detail::json::Document state;
                libai::governance_detail::parse_json(snapshot.state_json, state);
            }
            std::unique_ptr<char, void(*)(char*)> r(bridge_owned(snapshot.revision), libai_string_free);
            std::unique_ptr<char, void(*)(char*)> s(bridge_owned(snapshot.state_json), libai_string_free);
            store->scope = name;
            *revision = r.release(); *state_json = s.release();
            return 0;
        } catch (...) { store->value->close(name); store->scope.clear(); throw; }
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file snapshot open failed"); }
    return -1;
}
char *libai_file_execution_store_commit(libai_file_execution_store *store, const char *scope,
    const char *expected, const char *state)
{
    try {
        if (!store) throw std::invalid_argument("file store is null");
        std::lock_guard<std::mutex> lock(store->mutex);
        if (store->scope.empty() || store->scope != bridge_text(scope))
            throw std::logic_error("file store commit is not leased");
        return bridge_owned(store->value->commit(store->scope, bridge_text(expected), bridge_text(state)));
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file store commit failed"); }
    return nullptr;
}
int libai_file_execution_store_close(libai_file_execution_store *store, const char *scope)
{
    try {
        if (!store) throw std::invalid_argument("file store is null");
        std::lock_guard<std::mutex> lock(store->mutex);
        if (store->scope.empty()) return 0;
        if (store->scope != bridge_text(scope)) throw std::logic_error("file store scope mismatch");
        store->value->close(store->scope); store->scope.clear(); return 0;
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file store close failed"); }
    return -1;
}
int libai_file_execution_store_destroy(libai_file_execution_store *store)
{
    if (!store) return 0;
    try {
        std::unique_lock<std::mutex> lock(store->mutex);
        if (!store->scope.empty()) throw std::logic_error("file store is leased");
        lock.unlock(); delete store; return 0;
    } catch (const std::exception &error) { bridge_error(error.what()); }
    catch (...) { bridge_error("file store destruction failed"); }
    return -1;
}
}
