// nninitInitializeSdkModule 调的服务层初始化：真机上各自连 Horizon 系统服务（IPC），宿主没有这些服务进程。
// 原生接管：不建会话，直接返回；各服务的后续接口在用到时逐个接管成宿主实现（见同目录 hle_*.c）。
// nn::os::InitializeVirtualAddressMemory 属 SDK 的 os 层，不接管；fs::SetLocalApplicationAccessLog /
// SetResultHandledByApplication 只写 SDK 内部开关，不接管。
// Service layer initialization called by nninitInitializeSdkModule: on the real device each connects to a Horizon system service (IPC), the host has no such service processes.
// Native takeover: create no session, return directly; each service's later interfaces are taken over one by one as host implementations when they come into use (see hle_*.c in the same directory).
// nn::os::InitializeVirtualAddressMemory belongs to the SDK's os layer and is not taken over; fs::SetLocalApplicationAccessLog /
// SetResultHandledByApplication only write SDK-internal switches and are not taken over.
#include "hle.h"

#ifdef __ANDROID__
#include <android/log.h>
#include "../android/services_sdk.h"

HLE_FORCE("nndiagStartup", "诊断消息由实际 Android 日志后端输出", void, h_nndiagStartup, void) {
    __android_log_print(ANDROID_LOG_INFO, "D2RMobile", "Guest diagnostics use Android log output");
}
HLE_FORCE("_ZN2nn2oe10InitializeEv", "初始化已登记的 Android 应用生命周期后端；缺失则中止", void, h_oe_Initialize, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_OE); }
HLE_FORCE("_ZN2nn3aoc10InitializeEv", "初始化已登记且对应当前游戏版本的实际 DLC 清单后端；缺失则中止", void, h_aoc_Initialize, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_AOC); }
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "初始化已登记的 Android 错误上下文记录后端；缺失则中止", void, h_err_InitCtx, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_ERROR_CONTEXT); }
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv", "初始化已登记的实际 RomFS/存档/临时文件后端；缺失则中止", void, h_fs_InitApp, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_FS); }
HLE_FORCE("_ZN2nn4pctl10InitializeEv", "初始化已登记的实际本机访问策略后端；缺失则中止", void, h_pctl_Initialize, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_PARENTAL_CONTROL); }
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", "初始化已登记的 Android 中止/崩溃观察后端；缺失则中止", void, h_diag_AbortObs, void) { dm_android_service_initialize(DM_ANDROID_SERVICE_ABORT_OBSERVER); }
#else
static void once(const char *what) { LOG("init 接管：%s（空操作）", what); }

HLE_FORCE("nndiagStartup", "连 lm 日志服务；诊断日志改由宿主直接输出", void, h_nndiagStartup, void) { once("nndiagStartup"); }
HLE_FORCE("_ZN2nn2oe10InitializeEv", "连 am/oe 运行环境服务；运行环境查询逐个接成宿主默认值", void, h_oe_Initialize, void) { once("oe::Initialize"); }
HLE_FORCE("_ZN2nn3aoc10InitializeEv", "追加内容(DLC)服务：不建会话（无服务进程）；已拥有清单由 hle_misc.c 的 aoc 接口给出（术士君临，非空）", void, h_aoc_Initialize, void) { once("aoc::Initialize"); }
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "连 err 错误上下文服务；宿主不上报", void, h_err_InitCtx, void) { once("err::InitializeContextWriterApi"); }
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv", "连 fsp-srv 文件服务；文件接口接宿主文件系统 + RomFS", void, h_fs_InitApp, void) { once("fs::InitializeForApplication"); }
HLE_FORCE("_ZN2nn4pctl10InitializeEv", "连 pctl 家长控制服务；单机无家长控制", void, h_pctl_Initialize, void) { once("pctl::Initialize"); }
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", "注册到系统的中止观察；宿主崩溃由信号处理打印", void, h_diag_AbortObs, void) { once("diag::InitializeApplicationAbortObserver"); }
#endif
