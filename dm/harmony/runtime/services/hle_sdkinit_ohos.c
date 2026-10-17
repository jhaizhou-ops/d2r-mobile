// nninitInitializeSdkModule 调的服务层初始化：真机上各自连 Horizon 系统服务（IPC），宿主没有这些服务进程。
// 原生接管：不建会话，直接返回；各服务的后续接口在用到时逐个接管成宿主实现（见同目录 hle_*.c）。
// nn::os::InitializeVirtualAddressMemory 属 SDK 的 os 层，不接管；fs::SetLocalApplicationAccessLog /
// SetResultHandledByApplication 只写 SDK 内部开关，不接管。
#include "hle_ohos_compat.h"

static void once(const char *what) { LOG("init 接管：%s（空操作）", what); }

HLE_FORCE("nndiagStartup", "连 lm 日志服务；诊断日志改由宿主直接输出", void, h_nndiagStartup, void) { once("nndiagStartup"); }
HLE_FORCE("_ZN2nn2oe10InitializeEv", "连 am/oe 运行环境服务；运行环境查询逐个接成宿主默认值", void, h_oe_Initialize, void) { once("oe::Initialize"); }
HLE_FORCE("_ZN2nn3aoc10InitializeEv", "追加内容(DLC)服务：不建会话（无服务进程）；已拥有清单由 hle_misc.c 的 aoc 接口给出（术士君临，非空）", void, h_aoc_Initialize, void) { once("aoc::Initialize"); }
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "连 err 错误上下文服务；宿主不上报", void, h_err_InitCtx, void) { once("err::InitializeContextWriterApi"); }
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv", "连 fsp-srv 文件服务；文件接口接宿主文件系统 + RomFS", void, h_fs_InitApp, void) { once("fs::InitializeForApplication"); }
HLE_FORCE("_ZN2nn4pctl10InitializeEv", "连 pctl 家长控制服务；单机无家长控制", void, h_pctl_Initialize, void) { once("pctl::Initialize"); }
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", "注册到系统的中止观察；宿主崩溃由信号处理打印", void, h_diag_AbortObs, void) { once("diag::InitializeApplicationAbortObserver"); }
