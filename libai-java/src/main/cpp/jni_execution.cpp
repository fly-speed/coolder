#include "jni_internal.h"
#include <stdexcept>

namespace {
struct StoreContext { JNIEnv *env; jobject store; };
// Translate synchronous Java operations inside a bounded local reference frame.
void dispatch(void *user, const char *operation, const char *scope,
    const char *revision, const char *state, libai_execution_result *result)
{
    auto *context = static_cast<StoreContext *>(user);
    JNIEnv *env = context->env;
    if (env->PushLocalFrame(16) < 0) {
        env->ExceptionClear();
        libai_execution_result_set(result, "", "", "JNI execution frame failed");
        return;
    }
    try {
        jclass cls = env->FindClass("io/coolder/libai/Execution");
        if (!cls) throw std::runtime_error(jni_error(env));
        jmethodID method = env->GetStaticMethodID(cls, "dispatch",
            "(Lio/coolder/libai/Execution$Store;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
        if (!method) throw std::runtime_error(jni_error(env));
        jstring op = jni_string(env, operation), name = jni_string(env, scope),
            expected = jni_string(env, revision), snapshot = jni_string(env, state);
        if (env->ExceptionCheck()) throw std::runtime_error(jni_error(env));
        auto values = static_cast<jobjectArray>(env->CallStaticObjectMethod(cls,
            method, context->store, op, name, expected, snapshot));
        if (env->ExceptionCheck()) throw std::runtime_error(jni_error(env));
        if (!values || env->GetArrayLength(values) != 2)
            throw std::runtime_error("invalid execution response");
        auto next = static_cast<jstring>(env->GetObjectArrayElement(values, 0));
        auto raw = static_cast<jstring>(env->GetObjectArrayElement(values, 1));
        const auto next_text = jni_utf8(env, next), raw_text = jni_utf8(env, raw);
        libai_execution_result_set(result, next_text.c_str(), raw_text.c_str(), "");
    } catch (const std::exception &error) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        libai_execution_result_set(result, "", "", error.what());
    } catch (...) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        libai_execution_result_set(result, "", "", "JNI execution callback failed");
    }
    env->PopLocalFrame(nullptr);
}
}
void jni_store_dispatch(JNIEnv *env,jobject store,const char *op,const char *scope,
    const char *revision,const char *state,libai_execution_result *result) {
    StoreContext holder{env,store};dispatch(&holder,op,scope,revision,state,result);
}
void jni_execution(void *user, const char *op, const char *scope,
    const char *revision, const char *state, libai_execution_result *result)
{
    auto *context = static_cast<jni_agent_t *>(user);
    JNIEnv *env = nullptr;
    context->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (!env) { libai_execution_result_set(result, "", "", "Java caller thread unavailable"); return; }
    if (env->PushLocalFrame(8) < 0) {
        env->ExceptionClear(); libai_execution_result_set(result, "", "", "JNI store frame failed"); return;
    }
    jclass cls = env->GetObjectClass(context->owner);
    jfieldID field = cls ? env->GetFieldID(cls, "executionStore", "Lio/coolder/libai/Execution$Store;") : nullptr;
    jobject store = field ? env->GetObjectField(context->owner, field) : nullptr;
    if (env->ExceptionCheck() || !store) {
        env->ExceptionClear(); libai_execution_result_set(result, "", "", "Java store unavailable");
    } else {
        StoreContext holder{env, store};
        dispatch(&holder, op, scope, revision, state, result);
    }
    env->PopLocalFrame(nullptr);
}
extern "C" {
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeSetExecutionStore(
    JNIEnv *env, jobject, jlong pointer, jstring scope, jboolean enabled)
{
    try {
        auto *context = reinterpret_cast<jni_agent_t *>(pointer);
        const auto name = jni_utf8(env, scope);
        if (libai_agent_set_execution_store(context->agent, name.c_str(), enabled ? jni_execution : nullptr, context))
            jni_throw(env, libai_last_error());
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI execution configuration failed"); }
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Agent_nativeIdempotencyKey(
    JNIEnv *env, jclass, jlong pointer)
{
    try {
        return jni_string(env, libai_invocation_idempotency_key(reinterpret_cast<libai_invocation *>(pointer)));
    } catch (const std::exception &error) { jni_throw(env, error.what()); }
    catch (...) { jni_throw(env, "JNI idempotency key failed"); }
    return nullptr;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Execution_nativeConfirm(
    JNIEnv *env, jclass, jobject store, jstring scope, jstring input, jstring instructions,
    jstring revision, jstring key, jstring output, jboolean error, jstring outcome, jstring reference)
{
    try {
        StoreContext context{env, store};
        const auto s = jni_utf8(env, scope), i = jni_utf8(env, input), ins = jni_utf8(env, instructions),
            r = jni_utf8(env, revision), k = jni_utf8(env, key), o = jni_utf8(env, output),
            effect = jni_utf8(env, outcome), ref = jni_utf8(env, reference);
        if (libai_execution_confirm(dispatch, &context, s.c_str(), i.c_str(), ins.c_str(),
            r.c_str(), k.c_str(), o.c_str(), error, effect.c_str(), ref.c_str()))
            jni_throw(env, libai_last_error());
    } catch (const std::exception &failure) { jni_throw(env, failure.what()); }
    catch (...) { jni_throw(env, "JNI execution reconciliation failed"); }
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Execution_nativeAdoptDeadline(
    JNIEnv *env,jclass,jobject store,jstring scope,jstring input,jstring instructions,
    jstring revision,jlong remaining,jstring reference)
{
    try {
        StoreContext context{env,store};
        const auto s=jni_utf8(env,scope),i=jni_utf8(env,input),ins=jni_utf8(env,instructions),
            r=jni_utf8(env,revision),ref=jni_utf8(env,reference);
        if(libai_execution_adopt_deadline(dispatch,&context,s.c_str(),i.c_str(),ins.c_str(),
            r.c_str(),remaining,ref.c_str()))jni_throw(env,libai_last_error());
    }catch(const std::exception &failure){jni_throw(env,failure.what());}
    catch(...){jni_throw(env,"JNI legacy deadline adoption failed");}
}
}
