// Adreno 740 / 750（骁龙 8 Gen 2 / 8 Gen 3）收下两个安卓着色器改写后生成的 SPIR-V，运行时却在驱动里出错：kgsl 报 GPU
// 页错误（gpu timeout ... status 00EF1087），驱动丢掉设备，下一次提交返回 VK_ERROR_DEVICE_LOST（-4），启动约两秒、主菜单
// 出现之前就崩（#141）。两个改写本来是给 Adreno 730 写的，730 与 830 上都能跑，所以只在 740–799 这一族上缺省关掉，
// 标记文件仍可显式打开（见 xbg_game_initialize 里的两个开关）。
// Adreno 740 / 750 (Snapdragon 8 Gen 2 / 8 Gen 3) accept the SPIR-V of the two Android shader rewrites and then fault in the
// driver: kgsl reports a GPU page fault (gpu timeout ... status 00EF1087), the driver drops the device, and the next submit
// fails with VK_ERROR_DEVICE_LOST (-4) about two seconds into the boot, before the main menu (#141). Both rewrites were written
// for Adreno 730, which runs them, as does Adreno 830, so they default off on the 740-799 family only; an explicit marker file
// still turns either one on (see the two switches in xbg_game_initialize).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// 设备名形如 "Adreno (TM) 740"：取 "Adreno" 之后的第一段数字。不是三段数字（例如 "Adreno X1-85"）时当作认不出来。
// Device names look like "Adreno (TM) 740": take the first run of digits after "Adreno". Anything that is not three digits
// (for example "Adreno X1-85") counts as unrecognized.
inline int xbg_adreno_model(const char *device_name){
    if(!device_name)return 0;
    const char *at=strstr(device_name,"Adreno");
    if(!at)at=strstr(device_name,"adreno");
    if(!at)return 0;
    at+=6;
    while(*at && (*at<'0' || *at>'9'))++at;
    int model=0,digits=0;
    while(*at>='0' && *at<='9' && digits<4){model=model*10+(*at-'0');++at;++digits;}
    return digits==3?model:0;
}

// 厂商 0x5143 是高通；只有 740–799 这一族缺省关掉这两个改写。
// Vendor 0x5143 is Qualcomm; only the 740-799 family defaults both rewrites off.
inline bool xbg_adreno_rewrites_fault(uint32_t vendor_id,const char *device_name){
    if(vendor_id!=0x5143)return false;
    const int model=xbg_adreno_model(device_name);
    return model>=740 && model<800;
}

// 与其它 gfx 开关相同的优先顺序：标记文件优先（只有明确的 0 关闭），其次是机型判断，最后才看环境变量。安卓端
// jni_android.c 对所有机型都设 DM_GFX_*_=1，所以机型判断必须排在环境变量前面，否则这两个缺省会失效。
// The same precedence as the other gfx switches: an existing marker wins (only an explicit 0 turns it off), then the device
// family, and the environment last. The Android JNI sets DM_GFX_* to 1 on every phone, so the device check has to come before
// the environment or these defaults would never take effect.
inline bool xbg_adreno_rewrite_option(bool marker_present,int marker,const char *environment,bool driver_faults){
    if(marker_present)return marker!='0';
    if(driver_faults)return false;
    return environment && atoi(environment)!=0;
}
