// 零散服务层：追加内容 / 商店 / 错误小程序 / 敏感词过滤 / 服务器名解析 / 家长控制 / 软键盘。
// 真机上各自走 IPC 或系统小程序；宿主按单机离线给结果。纯计算的 SDK 部分（hash、util、软件 Opus）不接管，直接用原实现。
#include "hle_ohos_compat.h"

// ---------- aoc 追加内容（术士君临，保留）/ ec 商店（排除）----------
// 术士君临（Reign of the Warlock）是 Switch 包里唯一的追加内容：AOC 0100726014353001 = 本体 0100726014352000 的追加内容序号 1。
// 该 DLC 只是约 100 KB 的解锁凭证，内容本体已在主 RomFS；main 不导入 MountAddOnContent，只按「有没有序号 1」判断。
// 复刻范围要求保留术士君临（jhz 09-23），所以报告已拥有序号 1；排除的是商店（ec），不是 DLC 本身。
enum { AOC_WARLOCK = 1 };
// SW_AOC_OWNED=0：测试开关，回到「无 DLC」前提（只用于对拍未装 DLC 时录的真值；缺省 = 拥有）
static int aoc_owned(void) { static int v = -1; if (v < 0) { const char *e = getenv("SW_AOC_OWNED"); v = !(e && !strcmp(e, "0")); if (!v) LOG("SW_AOC_OWNED=0：aoc 报告 0 个追加内容（对拍测试用）"); } return v; }
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "追加内容数：已拥有术士君临 1 个（复刻范围必须保留）", int, h_aoc_Count, void) { return aoc_owned() ? 1 : 0; }
// ListAddOnContent(out, 起始下标, 最多个数)：按已拥有序号列表 {1} 从起始下标起写出，返回写出个数
HLE_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "追加内容列表：{1}（术士君临）", int, h_aoc_List, int *out, int off, int n) {
    static const int owned[] = {AOC_WARLOCK};
    int w = 0;
    if (!aoc_owned()) return 0;
    for (int i = off; i < (int)(sizeof owned / sizeof *owned) && w < n; i++) out[w++] = owned[i];
    return w;
}
// 事件对象按需由 nn::os 旁表建（未触发），游戏等它只会超时 / 取到未触发
HLE_FORCE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", "追加内容变更事件，排除范围；绑内核事件、恒不触发（游戏会 TryWait 轮询，未初始化即中止）", void, h_aoc_GetEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2ec24ShowShopAddOnContentListEv", "商店小程序，排除范围；剪口：空操作", void, h_ec_ShowShop, void) { LOG("ec::ShowShopAddOnContentList（剪）"); }

// ---------- err 错误小程序：宿主打印 ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "错误显示小程序；宿主打印模块 / 描述号", void, h_err_ShowError, Result r) {
    LOG("err::ShowError Result=%#x（模块 %u，描述 %u）", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc 敏感词过滤：词库在系统数据存档（经 IPC 挂载）；单机无他人可见文本，恒判无敏感词 ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterC1Ev", "敏感词过滤，词库走系统存档；宿主不建", void, h_ngc_Ctor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "敏感词过滤", void, h_ngc_Dtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", "敏感词过滤初始化；宿主无词库", Result, h_ngc_Init, void *self, void *work, uint64_t sz) { (void)self; (void)work; (void)sz; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "词库版本；宿主 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "敏感词检查；恒无命中", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}

// ---------- nsd 服务器名解析 / pctl 自由通信：联网，排除范围 ----------
HLE_FORCE("_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_Resolve, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "服务器名解析，联网排除；剪口", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
// bool 参数只看低 8 位；返回 bool
HLE_FORCE("_ZN2nn4pctl25TryBeginFreeCommunicationEb", "家长控制·与他人通信，联网排除；恒不允许", unsigned, h_pctl_TryBegin, unsigned show_ui) { (void)show_ui; return 0; }
HLE_FORCE("_ZN2nn4pctl20EndFreeCommunicationEv", "家长控制·与他人通信", void, h_pctl_End, void) {}

// swkbd 软键盘：见 hle_swkbd.c
