#include <jni.h>
#include "touch_android.h"
#include "front_android.h"

/* 使用命名JNI入口，与宿主已有JNI_OnLoad兼容。 | Named JNI entry points coexist with the host's JNI_OnLoad. */
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_TouchOverlay_nativeEvent(JNIEnv *env, jclass cls, jint phase, jint slot, jfloat x, jfloat y) {
    (void)env; (void)cls;
    dm_android_touch_event(phase,slot,x,y);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_TouchOverlay_nativeResize(JNIEnv *env, jclass cls, jfloat width, jfloat height, jfloat left, jfloat right, jfloat bottom, jboolean tablet) {
    (void)env; (void)cls;
    dm_android_touch_tablet(tablet);
    dm_android_touch_resize(width,height,left,right,bottom);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_TouchOverlay_nativeLayout(JNIEnv *env, jclass cls, jfloatArray dst) {
    (void)cls;
    if (!dst || (*env)->GetArrayLength(env,dst)<DM_ANDROID_TOUCH_DRAW_FLOATS) return;
    float data[DM_ANDROID_TOUCH_DRAW_FLOATS];
    dm_android_touch_layout(data);
    (*env)->SetFloatArrayRegion(env,dst,0,DM_ANDROID_TOUCH_DRAW_FLOATS,data);
}
/* 游戏视图矩形（px，窗口坐标系；front_android.h）：真全屏局内是贴底的界面矩形，局外是居中的 16:9，没开真全屏是整窗。
   Game view rect (px, window coordinates; front_android.h): the bottom-anchored UI block in a true-fullscreen run, the centered
   16:9 outside it, the whole window without true fullscreen. */
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_TouchOverlay_nativeViewRect(JNIEnv *env, jclass cls, jint width, jint height, jintArray out) {
    (void)cls;
    if (!out || (*env)->GetArrayLength(env,out)<4) return;
    int rect[4]; jint v[4];
    front_android_view_rect((int)width,(int)height,rect);
    v[0]=rect[0]; v[1]=rect[1]; v[2]=rect[2]; v[3]=rect[3];
    (*env)->SetIntArrayRegion(env,out,0,4,v);
}
/* 视图矩形（dp，相对窗口左上角）交给触控布局；窗口尺寸变化时先 resize 再 view。 | The view rect (dp, relative to the window's
   top-left) for the touch layout; on a window size change resize comes before view. */
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_TouchOverlay_nativeView(JNIEnv *env, jclass cls, jfloat x, jfloat y, jfloat width, jfloat height) {
    (void)env; (void)cls;
    dm_android_touch_view(x,y,width,height);
}
