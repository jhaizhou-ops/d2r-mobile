// swkbd 软键盘（系统小程序）→ 宿主原生输入框。MakePreset / SetInitialTextUtf8 / GetRequired*Size 留在 SDK 原样跑，
// 这里只读它们写出的 ShowKeyboardArg，并按真 swkbd 小程序的输出约定回写 String。
// 布局出处（sdk 3.3.94012 原映像偏移）：
//   ShowKeyboard 0x3a6770 逐字段搬 KeyboardConfig 进小程序参数：+0x24 标题 u16[65]、+0xa6 副标题 u16[129]、+0x1a8 提示 u16[257]、
//     +0x3ac 最大长度、+0x3b0 最小长度、+0x3bd isUseUtf8、+0x3c0 初始文本在工作缓冲内的偏移、+0x3c4 初始文本 UTF-16 长度；
//     工作缓冲指针 / 大小在 +0x4d0 / +0x4d8（随 CreateLargeStorage 一起交给小程序）
//   SetInitialTextUtf8 0x3a74c0：UTF-8 → UTF-16 写进 工作缓冲+偏移（偏移 0x14，至多 500 字 + 结尾 0）
//   ReadCloseResultAndString 0x3a63f0：输出存储 = u32 CloseResult + 文本区；文本区原样拷进 String{+0 指针, +8 字节数}
//   ShowKeyboard 收尾 0x3a6acc：CloseResult == 0（确定）→ Result 0，否则（取消）→ 0x29F；GetRequiredStringBufferSize = 0x7D4
// 游戏侧（main 0xc6deac）MakePreset 后置 isUseUtf8 = 1，读回按 C 串（0x1b66640 strlen）；UTF-16 输出按原约定一并支持。
// 无窗口且未设 DM_KBD：按取消返回（不弹窗、不卡住）。DM_KBD="文本"：不弹窗，直接按确定返回该文本（无窗口测试 / 对拍）。
// swkbd software keyboard (system applet) → host native input box. MakePreset / SetInitialTextUtf8 / GetRequired*Size are left to run as is in the SDK,
// here we only read the ShowKeyboardArg they write and write back String per the output convention of the real swkbd applet.
// Layout sources (offsets in the original sdk 3.3.94012 image):
//   ShowKeyboard 0x3a6770 moves KeyboardConfig field by field into the applet arguments: +0x24 title u16[65], +0xa6 subtitle u16[129], +0x1a8 hint u16[257],
//     +0x3ac maximum length, +0x3b0 minimum length, +0x3bd isUseUtf8, +0x3c0 offset of the initial text within the work buffer, +0x3c4 UTF-16 length of the initial text;
//     work buffer pointer / size at +0x4d0 / +0x4d8 (handed to the applet together with CreateLargeStorage)
//   SetInitialTextUtf8 0x3a74c0: UTF-8 → UTF-16 written to work buffer+offset (offset 0x14, at most 500 characters + terminating 0)
//   ReadCloseResultAndString 0x3a63f0: output storage = u32 CloseResult + text area; the text area is copied as is into String{+0 pointer, +8 byte count}
//   ShowKeyboard wrap-up 0x3a6acc: CloseResult == 0 (OK) → Result 0, otherwise (cancel) → 0x29F; GetRequiredStringBufferSize = 0x7D4
// Game side (main 0xc6deac) sets isUseUtf8 = 1 after MakePreset and reads back as a C string (0x1b66640 strlen); UTF-16 output is supported as well per the original convention.
// No window and DM_KBD unset: return as cancel (no popup, no hang). DM_KBD="text": no popup, return that text directly as OK (windowless testing / lockstep comparison).
#include "hle.h"
#include <unistd.h>
#include "front.h"

enum { ARG_HEADER = 0x24, ARG_SUB = 0xa6, ARG_GUIDE = 0x1a8, ARG_MAXLEN = 0x3ac, ARG_MINLEN = 0x3b0, ARG_UTF8 = 0x3bd,
       ARG_INIT_OFF = 0x3c0, ARG_INIT_LEN = 0x3c4, ARG_WORKBUF = 0x4d0,
       HEADER_N = 65, SUB_N = 129, GUIDE_N = 257 };
#define SWKBD_RESULT_CANCELED 0x29Fu   // swkbd 模块 159 / 描述 1：SDK 在 CloseResult != 0 时返回的常数 | swkbd module 159 / description 1: the constant the SDK returns when CloseResult != 0

