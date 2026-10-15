/* Android adapter for the unchanged upstream portable touch state machine.
 * No game-version addresses or Apple UI/skill observation hooks are linked. */
#include "../ios/Sources/dm_touch_controls.h"
#include "input_backend.h"
#include "../host/dm.h"
#include <jni.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static dm_tc_state controls;
static struct {int active,id;} slots[16];
static int ready,enabled;
static uint64_t pulse_epoch,pulse_samples;
static double pulse_start,pulse_seen;
static uint64_t last_buttons;
static int32_t last_axes[4];
static int published;
static int access_slot=-1;
static double access_until;

static double seconds(void) {return now_ns()/1e9;}
static void restrict_layout(void) {
    /* These upstream widgets require an audited original-game bridge. Keep
     * them absent until 91738 has native binding/scene identities. */
    controls.layout.target[DM_TC_PORTAL].r=0;
    controls.layout.diff.r=0;
    for(int i=0;i<8;i++)controls.layout.pick[i].r=0;
    controls.belt_rows=1;
    dm_tc_enable_targets(&controls,((1u<<DM_TC_COUNT)-1)&~(1u<<DM_TC_PORTAL));
}
static int publish(void) {
    int32_t axes[4]={controls.lx,controls.ly,controls.rx,controls.ry};
    if(published && controls.buttons==last_buttons && !memcmp(axes,last_axes,sizeof axes))return 0;
    int rc=dm_android_input_publish_actions(controls.buttons,axes[0],axes[1],axes[2],axes[3]);
    if(!rc){last_buttons=controls.buttons;memcpy(last_axes,axes,sizeof axes);published=1;
        static unsigned logged;if(logged++<128)LOG("ANDROID_UPSTREAM_TOUCH buttons=%#llx axes=%d,%d,%d,%d set=%d",(unsigned long long)controls.buttons,axes[0],axes[1],axes[2],axes[3],controls.skill_set);}
    return rc;
}
static void cancel(void) {
    if(ready)dm_tc_cancel_all(&controls);
    memset(slots,0,sizeof slots);pulse_epoch=0;published=0;
    access_slot=-1;
    dm_android_input_cancel();
}
static void tick(void) {
    if(!ready || !enabled)return;
    double now=seconds();dm_android_input_status input;dm_android_input_status_get(&input);
    if(access_slot>=0 && now>=access_until){dm_tc_end_at(&controls,access_slot,now);access_slot=-1;}
    dm_tc_tick(&controls,now);
    if(controls.pulse_button) {
        if(pulse_epoch!=controls.pulse_epoch){pulse_epoch=controls.pulse_epoch;pulse_samples=input.npad_samples;pulse_start=now;pulse_seen=0;}
        if(!pulse_seen && input.npad_samples>pulse_samples)pulse_seen=now;
        double hold=fmax(0.12,controls.pulse_hold_s);
        if((pulse_seen && now-pulse_seen>=hold && input.npad_samples-pulse_samples>=3) || now-pulse_start>2.0+hold) {
            dm_tc_pulse_end(&controls,pulse_epoch);pulse_samples=input.npad_samples;pulse_start=now;
        }
    }else if(dm_tc_pulse_pending(&controls) && (input.npad_samples-pulse_samples>=2 || now-pulse_start>0.5))dm_tc_pulse_resume(&controls);
    (void)publish();
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeResize(JNIEnv *env,jclass cls,jfloat w,jfloat h,jfloat left,jfloat right) {
    (void)env;(void)cls;if(!isfinite(w)||!isfinite(h)||w<=0||h<=0)return;
    pthread_mutex_lock(&lock);dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(left,right);
    if(ready){cancel();dm_tc_resize(&controls,w,h);}else{dm_tc_init(&controls,w,h);ready=1;}
    restrict_layout();pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEnable(JNIEnv *env,jclass cls,jboolean on) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);cancel();enabled=on!=0;pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeCancel(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);cancel();pthread_mutex_unlock(&lock);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEvent(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;if(!ids||!xs||!ys)return EINVAL;
    int n=(*env)->GetArrayLength(env,ids);
    if(n>16||(*env)->GetArrayLength(env,xs)!=n||(*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint id[16];jfloat x[16],y[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,id);(*env)->GetFloatArrayRegion(env,xs,0,n,x);(*env)->GetFloatArrayRegion(env,ys,0,n,y);
    if((*env)->ExceptionCheck(env))return EINVAL;
    for(int i=0;i<n;i++){if(!isfinite(x[i])||!isfinite(y[i]))return EINVAL;for(int j=0;j<i;j++)if(id[i]==id[j])return EINVAL;}
    pthread_mutex_lock(&lock);
    if(cancelled){cancel();pthread_mutex_unlock(&lock);return 0;}
    if(!ready||!enabled){pthread_mutex_unlock(&lock);return ENODEV;}
    double now=seconds();
    for(int slot=0;slot<16;slot++)if(slots[slot].active){int seen=0;for(int i=0;i<n;i++)seen|=slots[slot].id==id[i];
        if(!seen){dm_tc_end_at(&controls,slot,now);slots[slot].active=0;}}
    dm_android_pointer raw[16];int nr=0;
    for(int i=0;i<n;i++) {
        int slot=0;while(slot<16&&(!slots[slot].active||slots[slot].id!=id[i]))slot++;
        float px=x[i]*controls.layout.width,py=y[i]*controls.layout.height;
        if(slot==16){slot=0;while(slot<16&&(slots[slot].active||controls.finger[slot].active))slot++;if(slot==16){pthread_mutex_unlock(&lock);return EOVERFLOW;}
            slots[slot].active=1;slots[slot].id=id[i];dm_tc_begin_at(&controls,slot,px,py,now);
        }else dm_tc_move_at(&controls,slot,px,py,now);
        if(controls.finger[slot].target==DM_TC_NONE)raw[nr++]=(dm_android_pointer){.id=(uint32_t)id[i],.x=x[i],.y=y[i]};
    }
    tick();int rc=dm_android_input_publish_touch(raw,(size_t)nr);if(!rc)rc=publish();
    pthread_mutex_unlock(&lock);return rc;
}
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeFrame(JNIEnv *env,jclass cls) {
    (void)cls;jfloat data[58]={0};pthread_mutex_lock(&lock);tick();
    for(int i=0;i<DM_TC_COUNT;i++){dm_tc_target t=controls.layout.target[i];data[i*3]=t.x;data[i*3+1]=t.y;data[i*3+2]=t.r;
        for(int j=0;j<16;j++)if(controls.finger[j].active&&controls.finger[j].target==i)data[44+i]=1;}
    data[36]=controls.layout.joy_x;data[37]=controls.layout.joy_y;data[38]=controls.layout.joy_radius;
    data[39]=controls.layout.swap.x;data[40]=controls.layout.swap.y;data[41]=controls.layout.swap.r;
    data[42]=data[36]+controls.lx/32767.f*data[38];data[43]=data[37]-controls.ly/32767.f*data[38];
    data[56]=(float)controls.skill_set;data[57]=controls.joy_slot!=DM_TC_NONE;
    pthread_mutex_unlock(&lock);jfloatArray out=(*env)->NewFloatArray(env,58);if(out)(*env)->SetFloatArrayRegion(env,out,0,58,data);return out;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeActivate(JNIEnv *env,jclass cls,jint target) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);
    if(ready&&enabled){int slot=15;while(slot>=0&&(slots[slot].active||controls.finger[slot].active))slot--;
        if(slot>=0){double now=seconds();float x,y;
            if(target>=0&&target<DM_TC_COUNT){x=controls.layout.target[target].x;y=controls.layout.target[target].y;}
            else if(target==12){x=controls.layout.swap.x;y=controls.layout.swap.y;}
            else {x=controls.layout.joy_x;y=controls.layout.joy_y;}
            dm_tc_begin_at(&controls,slot,x,y,now);
            if(target>=20&&target<=23){float radius=controls.layout.joy_radius;dm_tc_move_at(&controls,slot,x+(target==20?-radius:target==21?radius:0),y+(target==22?-radius:target==23?radius:0),now);access_slot=slot;access_until=now+0.25;}
            else dm_tc_end_at(&controls,slot,now+0.01);
            tick();
        }
    }
    pthread_mutex_unlock(&lock);
}
