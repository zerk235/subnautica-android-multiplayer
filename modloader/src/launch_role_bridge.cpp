#include "launch_role_bridge.h"
#include "net.h"
#include "log.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {
JavaVM* machine=nullptr;
jclass unity=nullptr;
jfieldID activity_field=nullptr;
bool pending=false;
SessionLaunchRole requested=kLaunchAuto;
char last_request[96]{};
constexpr const char* directory="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_config";
constexpr const char* preference="/sdcard/Android/data/com.unknownworlds.subnautica/files/mod_config/session_role.txt";
bool clear_exception(JNIEnv* env) {if(!env->ExceptionCheck())return false;env->ExceptionClear();return true;}
bool read_extra(JNIEnv* env,jobject intent,jmethodID getter,const char* key,char* out,size_t capacity) {
    jstring name=env->NewStringUTF(key);
    jstring value=static_cast<jstring>(env->CallObjectMethod(intent,getter,name));
    env->DeleteLocalRef(name);
    if(clear_exception(env) || !value)return false;
    const char* bytes=env->GetStringUTFChars(value,nullptr);
    if(!bytes){env->DeleteLocalRef(value);clear_exception(env);return false;}
    bool ok=std::strlen(bytes)<capacity;
    if(ok)std::snprintf(out,capacity,"%s",bytes);
    env->ReleaseStringUTFChars(value,bytes);env->DeleteLocalRef(value);
    return ok;
}
void persist(SessionLaunchRole role) {
    mkdir(directory,0770);
    char temp[512];std::snprintf(temp,sizeof(temp),"%s.tmp",preference);
    FILE* file=std::fopen(temp,"w");
    if(!file){LOG_WARN("launch role: preference is only active for this process");return;}
    bool ok=std::fprintf(file,"%d\n",int(role))>0 && std::fflush(file)==0 && fsync(fileno(file))==0;
    if(std::fclose(file)!=0)ok=false;
    if(!ok || std::rename(temp,preference)!=0){unlink(temp);LOG_WARN("launch role: preference could not be committed");}
}
void poll_intent() {
    if(!machine || !unity || !activity_field)return;
    JNIEnv* env=nullptr;bool attached=false;
    jint state=machine->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if(machine->AttachCurrentThread(&env,nullptr)!=JNI_OK)return;attached=true;}
    else if(state!=JNI_OK)return;
    if(env->PushLocalFrame(16)==0) {
        jobject activity=env->GetStaticObjectField(unity,activity_field);
        if(!clear_exception(env) && activity) {
            jclass activity_class=env->GetObjectClass(activity);
            jmethodID get_intent=env->GetMethodID(activity_class,"getIntent","()Landroid/content/Intent;");
            if(!clear_exception(env) && get_intent) {
                jobject intent=env->CallObjectMethod(activity,get_intent);
                if(!clear_exception(env) && intent) {
                    jclass intent_class=env->GetObjectClass(intent);
                    jmethodID getter=env->GetMethodID(intent_class,"getStringExtra","(Ljava/lang/String;)Ljava/lang/String;");
                    char nonce[96]{},role[16]{};
                    if(!clear_exception(env) && getter &&
                        read_extra(env,intent,getter,"snmp_launch_request",nonce,sizeof(nonce)) && nonce[0] &&
                        std::strcmp(nonce,last_request)!=0 && read_extra(env,intent,getter,"snmp_launch_role",role,sizeof(role))) {
                        SessionLaunchRole next=kLaunchAuto;
                        bool valid=!std::strcmp(role,"auto");
                        if(!std::strcmp(role,"host")){next=kLaunchHost;valid=true;}
                        if(!std::strcmp(role,"guest")){next=kLaunchGuest;valid=true;}
                        std::snprintf(last_request,sizeof(last_request),"%s",nonce);
                        if(valid){requested=next;pending=true;LOG_INFO("launch role: request received role=%s; waiting for main menu",role);}
                    }
                }
            }
        }
        clear_exception(env);env->PopLocalFrame(nullptr);
    } else clear_exception(env);
    if(attached)machine->DetachCurrentThread();
}
}

void launch_role_bridge_init(JavaVM* vm,JNIEnv* env) {
    machine=vm;
    jclass local=env->FindClass("com/unity3d/player/UnityPlayer");
    if(!clear_exception(env) && local) {
        unity=static_cast<jclass>(env->NewGlobalRef(local));env->DeleteLocalRef(local);
        if(unity)activity_field=env->GetStaticFieldID(unity,"currentActivity","Landroid/app/Activity;");
        if(clear_exception(env))activity_field=nullptr;
    }
    int role=0;FILE* file=std::fopen(preference,"r");
    if(file){if(std::fscanf(file,"%d",&role)==1 && role>=0 && role<=2){requested=SessionLaunchRole(role);pending=true;}std::fclose(file);}
    launch_role_bridge_tick(true);
}

void launch_role_bridge_tick(bool at_menu) {
    // SaveLoadManager.Update runs on Unity's attached game thread. Avoid JNI work
    // every frame; no Android view or gameplay object is modified here.
    static unsigned frame=0;
    if((frame++%30)==0)poll_intent();
    if(pending && at_menu && net_session_choose_role(requested)) {
        persist(requested);pending=false;
        LOG_INFO("launch role: applied in menu role=%s",requested==kLaunchHost ? "host" : requested==kLaunchGuest ? "guest" : "auto");
    }
}
