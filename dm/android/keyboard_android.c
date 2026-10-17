#include "../host/dm.h"
#include "../host/front.h"
#include <jni.h>

extern size_t swkbd_u16_to_u8(const uint16_t *,size_t,char *,size_t);
extern size_t swkbd_u8_to_u16(const char *,size_t,uint16_t *,size_t);
static pthread_mutex_t keyboard_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t keyboard_changed=PTHREAD_COND_INITIALIZER;
static int ready,pending,accepted,minimum,maximum;
static uint64_t request_id;
static char title_text[256],subtitle[512],hint[1024],initial_text[2048],reply[4096];
static _Thread_local int requested_minimum;
void dm_android_keyboard_constraints(int min) {requested_minimum=min>0?min:0;}
/* dm_ui_text_entered 由 dm_cursor.c 提供（P2 起编入）：起名页输入完成自动接受 | dm_ui_text_entered comes from dm_cursor.c (compiled since P2) */
int front_text_input(const char *title,const char *sub,const char *guide,const char *initial,int max_chars,char *out,int cap) {
    pthread_mutex_lock(&keyboard_lock);
    if(!ready||pending){pthread_mutex_unlock(&keyboard_lock);return -1;}
    snprintf(title_text,sizeof(title_text),"%s",title);snprintf(subtitle,sizeof(subtitle),"%s",sub);
    snprintf(hint,sizeof(hint),"%s",guide);snprintf(initial_text,sizeof(initial_text),"%s",initial);
    minimum=requested_minimum;maximum=max_chars>0&&max_chars<1002?max_chars:1002;
    pending=1;accepted=0;reply[0]=0;request_id++;
    LOG("ANDROID_KEYBOARD_REQUEST id=%llu min=%d max=%d",(unsigned long long)request_id,minimum,maximum);
    while(pending)pthread_cond_wait(&keyboard_changed,&keyboard_lock);
    int result=accepted;if(result&&cap>0)snprintf(out,cap,"%s",reply);
    pthread_mutex_unlock(&keyboard_lock);return result;
}
static void append_json(char **cursor,size_t *left,const char *text) {
    if(*left<3)die("keyboard JSON capacity");*(*cursor)++='"';(*left)--;
    for(const unsigned char *s=(const unsigned char *)text;*s;s++){
        if(*left<8)die("keyboard JSON capacity");
        if(*s=='"'||*s=='\\'){*(*cursor)++='\\';*(*cursor)++=*s;*left-=2;}
        else if(*s<32){int n=snprintf(*cursor,*left,"\\u%04x",*s);*cursor+=n;*left-=n;}
        else {*(*cursor)++=*s;(*left)--;}
    }
    *(*cursor)++='"';(*left)--;**cursor=0;
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeKeyboardRequest(JNIEnv *env,jclass cls) {
    (void)cls;pthread_mutex_lock(&keyboard_lock);
    if(!pending){pthread_mutex_unlock(&keyboard_lock);return NULL;}
    char json[8192];int n=snprintf(json,sizeof(json),"{\"id\":%llu,\"min\":%d,\"max\":%d,\"title\":",(unsigned long long)request_id,minimum,maximum);
    char *cursor=json+n;size_t left=sizeof(json)-n;append_json(&cursor,&left,title_text);
    const char *keys[]={",\"subtitle\":",",\"hint\":",",\"initial\":"};const char *values[]={subtitle,hint,initial_text};
    for(unsigned i=0;i<3;i++){n=snprintf(cursor,left,"%s",keys[i]);cursor+=n;left-=n;append_json(&cursor,&left,values[i]);}
    *cursor++='}';*cursor=0;uint16_t utf16[8192];size_t units=swkbd_u8_to_u16(json,8191,utf16,8192);
    jstring result=(*env)->NewString(env,(const jchar *)utf16,(jsize)units);pthread_mutex_unlock(&keyboard_lock);return result;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeKeyboardResult(JNIEnv *env,jclass cls,jlong id,jboolean ok,jstring text) {
    (void)cls;pthread_mutex_lock(&keyboard_lock);
    if(pending&&(uint64_t)id==request_id){
        accepted=0;
        if(ok&&text){const jchar *chars=(*env)->GetStringChars(env,text,NULL);
            if(chars){jsize count=(*env)->GetStringLength(env,text);swkbd_u16_to_u8((const uint16_t *)chars,count,reply,sizeof(reply));(*env)->ReleaseStringChars(env,text,chars);accepted=1;}}
        pending=0;pthread_cond_broadcast(&keyboard_changed);
        LOG("ANDROID_KEYBOARD_RESULT id=%llu accepted=%d",(unsigned long long)request_id,accepted);
    }
    pthread_mutex_unlock(&keyboard_lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeKeyboardReady(JNIEnv *env,jclass cls,jboolean on) {
    (void)env;(void)cls;pthread_mutex_lock(&keyboard_lock);ready=on;
    if(!ready&&pending){pending=accepted=0;pthread_cond_broadcast(&keyboard_changed);LOG("ANDROID_KEYBOARD_LIFECYCLE_CANCEL");}
    pthread_mutex_unlock(&keyboard_lock);
}
