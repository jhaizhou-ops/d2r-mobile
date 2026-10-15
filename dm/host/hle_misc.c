// 零散服务层：追加内容 / 商店 / 错误小程序 / 敏感词过滤 / 服务器名解析 / 家长控制 / 软键盘。
// 真机上各自走 IPC 或系统小程序；宿主按单机离线给结果。纯计算的 SDK 部分（hash、util、软件 Opus）不接管，直接用原实现。
// Miscellaneous service layer: add-on content / shop / error applet / profanity filter / server name resolution / parental controls / software keyboard.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "hle.h"
#ifdef __ANDROID__
#include "../android/services_platform.h"
#endif

// ---------- aoc 追加内容（术士君临，保留）/ ec 商店（排除）----------
// 术士君临（Reign of the Warlock）是 Switch 包里唯一的追加内容：AOC 0100726014353001 = 本体 0100726014352000 的追加内容序号 1。
// 该 DLC 只是约 100 KB 的解锁凭证，内容本体已在主 RomFS；main 不导入 MountAddOnContent，只按「有没有序号 1」判断。
// 复刻范围要求保留术士君临，所以报告已拥有序号 1；排除的是商店（ec），不是 DLC 本身。
// ---------- aoc add-on content (Reign of the Warlock, kept) / ec shop (excluded) ----------
// Reign of the Warlock is the only add-on content in the Switch package: AOC 0100726014353001 = add-on content index 1 of the base 0100726014352000.
// This DLC is only an unlock ticket of about 100 KB, the content itself is already in the main RomFS; main does not import MountAddOnContent and only judges by "is index 1 present".
// The reimplementation scope requires keeping Reign of the Warlock, so index 1 is reported as owned; what is excluded is the shop (ec), not the DLC itself.
enum { AOC_WARLOCK = 1 };
// DM_AOC_OWNED=0：测试开关，回到「无 DLC」前提（只用于对拍未装 DLC 时录的真值；缺省 = 拥有）
// DM_AOC_OWNED=0: test switch, back to the "no DLC" premise (only for lockstep comparison against ground truth recorded without the DLC installed; default = owned)
#ifndef __ANDROID__
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_AOC_OWNED"); v = !(e && !strcmp(e, "0")); if (!v) LOG("DM_AOC_OWNED=0：aoc 报告 0 个追加内容（对拍测试用）"); } return v; }
#endif
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "追加内容数：Android按已验证当前版本内容清单", int, h_aoc_Count, void) {
#ifdef __ANDROID__
    return dm_android_platform_aoc_count();
#else
    return aoc_owned() ? 1 : 0;
#endif
}
// ListAddOnContent(out, 起始下标, 最多个数)：按已拥有序号列表 {1} 从起始下标起写出，返回写出个数
// ListAddOnContent(out, start index, max count): writes out from the owned index list {1} starting at the start index, returns the number written
HLE_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "追加内容列表：{1}（术士君临）", int, h_aoc_List, int *out, int off, int n) {
#ifdef __ANDROID__
    return dm_android_platform_aoc_list(out, off, n);
#else
    static const int owned[] = {AOC_WARLOCK};
    int w = 0;
    if (!aoc_owned()) return 0;
    for (int i = off; i < (int)(sizeof owned / sizeof *owned) && w < n; i++) out[w++] = owned[i];
    return w;
#endif
}
// 事件对象按需由 nn::os 旁表建（未触发），游戏等它只会超时 / 取到未触发
// The event object is created on demand by the nn::os side table (not signaled), the game waiting on it only times out / gets not-signaled
HLE_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", "追加内容变更事件，排除范围；绑内核事件、恒不触发（游戏会 TryWait 轮询，未初始化即中止）", void, h_aoc_GetEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListEv", "商店小程序，排除范围；剪口：空操作", void, h_ec_ShowShop, void) { LOG("ec::ShowShopAddOnContentList（剪）"); }

// ---------- err 错误小程序：宿主打印 ----------
// ---------- err error applet: printed by the host ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "错误显示小程序；宿主打印模块 / 描述号", void, h_err_ShowError, Result r) {
#ifdef __ANDROID__
    dm_android_platform_record_error(r, "nn::err::ShowError");
#endif
    LOG("err::ShowError Result=%#x（模块 %u，描述 %u）", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc 敏感词过滤：词库在系统数据存档（经 IPC 挂载）；单机无他人可见文本，恒判无敏感词 ----------
// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterC1Ev", "敏感词过滤，词库走系统存档；宿主不建", void, h_ngc_Ctor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_create(self, 30);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "敏感词过滤", void, h_ngc_Dtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_destroy(self, 30);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", "离线本地文字直通策略；不声称加载任天堂词库", Result, h_ngc_Init, void *self, void *work, uint64_t sz) {
#ifdef __ANDROID__
    if (!work || !sz || !dm_android_platform_object_exists(self, 30) || !dm_android_platform_network_ready()) return RES_FAIL;
    dm_android_platform_setting("local_text_passthrough", 1);
    dm_android_platform_object_ready(self, 30); return RES_OK;
#else
    (void)self; (void)work; (void)sz; return RES_OK;
#endif
}
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "词库版本；宿主 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "敏感词检查；恒无命中", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
#ifdef __ANDROID__
    if (!dm_android_platform_object_is_ready(self, 30) || (n && (!masks || !words))) return RES_FAIL;
    for (uint64_t i = 0; i < n; i++) if (!dm_android_platform_utf8_valid(words[i])) return RES_FAIL;
#endif
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}

// ---------- nsd 服务器名解析 / pctl 自由通信：联网，排除范围 ----------
// ---------- nsd server name resolution / pctl free communication: networking, out of scope ----------
HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_Resolve, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool 参数只看低 8 位；返回 bool
// bool arguments only look at the low 8 bits; returns bool
HLE_FORCE("_ZN2nn4pctl25TryBeginFreeCommunicationEb", "家长控制·与他人通信，联网排除；恒不允许", unsigned, h_pctl_TryBegin, unsigned show_ui) {
#ifdef __ANDROID__
    dm_android_platform_setting("communication_request_denied", 1);
#endif
    (void)show_ui; return 0;
}
HLE_FORCE("_ZN2nn4pctl20EndFreeCommunicationEv", "家长控制·与他人通信", void, h_pctl_End, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("communication_request_active", 0);
#endif
}

// swkbd 软键盘：见 hle_swkbd.c
// swkbd software keyboard: see hle_swkbd.c
