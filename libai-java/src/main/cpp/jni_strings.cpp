#include "jni_internal.h"
#include <cstring>
#include <stdexcept>

std::string jni_utf8(JNIEnv *env, jstring text)
{
	if (!text)
		return "";
	jclass cls = env->FindClass("java/lang/String");
	jmethodID method =
	    env->GetMethodID(cls, "getBytes", "(Ljava/lang/String;)[B");
	jstring encoding = env->NewStringUTF("UTF-8");
	auto bytes = static_cast<jbyteArray>(
	    env->CallObjectMethod(text, method, encoding));
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		env->DeleteLocalRef(encoding);
		env->DeleteLocalRef(cls);
		throw std::runtime_error("Java UTF-8 encoding failed");
	}
	std::string result;
	if (bytes) {
		result.resize(env->GetArrayLength(bytes));
		if (!result.empty())
			env->GetByteArrayRegion(bytes, 0, result.size(),
			    reinterpret_cast<jbyte *>(&result[0]));
		env->DeleteLocalRef(bytes);
	}
	env->DeleteLocalRef(encoding);
	env->DeleteLocalRef(cls);
	return result;
}

jstring jni_string(JNIEnv *env, const char *text)
{
	if (!text)
		text = "";
	const auto size = static_cast<jsize>(std::strlen(text));
	jbyteArray bytes = env->NewByteArray(size);
	if (!bytes)
		return NULL;
	env->SetByteArrayRegion(
	    bytes, 0, size, reinterpret_cast<const jbyte *>(text));
	jclass cls = env->FindClass("java/lang/String");
	jmethodID method =
	    env->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V");
	jstring encoding = env->NewStringUTF("UTF-8");
	auto result =
	    static_cast<jstring>(env->NewObject(cls, method, bytes, encoding));
	env->DeleteLocalRef(encoding);
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(bytes);
	return result;
}

std::string jni_error(JNIEnv *env)
{
	jthrowable error = env->ExceptionOccurred();
	if (!error)
		return "";
	env->ExceptionClear();
	jclass cls = env->GetObjectClass(error);
	jmethodID method =
	    env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
	auto text = static_cast<jstring>(env->CallObjectMethod(error, method));
	std::string result = !env->ExceptionCheck() && text ?
	    jni_utf8(env, text) :
	    "Java callback failed";
	if (env->ExceptionCheck())
		env->ExceptionClear();
	if (text)
		env->DeleteLocalRef(text);
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(error);
	return result;
}

void jni_throw(JNIEnv *env, const char *message)
{
	if (env->ExceptionCheck())
		return;
	jclass cls = env->FindClass("java/lang/IllegalStateException");
	jmethodID constructor =
	    env->GetMethodID(cls, "<init>", "(Ljava/lang/String;)V");
	jstring text = jni_string(env, message);
	auto error =
	    static_cast<jthrowable>(env->NewObject(cls, constructor, text));
	env->Throw(error);
	env->DeleteLocalRef(error);
	env->DeleteLocalRef(text);
	env->DeleteLocalRef(cls);
}
