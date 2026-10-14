#include "jni_internal.h"
#include <limits>
#include <memory>
#include <stdexcept>

/* Reject truncating a Java long into the target platform's size_t. */
static libai_options options(JNIEnv *env, jlongArray values)
{
	if (!values || env->GetArrayLength(values) != 8)
		throw std::invalid_argument("invalid run limits");
	jlong raw[8];
	env->GetLongArrayRegion(values, 0, 8, raw);
	for (size_t i = 0; i < 7; ++i) {
		if (raw[i] < 0 ||
		    static_cast<uint64_t>(raw[i]) >
		        std::numeric_limits<size_t>::max())
			throw std::invalid_argument(
			    "run limit exceeds native size_t");
	}
	libai_options result = { static_cast<size_t>(raw[0]),
		static_cast<size_t>(raw[1]), static_cast<size_t>(raw[2]),
		static_cast<size_t>(raw[3]), static_cast<size_t>(raw[4]),
		static_cast<size_t>(raw[5]), static_cast<size_t>(raw[6]),
		raw[7] };
	return result;
}

extern "C" {
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeConfigureOutput(
    JNIEnv *env, jobject, jlong pointer, jstring schema, jlong repairs)
{
    try {
        auto *context = reinterpret_cast<jni_agent_t *>(pointer);
        if (!context || repairs<0 || repairs>8) throw std::invalid_argument("invalid output configuration");
        const auto wire = jni_utf8(env, schema);
        if (libai_agent_configure_output(context->agent,wire.c_str(),static_cast<size_t>(repairs)))
            jni_throw(env,libai_last_error());
    } catch (const std::exception &e) { jni_throw(env,e.what()); }
    catch (...) { jni_throw(env,"output contract configuration failed"); }
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeConfigureModel(
    JNIEnv *env, jobject, jlong pointer, jstring profile)
{
	try {
		auto *context = reinterpret_cast<jni_agent_t *>(pointer);
		if (!context) throw std::invalid_argument("agent is null");
		const auto wire = jni_utf8(env, profile);
		if (libai_agent_configure_model(context->agent, wire.c_str()))
			jni_throw(env, libai_last_error());
	} catch (const std::exception &e) { jni_throw(env, e.what()); }
	catch (...) { jni_throw(env, "model profile configuration failed"); }
}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeCreate(JNIEnv *env,
    jobject owner, jstring instructions, jstring config, jstring key)
{
	try {
		const auto text = jni_utf8(env, instructions);
		const auto json =
		    config ? jni_utf8(env, config) : std::string();
		const auto secret = config ? jni_utf8(env, key) : std::string();
		std::unique_ptr<jni_agent_t> context(new jni_agent_t);
		env->GetJavaVM(&context->vm);
		context->owner = env->NewGlobalRef(owner);
		if (!context->owner)
			return 0;
		if (config) {
			context->agent = libai_agent_create_provider(
			    json.c_str(), secret.c_str(), text.c_str());
		} else {
			context->agent = libai_agent_create(
			    jni_model, context.get(), text.c_str());
		}
		if (!context->agent) {
			env->DeleteGlobalRef(context->owner);
			jni_throw(env, libai_last_error());
			return 0;
		}
		return reinterpret_cast<jlong>(context.release());
	} catch (const std::exception &error) {
		jni_throw(env, error.what());
	} catch (...) {
		jni_throw(env, "JNI agent creation failed");
	}
	return 0;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeDestroy(
    JNIEnv *env, jobject, jlong pointer)
{
	auto *context = reinterpret_cast<jni_agent_t *>(pointer);
	if (!context)
		return;
	if (libai_agent_destroy(context->agent)) {
		jni_throw(env, libai_last_error());
		return;
	}
	env->DeleteGlobalRef(context->owner);
	delete context;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddToolGoverned(JNIEnv *env,
    jobject, jlong pointer, jstring name, jstring description, jstring schema,
    jboolean approval, jstring revision, jboolean side_effect_free)
{
	try {
		auto *context = reinterpret_cast<jni_agent_t *>(pointer);
		const auto tool_name = jni_utf8(env, name),
		           text = jni_utf8(env, description),
		           parameters = jni_utf8(env, schema),
                   version = jni_utf8(env, revision);
		if (libai_agent_add_tool_governed(context->agent, tool_name.c_str(),
		        text.c_str(), parameters.c_str(), approval, version.c_str(), side_effect_free, jni_tool,
		        context))
			jni_throw(env, libai_last_error());
	} catch (const std::exception &error) {
		jni_throw(env, error.what());
	} catch (...) {
		jni_throw(env, "JNI tool registration failed");
	}
}
// Preserve the previous versioned entry point and its conservative default.
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddToolVersioned(JNIEnv *env,
    jobject owner, jlong pointer, jstring name, jstring description, jstring schema,
    jboolean approval, jstring revision)
{
    Java_io_coolder_libai_Agent_nativeAddToolGoverned(env, owner, pointer, name,
        description, schema, approval, revision, JNI_FALSE);
}
// Preserve the original JNI signature for applications using an older JAR.
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddTool(JNIEnv *env,
    jobject owner, jlong pointer, jstring name, jstring description, jstring schema,
    jboolean approval)
{
    jstring version = env->NewStringUTF("1");
    if (!version) return;
    Java_io_coolder_libai_Agent_nativeAddToolVersioned(env, owner, pointer,
        name, description, schema, approval, version);
    env->DeleteLocalRef(version);
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Agent_nativeRun(JNIEnv *env,
    jobject, jlong pointer, jstring input, jlongArray limits, jboolean stream,
    jboolean authorize, jboolean events, jlong cancellation)
{
	try {
		auto *context = reinterpret_cast<jni_agent_t *>(pointer);
		const auto config = options(env, limits);
		libai_hooks hooks = { context, stream ? jni_text : NULL,
			authorize ? jni_authorize : NULL,
			events ? jni_event : NULL };
		const auto text = jni_utf8(env, input);
		char *raw = libai_agent_run_with_cancellation(context->agent,
		    text.c_str(), &config, &hooks,
		    reinterpret_cast<libai_cancellation *>(cancellation));
		if (!raw) {
			jni_throw(env, libai_last_error());
			return NULL;
		}
		std::unique_ptr<char, void (*)(char *)> result(
		    raw, libai_string_free);
		return jni_string(env, result.get());
	} catch (const std::exception &error) {
		jni_throw(env, error.what());
	} catch (...) {
		jni_throw(env, "JNI run failed");
	}
	return NULL;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeCancel(
    JNIEnv *, jobject, jlong pointer)
{
	auto *context = reinterpret_cast<jni_agent_t *>(pointer);
	if (context)
		libai_agent_cancel(context->agent);
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeReset(
    JNIEnv *env, jobject, jlong pointer)
{
	auto *context = reinterpret_cast<jni_agent_t *>(pointer);
	if (libai_agent_reset(context->agent))
		jni_throw(env, libai_last_error());
}
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_Agent_nativeStopped(
    JNIEnv *, jclass, jlong pointer)
{
	return libai_invocation_stopped(
	    reinterpret_cast<libai_invocation *>(pointer));
}
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_Agent_nativeEmit(
    JNIEnv *env, jclass, jlong pointer, jstring text)
{
	try {
		const auto delta = jni_utf8(env, text);
		return libai_invocation_emit(
		    reinterpret_cast<libai_invocation *>(pointer),
		    delta.c_str());
	} catch (...) {
		jni_throw(env, "JNI stream failed");
		return JNI_FALSE;
	}
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Agent_nativeMessages(
    JNIEnv *env, jobject, jlong pointer)
{
	auto *context = reinterpret_cast<jni_agent_t *>(pointer);
	std::unique_ptr<char, void (*)(char *)> result(
	    libai_agent_messages(context->agent), libai_string_free);
	if (!result) {
		jni_throw(env, libai_last_error());
		return nullptr;
	}
	return jni_string(env, result.get());
}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeTokenCreate(
    JNIEnv *env, jclass)
{
	auto *token = libai_cancellation_create();
	if (!token)
		jni_throw(env, libai_last_error());
	return reinterpret_cast<jlong>(token);
}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeTokenRetain(
    JNIEnv *env, jclass, jlong pointer)
{
	auto *token = libai_cancellation_retain(
	    reinterpret_cast<libai_cancellation *>(pointer));
	if (!token)
		jni_throw(env, libai_last_error());
	return reinterpret_cast<jlong>(token);
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeTokenDestroy(
    JNIEnv *, jclass, jlong pointer)
{
	libai_cancellation_destroy(
	    reinterpret_cast<libai_cancellation *>(pointer));
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeTokenCancel(
    JNIEnv *, jclass, jlong pointer)
{
	libai_cancellation_cancel(
	    reinterpret_cast<libai_cancellation *>(pointer));
}
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_Agent_nativeTokenCancelled(
    JNIEnv *, jclass, jlong pointer)
{
	return libai_cancellation_cancelled(
	    reinterpret_cast<libai_cancellation *>(pointer));
}
}
