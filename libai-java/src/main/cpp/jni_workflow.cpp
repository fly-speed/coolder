#include "jni_internal.h"
#include "jni_thread.h"
#include <memory>
#include <algorithm>
#include <stdexcept>
namespace {
using libai_jni::Env;
using OwnedText=std::unique_ptr<char,void(*)(char*)>;
struct Node {
    JavaVM *vm=nullptr;jobject handler=nullptr;jclass cls=nullptr;jmethodID dispatch=nullptr;
    ~Node(){Env current(vm);if(current.env){if(handler)current.env->DeleteGlobalRef(handler);if(cls)current.env->DeleteGlobalRef(cls);}}
};
std::string quote(const std::string &value){std::string out="\"";const char *hex="0123456789abcdef";
    for(unsigned char c:value){if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=c;}return out+'"';
}
void failure(libai_workflow_result *result,const char *diagnostic)noexcept{
    try{std::string message=diagnostic?diagnostic:"JNI workflow callback failed";std::size_t end=std::min<std::size_t>(4096,message.size());if(end<message.size())while(end&&(static_cast<unsigned char>(message[end])&0xc0)==0x80)--end;
        auto wire="{\"error\":"+quote(message.substr(0,end).empty()?"JNI workflow callback failed":message.substr(0,end))+"}";
        libai_workflow_result_set(result,wire.data(),wire.size());
    }catch(...){const char *wire="{\"error\":\"JNI workflow callback failed\"}";libai_workflow_result_set(result,wire,std::char_traits<char>::length(wire));}
}
void callback(void *user,const char *state,const char *metadata,libai_workflow_result *result,libai_invocation *invocation){
    auto *node=static_cast<Node*>(user);Env current(node->vm);auto *env=current.env;
    if(!env){failure(result,"JNI workflow thread unavailable");return;}
    if(env->PushLocalFrame(16)<0){env->ExceptionClear();failure(result,"JNI workflow frame failed");return;}
    try{auto input=jni_string(env,state),context=jni_string(env,metadata);if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        auto response=static_cast<jstring>(env->CallStaticObjectMethod(node->cls,node->dispatch,node->handler,input,context,reinterpret_cast<jlong>(invocation)));
        if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));if(!response)throw std::runtime_error("Java workflow output null");
        auto wire=jni_utf8(env,response);if(env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        if(libai_workflow_result_set(result,wire.data(),wire.size()))throw std::runtime_error("Java workflow output invalid/too large");
    }catch(const std::exception &e){if(env->ExceptionCheck())env->ExceptionClear();failure(result,e.what());}
    catch(...){if(env->ExceptionCheck())env->ExceptionClear();failure(result,"JNI workflow callback failed");}
    env->PopLocalFrame(nullptr);
}
void release(void *user){delete static_cast<Node*>(user);}
struct Store {JNIEnv *env;jobject value;};
void store(void *user,const char *op,const char *scope,const char *revision,const char *state,libai_execution_result *result){
    auto *holder=static_cast<Store*>(user);jni_store_dispatch(holder->env,holder->value,op,scope,revision,state,result);
}
libai_workflow *graph(jlong pointer){if(!pointer)throw std::invalid_argument("workflow is null");return reinterpret_cast<libai_workflow*>(pointer);}
}
extern "C" {
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Workflow_nativeCreate(JNIEnv *env,jclass){
    auto *value=libai_workflow_create();if(!value){jni_throw(env,libai_last_error());return 0;}return reinterpret_cast<jlong>(value);
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Workflow_nativeAddNode(JNIEnv *env,jclass cls,jlong pointer,jstring spec,jobject handler){
    try{auto wire=jni_utf8(env,spec);std::unique_ptr<Node> node(new Node);
        if(env->GetJavaVM(&node->vm)!=JNI_OK)throw std::runtime_error("JNI VM unavailable");
        node->handler=env->NewGlobalRef(handler);if(!node->handler||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        node->cls=static_cast<jclass>(env->NewGlobalRef(cls));if(!node->cls||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        node->dispatch=env->GetStaticMethodID(node->cls,"dispatch","(Lio/coolder/libai/Workflow$Node;Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;");
        if(!node->dispatch||env->ExceptionCheck())throw std::runtime_error(jni_error(env));
        if(libai_workflow_add_node(graph(pointer),wire.c_str(),callback,node.get(),release))throw std::runtime_error(libai_last_error());node.release();
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI workflow registration failed");}
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Workflow_nativeAddEdge(JNIEnv *env,jclass,jlong pointer,jstring from,jstring to,jstring label){
    try{auto f=jni_utf8(env,from),t=jni_utf8(env,to),l=jni_utf8(env,label);if(libai_workflow_add_edge(graph(pointer),f.c_str(),t.c_str(),l.c_str()))throw std::runtime_error(libai_last_error());}
    catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI workflow edge failed");}
}
JNIEXPORT jstring JNICALL Java_io_coolder_libai_Workflow_nativeRun(JNIEnv *env,jclass,jlong pointer,jstring initial,jstring options,jobject storage,jlong token){
    try{auto input=jni_utf8(env,initial),wire=jni_utf8(env,options);Store holder{env,storage};
        std::unique_ptr<libai_cancellation,void(*)(libai_cancellation*)> fresh(token?nullptr:libai_cancellation_create(),libai_cancellation_destroy);
        if(!token&&!fresh)throw std::runtime_error(libai_last_error());
        OwnedText result(libai_workflow_run(graph(pointer),input.c_str(),wire.c_str(),storage?store:nullptr,&holder,token?reinterpret_cast<libai_cancellation*>(token):fresh.get()),libai_string_free);
        if(!result)throw std::runtime_error(libai_last_error());return jni_string(env,result.get());
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI workflow run failed");}return nullptr;
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Workflow_nativeConfirm(JNIEnv *env,jclass,jlong pointer,jobject storage,jstring scope,jstring initial,jstring revision,jstring name,jstring outcome,jstring output,jstring reference){
    try{auto s=jni_utf8(env,scope),i=jni_utf8(env,initial),r=jni_utf8(env,revision),n=jni_utf8(env,name),o=jni_utf8(env,outcome),value=jni_utf8(env,output),ref=jni_utf8(env,reference);Store holder{env,storage};
        if(libai_workflow_confirm_node(graph(pointer),store,&holder,s.c_str(),i.c_str(),r.c_str(),n.c_str(),o.c_str(),value.c_str(),ref.c_str()))throw std::runtime_error(libai_last_error());
    }catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI workflow receipt failed");}
}
JNIEXPORT void JNICALL Java_io_coolder_libai_Workflow_nativeDestroy(JNIEnv *env,jclass,jlong pointer){
    try{if(libai_workflow_destroy(graph(pointer)))throw std::runtime_error(libai_last_error());}
    catch(const std::exception &e){jni_throw(env,e.what());}catch(...){jni_throw(env,"JNI workflow close failed");}
}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeRemainingMs(JNIEnv *,jclass,jlong pointer){return libai_invocation_remaining_ms(reinterpret_cast<libai_invocation*>(pointer));}
JNIEXPORT jlong JNICALL Java_io_coolder_libai_Agent_nativeInvocationToken(JNIEnv *,jclass,jlong pointer){return reinterpret_cast<jlong>(libai_invocation_retain_cancellation(reinterpret_cast<libai_invocation*>(pointer)));}
}
