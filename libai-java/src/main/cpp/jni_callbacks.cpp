#include "jni_internal.h"
#include <limits>

/* Callbacks always execute on a Java-attached caller thread, not a native worker. */
static JNIEnv *environment(jni_agent_t *context)
{
	JNIEnv *env = NULL;
	context->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
	return env;
}

/* Keep reflection references local to this callback's bounded JNI frame. */
static std::string text_field(JNIEnv *env, jobject object, const char *name)
{
	jclass cls = env->GetObjectClass(object);
	jfieldID field = env->GetFieldID(cls, name, "Ljava/lang/String;");
	auto text = static_cast<jstring>(env->GetObjectField(object, field));
	auto result = jni_utf8(env, text);
	if (text)
		env->DeleteLocalRef(text);
	env->DeleteLocalRef(cls);
	return result;
}

/* Copy the Java model's complete invocation list into native-owned memory. */
static void response_calls(JNIEnv *env, jobject object, libai_response *output)
{
	jclass cls = env->GetObjectClass(object);
	jfieldID field = env->GetFieldID(cls, "calls", "Ljava/util/List;");
	jobject list = env->GetObjectField(object, field);
	jclass list_cls = env->FindClass("java/util/List");
	jmethodID size = env->GetMethodID(list_cls, "size", "()I");
	jmethodID get =
	    env->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
	jint length = env->CallIntMethod(list, size);
	for (jint i = 0; i < length && !env->ExceptionCheck(); ++i) {
		jobject call = env->CallObjectMethod(list, get, i);
		if (env->ExceptionCheck())
			break;
		if (!call) {
			jni_throw(env, "null tool call");
			break;
		}
		const auto id = text_field(env, call, "id");
		const auto name = text_field(env, call, "name");
		const auto arguments = text_field(env, call, "arguments");
		libai_response_call(
		    output, id.c_str(), name.c_str(), arguments.c_str());
		env->DeleteLocalRef(call);
	}
	env->DeleteLocalRef(list_cls);
	env->DeleteLocalRef(list);
	env->DeleteLocalRef(cls);
}

void jni_model(void *user, const char *request, libai_response *output,
    libai_invocation *invocation)
{
	auto *context = static_cast<jni_agent_t *>(user);
	JNIEnv *env = environment(context);
	if (!env || env->PushLocalFrame(32) < 0) {
		libai_response_error(output, "JNI model frame failed", 0);
		return;
	}
	jclass cls = env->GetObjectClass(context->owner);
	jmethodID method = env->GetMethodID(cls, "complete",
	    "(Ljava/lang/String;J)Lio/coolder/libai/Agent$Response;");
	jobject response = env->CallObjectMethod(context->owner, method,
	    jni_string(env, request), reinterpret_cast<jlong>(invocation));
	if (!env->ExceptionCheck() && response) {
		const auto text = text_field(env, response, "text");
		const auto reasoning = text_field(env, response, "reasoning");
		const auto error = text_field(env, response, "error");
		jclass response_cls = env->GetObjectClass(response);
		auto input = env->GetLongField(response,
		    env->GetFieldID(response_cls, "inputTokens", "J"));
		auto generated = env->GetLongField(response,
		    env->GetFieldID(response_cls, "outputTokens", "J"));
		auto retryable = env->GetBooleanField(
		    response, env->GetFieldID(response_cls, "retryable", "Z"));
		if (static_cast<uint64_t>(input) >
		        std::numeric_limits<size_t>::max() ||
		    static_cast<uint64_t>(generated) >
		        std::numeric_limits<size_t>::max())
			jni_throw(env, "token usage exceeds native size_t");
		libai_response_text(output, text.c_str(), reasoning.c_str());
		libai_response_usage(output, static_cast<size_t>(input),
		    static_cast<size_t>(generated));
		libai_response_error(output, error.c_str(), retryable);
		if (!env->ExceptionCheck())
			response_calls(env, response, output);
	}
	if (env->ExceptionCheck())
		libai_response_error(output, jni_error(env).c_str(), 0);
	env->PopLocalFrame(NULL);
}

void jni_tool(void *user, const char *id, const char *name,
    const char *arguments, libai_tool_result *output,
    libai_invocation *invocation)
{
	auto *context = static_cast<jni_agent_t *>(user);
	JNIEnv *env = environment(context);
	if (!env || env->PushLocalFrame(32) < 0) {
		libai_tool_result_set(output, "JNI tool frame failed", 1);
		return;
	}
	jclass cls = env->GetObjectClass(context->owner);
	jmethodID method = env->GetMethodID(cls, "execute",
	    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)Lio/coolder/libai/Agent$ToolResult;");
	jobject result = env->CallObjectMethod(context->owner, method,
	    jni_string(env, id), jni_string(env, name),
	    jni_string(env, arguments), reinterpret_cast<jlong>(invocation));
	if (!env->ExceptionCheck() && result) {
		const auto text = text_field(env, result, "text");
		jclass result_cls = env->GetObjectClass(result);
		jboolean error = env->GetBooleanField(
		    result, env->GetFieldID(result_cls, "error", "Z"));
		libai_tool_result_set(output, text.c_str(), error);
	}
	if (env->ExceptionCheck())
		libai_tool_result_set(output, jni_error(env).c_str(), 1);
	env->PopLocalFrame(NULL);
}

int jni_text(void *user, const char *delta)
{
	auto *context = static_cast<jni_agent_t *>(user);
	JNIEnv *env = environment(context);
	if (!env || env->PushLocalFrame(16) < 0)
		return 0;
	jclass cls = env->GetObjectClass(context->owner);
	jmethodID method =
	    env->GetMethodID(cls, "text", "(Ljava/lang/String;)Z");
	jboolean allowed = env->CallBooleanMethod(
	    context->owner, method, jni_string(env, delta));
	if (env->ExceptionCheck()) {
		jni_error(env);
		allowed = JNI_FALSE;
	}
	env->PopLocalFrame(NULL);
	return allowed;
}

int jni_authorize(
    void *user, const char *name, const char *arguments, int required)
{
	auto *context = static_cast<jni_agent_t *>(user);
	JNIEnv *env = environment(context);
	if (!env || env->PushLocalFrame(16) < 0)
		return 0;
	jclass cls = env->GetObjectClass(context->owner);
	jmethodID method = env->GetMethodID(
	    cls, "authorize", "(Ljava/lang/String;Ljava/lang/String;Z)Z");
	jboolean allowed = env->CallBooleanMethod(context->owner, method,
	    jni_string(env, name), jni_string(env, arguments),
	    static_cast<jboolean>(required));
	if (env->ExceptionCheck()) {
		jni_error(env);
		libai_agent_cancel(context->agent);
		allowed = JNI_FALSE;
	}
	env->PopLocalFrame(NULL);
	return allowed;
}

void jni_event(void *user, const char *event)
{
	auto *context = static_cast<jni_agent_t *>(user);
	JNIEnv *env = environment(context);
	if (!env || env->PushLocalFrame(16) < 0) {
		libai_agent_cancel(context->agent);
		return;
	}
	jclass cls = env->GetObjectClass(context->owner);
	jmethodID method =
	    env->GetMethodID(cls, "event", "(Ljava/lang/String;)V");
	env->CallVoidMethod(context->owner, method, jni_string(env, event));
	if (env->ExceptionCheck()) {
		jni_error(env);
		libai_agent_cancel(context->agent);
	}
	env->PopLocalFrame(NULL);
}
