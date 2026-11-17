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

// 厂商 0x5143 是高通；740–799 这一族缺省关掉这两个改写。型号数字不够：骁龙 7 Gen 3 的 Adreno 720（设备号 0x43020000）
// 与 8s Gen 3 的 735（0x43030b00）和 750（0x43051401）是同一代芯片，设备号最高字节都是 0x43；730（0x07030002）与
// 830（0x44050001）不是。720 跑这两个改写，主菜单第一帧就 VK_ERROR_DEVICE_LOST（#263），与 740 当初（#141）一样，所以
// 设备号最高字节为 0x43 的高通芯片也按这一族处理。device_id 传 0 时只看型号。
// Vendor 0x5143 is Qualcomm; the 740-799 family defaults both rewrites off. The model number is not enough: the Snapdragon 7
// Gen 3's Adreno 720 (device ID 0x43020000) is the same chip generation as the 8s Gen 3's 735 (0x43030b00) and the 750
// (0x43051401), all with 0x43 in the top byte of the device ID; the 730 (0x07030002) and 830 (0x44050001) are not. With these
// rewrites the 720 hits VK_ERROR_DEVICE_LOST on the main menu's first frame (#263), as the 740 did (#141), so Qualcomm chips
// whose device ID has 0x43 in the top byte count as this family too. With device_id 0 only the model is used.
inline bool xbg_adreno_rewrites_fault(uint32_t vendor_id,const char *device_name,uint32_t device_id=0){
    if(vendor_id!=0x5143)return false;
    const int model=xbg_adreno_model(device_name);
    return (model>=740 && model<800) || (device_id>>24)==0x43;
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

// Adreno 750–799 这一族（骁龙 8 Gen 3 等）的高通着色器编译器 libllvm-qgl.so，遇到「子组指令写在被调用的
// 辅助函数里」的着色器会在建管线时段错误（addr=0x48，#123、#153、#166 与一份私下反馈，三星与小米共四台机器）。这一族上引擎
// 载入这种着色器时先把辅助函数内联进入口（导入包不变）；标记文件 gfx-inline 可显式开关（1 / 0）。
// The Qualcomm shader compiler of the Adreno 750-799 family (Snapdragon 8 Gen 3 and others; libllvm-qgl.so) segfaults while building a pipeline from a shader
// whose subgroup instructions sit in a called helper function (addr=0x48; #123, #153, #166 and one private report: four
// Samsung and Xiaomi phones). On this family the engine inlines the helpers into the entry point when it loads such a shader
// (the import pack is unchanged); the marker file gfx-inline turns it on or off explicitly (1 / 0).
// 骁龙 8s Gen 3 的 Adreno 735 是 750 那一代（设备号 0x43030b00），编译器同样在 098a126f92bf533a 上段错误（addr=0x48，#169：
// 报告者把内联放宽到 735 后能玩），所以 735 也内联。
// The Snapdragon 8s Gen 3's Adreno 735 is the 750's generation (device ID 0x43030b00), and its compiler segfaults on
// 098a126f92bf533a in the same way (addr=0x48, #169: the reporter extended the inlining to the 735 and the game ran), so the
// 735 is inlined too.
inline bool xbg_adreno_compiler_needs_inline(uint32_t vendor_id,const char *device_name){
    if(vendor_id!=0x5143)return false;
    const int model=xbg_adreno_model(device_name);
    return model==735 || (model>=750 && model<800);
}

// Adreno 8xx：计算派发会多跑范围之外的调用（#256：830 上局部尺寸 8×4×1、y 方向工作组数为奇数时多一行工作组；#261 的 825 症状
// 相同），计算着色器载入时在入口加派发范围检查（xbg_spirv_dispatch.cpp）。标记文件 gfx-dispatch-guard 可显式开关（1 / 0）。
// Adreno 8xx: a compute dispatch also runs invocations outside its range (#256: on the 830, one extra row of work groups for
// local size 8x4x1 and an odd y group count; the 825 in #261 shows the same symptom), so compute shaders get the dispatch-range
// check at their entry when they load (xbg_spirv_dispatch.cpp). The marker file gfx-dispatch-guard turns it on or off
// explicitly (1 / 0).
inline bool xbg_adreno_dispatch_overruns(uint32_t vendor_id,const char *device_name){
    if(vendor_id!=0x5143)return false;
    const int model=xbg_adreno_model(device_name);
    return model>=800 && model<900;
}

// 优先顺序：标记文件（只有明确的 0 关闭），其次机型判断（这一族缺省开），最后环境变量 DM_GFX_DISPATCH_GUARD（别的机型想开时用）。
// Precedence: an existing marker (only an explicit 0 turns it off), then the device family (on by default there), and the
// environment variable DM_GFX_DISPATCH_GUARD last (for turning it on elsewhere).
inline bool xbg_adreno_dispatch_guard_option(bool marker_present,int marker,const char *environment,bool driver_overruns){
    if(marker_present)return marker!='0';
    if(driver_overruns)return true;
    return environment && atoi(environment)!=0;
}
