#include "jni_internal.h"
#include <memory>
#include <stdexcept>
namespace {
using OwnedText = std::unique_ptr<char, void(*)(char*)>;
libai_file_execution_store *store(jlong pointer)
{
    if (!pointer) throw std::invalid_argument("file execution store is null");
    return reinterpret_cast<libai_file_execution_store *>(pointer);
}
}
extern "C" {
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_FileExecutionStore_nativeAvailable(JNIEnv *, jclass)
{ return libai_file_execution_store_available(); }
JNIEXPORT jlong JNICALL Java_io_coolder_libai_FileExecutionStore_nativeCreate(JNIEnv *env, jclass, jstring root)
{
    try {
        const auto path = jni_utf8(env, root);
        auto *pointer = libai_file_execution_store_create_plaintext(path.c_str());
        if (!pointer) jni_throw(env, libai_last_error());
        return reinterpret_cast<jlong>(pointer);
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI file store creation failed"); }
    return 0;
}
JNIEXPORT jobjectArray JNICALL Java_io_coolder_libai_FileExecutionStore_nativeOpen(
    JNIEnv *env, jclass, jlong pointer, jstring scope)
{
    std::string name;
    bool opened = false, frame = false;
    try {
        if (env->PushLocalFrame(16) < 0) return nullptr;
        frame = true; name = jni_utf8(env, scope);
        char *revision = nullptr, *state = nullptr;
        if (libai_file_execution_store_open_values(store(pointer), name.c_str(), &revision, &state)) {
            jni_throw(env, libai_last_error());env->PopLocalFrame(nullptr);return nullptr;
        }
        opened = true;
        OwnedText r(revision, libai_string_free), s(state, libai_string_free);
        jclass strings = env->FindClass("java/lang/String");
        if (!strings) throw std::runtime_error("JNI String class unavailable");
        jobjectArray result = env->NewObjectArray(2, strings, nullptr);
        if (!result) throw std::runtime_error("JNI snapshot allocation failed");
        jstring rev = jni_string(env, r.get()), json = jni_string(env, s.get());
        if (env->ExceptionCheck()) throw std::runtime_error("JNI snapshot text failed");
        env->SetObjectArrayElement(result, 0, rev);
        if (env->ExceptionCheck()) throw std::runtime_error("JNI snapshot revision failed");
        env->SetObjectArrayElement(result, 1, json);
        if (env->ExceptionCheck()) throw std::runtime_error("JNI snapshot state failed");
        frame = false;
        result = static_cast<jobjectArray>(env->PopLocalFrame(result));
        if (!result) throw std::runtime_error("JNI snapshot frame transfer failed");
        return result;
    } catch (const std::exception &error) {
        if (opened) libai_file_execution_store_close(reinterpret_cast<libai_file_execution_store *>(pointer), name.c_str());
        if (frame) env->PopLocalFrame(nullptr);
        jni_throw(env, error.what());
    } catch (...) {
        if (opened) libai_file_execution_store_close(reinterpret_cast<libai_file_execution_store *>(pointer), name.c_str());
        if (frame) env->PopLocalFrame(nullptr);
        jni_throw(env, "JNI file store open failed");
    }
    return nullptr;
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_FileExecutionStore_nativeCommit(
    JNIEnv *env, jclass, jlong pointer, jstring scope, jstring expected, jstring state)
{
    try {
        const auto name = jni_utf8(env, scope), revision = jni_utf8(env, expected), json = jni_utf8(env, state);
        OwnedText output(libai_file_execution_store_commit(store(pointer), name.c_str(), revision.c_str(), json.c_str()), libai_string_free);
        if (!output) { jni_throw(env, libai_last_error()); return nullptr; }
        return jni_string(env, output.get());
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI file store commit failed"); }
    return nullptr;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_FileExecutionStore_nativeClose(JNIEnv *env, jclass, jlong pointer, jstring scope)
{
    try {
        const auto name = jni_utf8(env, scope);
        if (libai_file_execution_store_close(store(pointer), name.c_str())) jni_throw(env, libai_last_error());
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI file store close failed"); }
}
JNIEXPORT void JNICALL Java_io_coolder_libai_FileExecutionStore_nativeDestroy(JNIEnv *env, jclass, jlong pointer)
{
    try {
        if (libai_file_execution_store_destroy(store(pointer))) jni_throw(env, libai_last_error());
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI file store destruction failed"); }
}
}