// UTF-16（至多 n 个单元，遇 0 止）→ UTF-8，写 out（cap 含结尾 0），返回写入字节数
// UTF-16 (at most n units, stops at 0) → UTF-8, written to out (cap includes the terminating 0), returns the number of bytes written
size_t swkbd_u16_to_u8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        char b[4]; int k = c < 0x80 ? (b[0] = (char)c, 1) : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
                 : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3)
                 : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)), b[3] = (char)(0x80 | (c & 63)), 4);
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k); o += k;
    }
    if (cap) out[o] = 0;
    return o;
}
// UTF-8 → UTF-16，至多 max_units 个单元（不拆代理对），写 out（cap 单元含结尾 0），返回单元数
// UTF-8 → UTF-16, at most max_units units (surrogate pairs are not split), written to out (cap units including the terminating 0), returns the unit count
size_t swkbd_u8_to_u16(const char *s, size_t max_units, uint16_t *out, size_t cap) {
    size_t o = 0; const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c; int k = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        c = k == 1 ? *p : k == 2 ? *p & 31 : k == 3 ? *p & 15 : *p & 7;
        for (int j = 1; j < k; j++) { if ((p[j] & 0xC0) != 0x80) { k = j; break; } c = c << 6 | (p[j] & 63); }
        size_t need = c >= 0x10000 ? 2 : 1;
        if (o + need > max_units || o + need + 1 > cap) break;
        if (need == 2) { c -= 0x10000; out[o++] = (uint16_t)(0xD800 | c >> 10); out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else out[o++] = (uint16_t)c;
        p += k;
    }
    if (cap) out[o] = 0;
    return o;
}

// 核心（单测直接调）：从 arg 解出显示参数，经 input 取得用户文本（返回 1 确定 / 0 取消 / -1 无窗口），按 arg 约定回写 out
// 游戏的离线角色名规则（原映像，逐字节判）：2–15 字节、首尾必须「字母」、'-' '_' 合计最多一个，其余必须是字母。原版只认 ASCII 字母；
// hle_charname.c 把 ≥0x80 的字节也放行（中文名），所以规则变成：英文字母或汉字，UTF-8 合计不超过 15 字节（汉字每个 3 字节，最多 5 个）。
// 起名面板开着时让前端按这条规则当场拦，并把规则写在对话框里。diag.c 按界面消息维护这个标志。
// Core (called directly by unit tests): decode the display parameters from arg, get the user text through input (returns 1 OK / 0 cancel / -1 no window), write back out per the arg convention
// The game's offline character name rule (original image, judged byte by byte): 2–15 bytes, first and last must be a "letter", '-' and '_' at most one in total, the rest must be letters. The original game only accepts ASCII letters;
// hle_charname.c also lets bytes ≥0x80 through (Chinese names), so the rule becomes: English letters or Chinese characters, at most 15 UTF-8 bytes in total (3 bytes per Chinese character, at most 5).
// While the naming panel is open the front end blocks on the spot by this rule and writes the rule in the dialog. diag.c maintains this flag from UI messages.
int g_swkbd_name_panel;
int g_swkbd_numeric_max = -1;   // ≥0 = 这一次输入是数量框、值 = 上限（dm_textinput.c 置，前端输入框读，用完置回 -1） | ≥0 = this input is a quantity box, value = upper limit (set by dm_textinput.c, read by the front end input box, set back to -1 after use)
int (*g_swkbd_unfilter)(void);   // 宿主登记（hle_charname.c 的 dm_charname_unfilter；单测里为空）：起名面板的键盘请求不过游戏的 ASCII 键入过滤 | registered by the host (dm_charname_unfilter in hle_charname.c; NULL in unit tests): keyboard requests from the naming panel bypass the game's ASCII typing filter
typedef int (*swkbd_input_fn)(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
Result swkbd_show(void *out, const void *arg_, swkbd_input_fn input) {
    const uint8_t *arg = arg_;
    char title[HEADER_N * 3 + 1], sub[SUB_N * 3 + 1], guide[GUIDE_N * 3 + 1], init[501 * 3 + 1] = "", text[1002 * 3 + 1];
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_HEADER), HEADER_N, title, sizeof title);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_SUB), SUB_N, sub, sizeof sub);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_GUIDE), GUIDE_N, guide, sizeof guide);
    int32_t maxlen, ilen, ioff; memcpy(&maxlen, arg + ARG_MAXLEN, 4); memcpy(&ilen, arg + ARG_INIT_LEN, 4); memcpy(&ioff, arg + ARG_INIT_OFF, 4);
    const uint8_t *work; memcpy(&work, arg + ARG_WORKBUF, 8);
    if (ilen > 0 && work) swkbd_u16_to_u8((const uint16_t *)(work + ioff), (size_t)ilen, init, sizeof init);

    const char *script = getenv("DM_KBD");
