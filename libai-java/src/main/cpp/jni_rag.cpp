#include "jni_internal.h"
#include "jni_thread.h"
#include <memory>
#include <algorithm>
#include <stdexcept>
namespace {
using libai_jni::Env;
using OwnedText=std::unique_ptr<char,void(*)(char*)>;
struct Host {
    JavaVM *vm=nullptr;jobject value=nullptr;jclass cls=nullptr;jmethodID dispatch=nullptr;
    ~Host(){Env current(vm);if(current.env){if(value)current.env->DeleteGlobalRef(value);if(cls)current.env->DeleteGlobalRef(cls);}}
};
void failure(libai_rag_result *result,const char *text)noexcept {
    try{std::string value=text?text:"JNI RAG callback failed";auto end=std::min<std::size_t>(4096,value.size());if(end<value.size())while(end&&(static_cast<unsigned char>(value[end])&0xc0)==0x80)--end;value.resize(end);libai_rag_result_error(result,value.empty()?"JNI RAG callback failed":value.c_str());}
    catch(...){libai_rag_result_error(result,"JNI RAG callback failed");}
}
void callback(void *user,const char *operation,const char *arguments,const char *context,libai_rag_result *result,libai_invocation *invocation){
    auto *host=static_cast<Host*>(user);Env current(host->vm);auto *env=current.env;
    if(!env){failure(result,"JNI RAG thread unavailable");return;}
    if(env->PushLocalFrame(16)<0){env->ExceptionClear();failure(result,"JNI RAG frame failed");return;}
    try{auto op=jni_string(env,operation),args=jni_string(env,arguments),metadata=jni_string(env,context);if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        auto response=static_cast<jstring>(env->CallStaticObjectMethod(host->cls,host->dispatch,host->value,op,args,metadata,reinterpret_cast<jlong>(invocation)));
        if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));if(!response)throw std::runtime_error("Java RAG output null");auto wire=jni_utf8(env,response);if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        if(libai_rag_result_set(result,wire.data(),wire.size()))throw std::runtime_error("Java RAG result invalid or too large");
    }catch(const std::exception &e){if(env->ExceptionCheck())env->ExceptionClear();failure(result,e.what());}
    catch(...){if(env->ExceptionCheck())env->ExceptionClear();failure(result,"JNI RAG callback failed");}
    env->PopLocalFrame(nullptr);
}
void release(void *user){delete static_cast<Host*>(user);}
libai_rag *component(jlong pointer){if(!pointer)throw std::invalid_argument("RAG component null");return reinterpret_cast<libai_rag*>(pointer);}
}
extern "C" {
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Rag_nativeCreate(JNIEnv *env,jclass cls,jstring kind,jobject value,jstring configuration){
    try{auto type=jni_utf8(env,kind),config=jni_utf8(env,configuration);if(!value)throw std::invalid_argument("RAG host null");std::unique_ptr<Host> host(new Host);
        if(env->GetJavaVM(&host->vm)!=JNI_OK)throw std::runtime_error("JNI VM unavailable");host->value=env->NewGlobalRef(value);if(!host->value||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        host->cls=static_cast<jclass>(env->NewGlobalRef(cls));if(!host->cls||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        host->dispatch=env->GetStaticMethodID(host->cls,"dispatch","(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;");if(!host->dispatch||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        auto *result=libai_rag_create(type.c_str(),callback,host.get(),release,config.c_str());if(!result)throw std::runtime_error(libai_last_error());host.release();return reinterpret_cast<jlong>(result);
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI RAG creation failed");}return 0;
}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Rag_nativeBuiltin(JNIEnv *env,jclass,jstring kind,jstring configuration,jlong embedding,jlong store){
    try{auto type=jni_utf8(env,kind),config=jni_utf8(env,configuration);libai_rag *result=nullptr;
        if(type=="splitter")result=libai_rag_create_splitter(config.c_str());else if(type=="store")result=libai_rag_create_memory_store(config.c_str());else if(type=="retriever")result=libai_rag_create_vector_retriever(component(embedding),component(store),config.c_str());else throw std::invalid_argument("RAG kind unknown");
        if(!result)throw std::runtime_error(libai_last_error());return reinterpret_cast<jlong>(result);
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI RAG builtin failed");}return 0;
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Rag_nativeCall(JNIEnv *env,jclass,jlong pointer,jstring operation,jstring input,jstring context,jstring limits,jlong other,jlong token){
    try{auto op=jni_utf8(env,operation),data=jni_utf8(env,input),ctx=jni_utf8(env,context),policy=jni_utf8(env,limits);auto *cancel=reinterpret_cast<libai_cancellation*>(token);char *result=nullptr;
        if(op=="load")result=libai_rag_load(component(pointer),data.c_str(),ctx.c_str(),cancel);
        else if(op=="prepare")result=libai_rag_prepare_index(component(pointer),component(other),data.c_str(),ctx.c_str(),policy.c_str(),cancel);
        else if(op=="retrieve")result=libai_rag_retrieve(component(pointer),data.c_str(),ctx.c_str(),cancel);
        else if(op=="commit"){if(libai_rag_commit_index(component(pointer),data.c_str(),ctx.c_str(),cancel))throw std::runtime_error(libai_last_error());return jni_string(env,"{}");}
        else throw std::invalid_argument("RAG operation unknown");OwnedText owned(result,libai_string_free);if(!owned)throw std::runtime_error(libai_last_error());return jni_string(env,owned.get());
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI RAG operation failed");}return nullptr;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Rag_nativeDestroy(JNIEnv *env,jclass,jlong pointer){
    try{if(libai_rag_destroy(component(pointer)))throw std::runtime_error(libai_last_error());}catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI RAG close failed");}
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Agent_nativeAddRetrievalTool(JNIEnv *env,jobject,jlong pointer,jlong retriever,jstring name,jstring binding){
    try{auto *agent=reinterpret_cast<jni_agent_t*>(pointer);if(!agent)throw std::invalid_argument("Agent null");auto n=jni_utf8(env,name),policy=jni_utf8(env,binding);if(libai_agent_add_retrieval_tool(agent->agent,component(retriever),n.c_str(),policy.c_str()))throw std::runtime_error(libai_last_error());}
    catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI RAG binding failed");}
}
}
