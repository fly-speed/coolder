#include "jni_internal.h"
#include "jni_thread.h"
#include <memory>
#include <stdexcept>
namespace {
using libai_jni::Env;
struct Context {
    JavaVM *vm=nullptr;jobject transport=nullptr;jclass cls=nullptr;jmethodID dispatch=nullptr;
    ~Context(){Env current(vm);if(current.env){
        if(transport)current.env->DeleteGlobalRef(transport);if(cls)current.env->DeleteGlobalRef(cls);
    }}
};
void failure(libai_mcp_result *result,const std::string &message) noexcept {
    try {auto text=message.substr(0,4096);libai_mcp_result_error(result,text.c_str());}
    catch(...){libai_mcp_result_error(result,"JNI MCP callback failed");}
}
void callback(void *user,const char *operation,const char *wire,const char *metadata,
    libai_mcp_result *result,libai_invocation *invocation) {
    auto *ctx=static_cast<Context*>(user);Env current(ctx->vm);JNIEnv *env=current.env;
    if(!env){failure(result,"JNI MCP thread unavailable");return;}
    if(env->PushLocalFrame(16)<0){env->ExceptionClear();failure(result,"JNI MCP frame failed");return;}
    try {
        auto op=jni_string(env,operation),body=jni_string(env,wire),context=jni_string(env,metadata);
        if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        auto response=static_cast<jstring>(env->CallStaticObjectMethod(ctx->cls,ctx->dispatch,
            ctx->transport,op,body,context,reinterpret_cast<jlong>(invocation)));
        if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        if(std::string(operation)=="request"){
            if(!response)throw std::runtime_error("Java MCP response is null");
            auto text=jni_utf8(env,response);
            if(libai_mcp_result_response(result,text.data(),text.size()))throw std::runtime_error("Java MCP response exceeds bounds");
        }
    }catch(const std::exception &e){if(env->ExceptionCheck())env->ExceptionClear();failure(result,e.what());}
    catch(...){if(env->ExceptionCheck())env->ExceptionClear();failure(result,"JNI MCP callback failed");}
    env->PopLocalFrame(nullptr);
}
void release(void *user){delete static_cast<Context*>(user);}
libai_mcp *client(jlong pointer){if(!pointer)throw std::invalid_argument("MCP client is null");return reinterpret_cast<libai_mcp*>(pointer);}
using OwnedText=std::unique_ptr<char,void(*)(char*)>;
}
extern "C" {
JNIEXPORT jlong JNICALL Java_io_coolder_libai_McpClient_nativeCreate(JNIEnv *env,jclass cls,jobject transport) {
    try {
        if(!transport)throw std::invalid_argument("MCP transport is null");
        std::unique_ptr<Context> ctx(new Context);
        if(env->GetJavaVM(&ctx->vm)!=JNI_OK)throw std::runtime_error("JNI VM unavailable");
        ctx->transport=env->NewGlobalRef(transport);
        if(!ctx->transport || env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        ctx->cls=static_cast<jclass>(env->NewGlobalRef(cls));
        if(!ctx->cls || env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        ctx->dispatch=env->GetStaticMethodID(ctx->cls,"dispatch","(Lio/coolder/libai/McpClient$Transport;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;");
        if(!ctx->dispatch || env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        auto pointer=libai_mcp_create(callback,ctx.get(),release);
        if(!pointer)throw std::runtime_error(libai_last_error());
        ctx.release();return reinterpret_cast<jlong>(pointer);
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP creation failed");}return 0;
}
JNIEXPORT jboolean JNICALL Java_io_coolder_libai_McpClient_nativeStdioAvailable(JNIEnv *,jclass){return libai_mcp_stdio_available()!=0;}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_McpClient_nativeStdio(JNIEnv *env,jclass,jstring options) {
    try {auto text=jni_utf8(env,options);auto pointer=libai_mcp_create_stdio(text.c_str());
        if(!pointer)throw std::runtime_error(libai_last_error());return reinterpret_cast<jlong>(pointer);
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP stdio creation failed");}return 0;
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_McpClient_nativeCall(JNIEnv *env,jclass,jlong pointer,jstring operation,jstring arguments,jlong token) {
    try {auto op=jni_utf8(env,operation),args=jni_utf8(env,arguments);
        OwnedText text(libai_mcp_call(client(pointer),op.c_str(),args.c_str(),reinterpret_cast<libai_cancellation*>(token)),libai_string_free);
        if(!text)throw std::runtime_error(libai_last_error());return jni_string(env,text.get());
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP operation failed");}return nullptr;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_McpClient_nativeClose(JNIEnv *env,jclass,jlong pointer) {
    try {if(libai_mcp_close(client(pointer)))throw std::runtime_error(libai_last_error());}
    catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP close failed");}
}
JNIEXPORT void JNICALL Java_io_coolder_libai_McpClient_nativeDestroy(JNIEnv *env,jclass,jlong pointer) {
    try {if(libai_mcp_destroy(client(pointer)))throw std::runtime_error(libai_last_error());}
    catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP destruction failed");}
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddMcpTool(JNIEnv *env,jobject,jlong pointer,jlong connection,jstring remote,jstring local,jstring policy) {
    try {auto *agent=reinterpret_cast<jni_agent_t*>(pointer);if(!agent)throw std::invalid_argument("agent is null");
        auto r=jni_utf8(env,remote),l=jni_utf8(env,local),p=jni_utf8(env,policy);
        if(libai_agent_add_mcp_tool(agent->agent,client(connection),r.c_str(),l.c_str(),p.c_str()))throw std::runtime_error(libai_last_error());
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI MCP binding failed");}
}
}
