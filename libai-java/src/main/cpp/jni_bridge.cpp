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
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeCreate(
    JNIEnv *env, jobject owner, jstring instructions)
{
	try {
		const auto text = jni_utf8(env, instructions);
		std::unique_ptr<jni_agent_t> context(new jni_agent_t);
		env->GetJavaVM(&context->vm);
		context->owner = env->NewGlobalRef(owner);
		if (!context->owner)
			return 0;
		context->agent =
		    libai_agent_create(jni_model, context.get(), text.c_str());
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
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddTool(JNIEnv *env,
    jobject, jlong pointer, jstring name, jstring description, jstring schema,
    jboolean approval)
{
	try {
		auto *context = reinterpret_cast<jni_agent_t *>(pointer);
		const auto tool_name = jni_utf8(env, name),
		           text = jni_utf8(env, description),
		           parameters = jni_utf8(env, schema);
		if (libai_agent_add_tool(context->agent, tool_name.c_str(),
		        text.c_str(), parameters.c_str(), approval, jni_tool,
		        context))
			jni_throw(env, libai_last_error());
	} catch (const std::exception &error) {
		jni_throw(env, error.what());
	} catch (...) {
		jni_throw(env, "JNI tool registration failed");
	}
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Agent_nativeRun(JNIEnv *env,
    jobject, jlong pointer, jstring input, jlongArray limits, jboolean stream,
    jboolean authorize, jboolean events)
{
	try {
		auto *context = reinterpret_cast<jni_agent_t *>(pointer);
		const auto config = options(env, limits);
		libai_hooks hooks = { context, stream ? jni_text : NULL,
			authorize ? jni_authorize : NULL,
			events ? jni_event : NULL };
		const auto text = jni_utf8(env, input);
		char *raw = libai_agent_run(
		    context->agent, text.c_str(), &config, &hooks);
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
}
