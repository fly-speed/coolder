#include "jni_internal.h"
#include <memory>
#include <stdexcept>
extern "C" {
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_CodingService_nativeAvailable(
    JNIEnv *, jclass)
{
	return libai_coding_available() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_CodingService_nativeCall(
    JNIEnv *env, jclass, jstring root, jstring operation, jstring arguments)
{
	try {
		const auto r = jni_utf8(env, root),
		           op = jni_utf8(env, operation),
		           a = jni_utf8(env, arguments);
		std::unique_ptr<char, decltype(&libai_string_free)> result(
		    libai_coding_call(r.c_str(), op.c_str(), a.c_str()),
		    libai_string_free);
		if (!result) {
			jni_throw(env, libai_last_error());
			return nullptr;
		}
		return jni_string(env, result.get());
	} catch (const std::exception &e) {
		jni_throw(env, e.what());
	} catch (...) {
		jni_throw(env, "JNI coding operation failed");
	}
	return nullptr;
}
}
