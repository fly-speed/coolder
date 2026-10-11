#pragma once
#include <jni.h>
#include "libai.h"
#include <string>

/* Native state retains the Java facade until explicit close. */
struct jni_agent_t {
	/* VM used to obtain the run thread's own JNIEnv. */
	JavaVM *vm;
	/* Global facade reference holding Java model/tool callbacks alive. */
	jobject owner;
	/* Portable native agent and its persistent session. */
	libai_agent *agent;
};
/* Convert Java strings through standard UTF-8, avoiding modified-UTF-8 corruption. */
std::string jni_utf8(JNIEnv *env, jstring text);
/* Construct a Java string from standard UTF-8, including supplementary characters. */
jstring jni_string(JNIEnv *env, const char *text);
/* Consume a pending Java exception into a safe copied model/tool error. */
std::string jni_error(JNIEnv *env);
/* Raise an application-visible native API diagnostic. */
void jni_throw(JNIEnv *env, const char *message);
/* Run a model callback on the current Java caller thread. */
void jni_model(void *, const char *, libai_response *, libai_invocation *);
/* Dispatch a named Java tool and copy its result into native storage. */
void jni_tool(void *, const char *, const char *, const char *,
    libai_tool_result *, libai_invocation *);
/* Forward streamed text; Java observer errors request cancellation. */
int jni_text(void *, const char *);
/* Apply Java's per-call authorization policy. */
int jni_authorize(void *, const char *, const char *, int);
/* Forward native lifecycle events as UTF-8 JSON. */
void jni_event(void *, const char *);