#ifdef __ANDROID__
    int32_t minlen;memcpy(&minlen,arg+ARG_MINLEN,4);
    void dm_android_keyboard_constraints(int min);dm_android_keyboard_constraints(minlen);
#endif
    // 键盘弹着期间冻结客体时钟（离线单机里玩家打字时怪不该照打、回来也不该补拍；这里按 iOS 原生体验，不守 Switch「小程序占前台时钟照走」的原版语义）
    // Freeze the guest clock while the keyboard is up (in offline single-player, monsters should not keep attacking while the player types, nor should ticks be caught up on return; this follows the native iOS experience, not the original game's Switch semantics of "the clock keeps running while an applet holds the foreground")
    if (!script) guest_clock_freeze_for(2, 1);
    int r = script ? (snprintf(text, sizeof text, "%s", script), 1) : input(title, sub, guide, init, maxlen > 0 ? maxlen : 0, text, sizeof text);
    if (!script) guest_clock_freeze_for(2, 0);
    g_swkbd_numeric_max = -1;   // 数字框标记只管这一次（dm_textinput.c 在弹键盘前置） | the numeric box mark applies to this one time only (set by dm_textinput.c before bringing up the keyboard)
    LOG("swkbd：标题「%s」初始「%s」上限 %d → %s%s%s", title, init, maxlen,
        r > 0 ? "确定「" : r == 0 ? "取消" : "无窗口按取消", r > 0 ? text : "", r > 0 ? "」" : "");
    if (r <= 0) return SWKBD_RESULT_CANCELED;
    if (g_swkbd_name_panel && g_swkbd_unfilter && g_swkbd_unfilter()) LOG("swkbd：起名面板——已关掉游戏对返回串的 ASCII 键入过滤（中文名放行，见 hle_charname.c）");

    uint8_t *buf; uint64_t cap; memcpy(&buf, (uint8_t *)out, 8); memcpy(&cap, (uint8_t *)out + 8, 8);
    if (!buf || !cap) return RES_OK;                         // 与 SDK 同：String 未给缓冲则不读文本 | same as the SDK: if String has no buffer, the text is not read
    memset(buf, 0, cap);
    uint16_t u16[1003]; size_t n = swkbd_u8_to_u16(text, maxlen > 0 ? (size_t)maxlen : 1002, u16, 1003);   // 按字数上限截断（UTF-16 单元计） | truncate to the character limit (counted in UTF-16 units)
    if (arg[ARG_UTF8]) swkbd_u16_to_u8(u16, n, (char *)buf, cap);
    else memcpy(buf, u16, (n + 1) * 2 <= cap ? n * 2 : (cap / 2 - 1) * 2);
    { char fin[64]; swkbd_u16_to_u8(u16, n, fin, sizeof fin); void dm_ui_text_entered(const char *text); dm_ui_text_entered(n > 0 ? fin : NULL); }
    { const char *lag = getenv("DM_TEST_SWKBD_LAG_MS"); if (lag) usleep((useconds_t)atoi(lag) * 1000); }   // 测试：把「键盘已交回 → 游戏那条线程拿到结果」拖慢，验自动接受不抢跑   // 前端路径理顺（dm_cursor.c）：起名页输完名字自动接受 | test: slow down "keyboard handed back → the game's thread gets the result" to verify that auto-accept does not jump the gun   // front end path cleanup (dm_cursor.c): auto-accept after the name is typed on the naming page
    return RES_OK;
}

HLE_FORCE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", "软键盘小程序 → 宿主原生输入框（DM_KBD 直给文本）",
          Result, h_swkbd_Show, void *out, const void *arg) {
    return swkbd_show(out, arg, front_text_input);
}
