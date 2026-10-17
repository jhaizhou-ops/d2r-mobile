#include "game_keyboard.h"
#include "keyboard_wait_state.h"
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>
#include <atomic>
#include <time.h>
namespace {
std::atomic<unsigned> receiptCount{0};
void Receipt(const char *event,int id,int result){if(receiptCount.fetch_add(1)>=64)return;timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);fprintf(stderr,"[frontend-keyboard] event=%s id=%d result=%d monotonic_ns=%llu\n",event,id,result,(unsigned long long)t.tv_sec*1000000000ULL+t.tv_nsec);}
struct Request : XblKeyboardWaitState {
    std::mutex mutex;std::condition_variable answered;
    int maximum=0,capacity=0;
    std::string title,subtitle,guide,initial,value;
} request;
int Keyboard(void *,const char *title,const char *subtitle,const char *guide,const char *initial,int maximum,char *output,int bytes){
    Receipt("callback-entry",0,0);
    if(!output||bytes<1||maximum<0||maximum>65536)return -1;
    std::unique_lock<std::mutex> lock(request.mutex);const int begun=request.begin();if(begun!=2)return begun;
    request.maximum=maximum;request.capacity=bytes;
    request.title=title?title:"";request.subtitle=subtitle?subtitle:"";request.guide=guide?guide:"";request.initial=initial?initial:"";request.value.clear();
    Receipt("pending",request.id,0);
    const int result=request.waitResult([&](auto ready){return request.answered.wait_for(lock,std::chrono::minutes(5),ready);});
    Receipt("callback-return",request.id,result);
    if(result==1){if(request.value.size()>=size_t(bytes))return -1;memcpy(output,request.value.c_str(),request.value.size()+1);}
    return result;
}
void Text(napi_env env,napi_value object,const char *key,const std::string &text){napi_value value{};napi_create_string_utf8(env,text.c_str(),text.size(),&value);napi_set_named_property(env,object,key,value);}
void Number(napi_env env,napi_value object,const char *key,int value){napi_value number{};napi_create_int32(env,value,&number);napi_set_named_property(env,object,key,number);}
}
int RegisterGameKeyboard(void *runtime){
    using Handler=int(*)(void *,const char *,const char *,const char *,const char *,int,char *,int);
    auto set=reinterpret_cast<int(*)(Handler,void*)>(dlsym(runtime,"xblr_set_keyboard_handler"));
    const int rc=set?set(Keyboard,nullptr):-1;Receipt("registered",0,rc);return rc;
}
void CancelGameKeyboard(bool runtimeStarted){
    std::lock_guard<std::mutex> lock(request.mutex);
    if(request.close(runtimeStarted))request.answered.notify_all();
}
napi_value GetGameKeyboard(napi_env env,napi_callback_info){
    std::lock_guard<std::mutex> lock(request.mutex);napi_value result{};
    if(!request.pending||request.complete){napi_get_null(env,&result);return result;}
    static int polled=0;if(polled!=request.id){polled=request.id;Receipt("polled",request.id,0);}
    napi_create_object(env,&result);Number(env,result,"id",request.id);Number(env,result,"maximum",request.maximum);
    Text(env,result,"title",request.title);Text(env,result,"subtitle",request.subtitle);Text(env,result,"guide",request.guide);Text(env,result,"initial",request.initial);return result;
}
napi_value AnswerGameKeyboard(napi_env env,napi_callback_info info){
    size_t n=3;napi_value args[3]{};int32_t id=0;bool confirmed=false;size_t utf16=0,bytes=0;
    if(napi_get_cb_info(env,info,&n,args,nullptr,nullptr)!=napi_ok||n!=3||napi_get_value_int32(env,args[0],&id)!=napi_ok||napi_get_value_bool(env,args[1],&confirmed)!=napi_ok||napi_get_value_string_utf16(env,args[2],nullptr,0,&utf16)!=napi_ok||napi_get_value_string_utf8(env,args[2],nullptr,0,&bytes)!=napi_ok){napi_throw_error(env,nullptr,"Invalid keyboard response");return nullptr;}
    std::lock_guard<std::mutex> lock(request.mutex);
    if(!request.accepts(id)){napi_throw_error(env,nullptr,"Keyboard request expired");return nullptr;}
    if(confirmed&&(utf16>size_t(request.maximum)||bytes>=size_t(request.capacity))){napi_throw_error(env,nullptr,"输入内容过长");return nullptr;}
    if(confirmed){std::string value(bytes+1,'\0');size_t copied=0;napi_get_value_string_utf8(env,args[2],value.data(),value.size(),&copied);value.resize(copied);request.value=value;}
    Receipt("reply",id,confirmed?1:0);
    request.answer(id,confirmed?1:0);request.answered.notify_one();napi_value result{};napi_get_undefined(env,&result);return result;
}
