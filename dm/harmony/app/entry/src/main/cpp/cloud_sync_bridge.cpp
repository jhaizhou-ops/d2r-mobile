// Save sync bridge: the unchanged iOS core (cloud/sw_cloudsync.c, "local save dir <-> one cloud dir", one pack per
// character, later write wins, the overwritten copy goes to a backup dir) driven from ArkTS. On HarmonyOS the "cloud dir"
// is the app's distributed files dir, which the system keeps in sync across devices on the same HUAWEI ID.
#include "cloud_sync_bridge.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
extern "C" int sw_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit));
namespace {
std::mutex syncMutex;
bool ArgString(napi_env env,napi_value v,std::string &out){
    size_t n=0;if(napi_get_value_string_utf8(env,v,nullptr,0,&n)!=napi_ok||n==0||n>4096)return false;
    out.assign(n+1,'\0');size_t got=0;if(napi_get_value_string_utf8(env,v,&out[0],out.size(),&got)!=napi_ok)return false;out.resize(got);return out[0]=='/';
}
uint64_t NowMs(){timespec t{};clock_gettime(CLOCK_REALTIME,&t);return uint64_t(t.tv_sec)*1000+uint64_t(t.tv_nsec)/1000000;}
}
// cloudSync(saveDir, cloudDir, backupDir, stateFile, allowDown) -> number of actions, or -1.
napi_value CloudSync(napi_env env,napi_callback_info info){
    size_t n=5;napi_value a[5]{};std::string save,cloud,bak,state;bool down=false;
    if(napi_get_cb_info(env,info,&n,a,nullptr,nullptr)!=napi_ok||n!=5||!ArgString(env,a[0],save)||!ArgString(env,a[1],cloud)||
       !ArgString(env,a[2],bak)||!ArgString(env,a[3],state)||napi_get_value_bool(env,a[4],&down)!=napi_ok){napi_throw_error(env,nullptr,"Invalid cloud sync arguments");return nullptr;}
    int rc=-1;{
        std::lock_guard<std::mutex> lock(syncMutex);
        mkdir(cloud.c_str(),0770);mkdir(bak.c_str(),0770);
        struct stat st{};if(!stat(save.c_str(),&st)&&S_ISDIR(st.st_mode))rc=sw_cloud_sync(save.c_str(),cloud.c_str(),bak.c_str(),state.c_str(),down?1:0,NowMs(),nullptr);
        fprintf(stderr,"[cloud] sync allowDown=%d rc=%d\n",down?1:0,rc);
    }
    napi_value r{};napi_create_int32(env,rc,&r);return r;
}
