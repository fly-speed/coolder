#pragma once
#include <jni.h>
namespace libai_jni {
// Android and desktop JDKs expose different AttachCurrentThread wrapper types.
template<class VM>auto attach(VM *vm,JNIEnv **env,int)->decltype(vm->AttachCurrentThread(env,nullptr)) {
    return vm->AttachCurrentThread(env,nullptr);
}
template<class VM>auto attach(VM *vm,JNIEnv **env,long)->decltype(vm->AttachCurrentThread(reinterpret_cast<void**>(env),nullptr)) {
    return vm->AttachCurrentThread(reinterpret_cast<void**>(env),nullptr);
}
// Never share JNIEnv across threads; detach only a thread attached by this owner.
struct Env {
    JavaVM *vm;JNIEnv *env=nullptr;bool attached=false;
    explicit Env(JavaVM *value):vm(value){
        if(!vm)return;
        auto result=vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
        if(result==JNI_EDETACHED){if(attach(vm,&env,0)==JNI_OK)attached=true;else env=nullptr;}
        else if(result!=JNI_OK)env=nullptr;
    }
    ~Env(){if(attached)vm->DetachCurrentThread();}
    Env(const Env&)=delete;Env &operator=(const Env&)=delete;
};
}
