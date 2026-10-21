// 我们自己加的界面文字取串（语言跟原版设置走：简中 / 繁中用中文，esES / esMX 用西语，ruRU 用俄语，其余英文；物品注释只做了中文与西语（俄语界面不显示），旧译名只有中文）。
// 唯一来源 dm/i18n/ui_text.tsv（键 / 中文 / English / Español / Español MX / Русский / 用在哪），dm/ios/tools/dm_i18n.py 用它做两件事：
//   · 追加进原版字符串表 ui.json（RomFS 覆盖层）：zhCN、zhTW 列 = 中文，esES = 西语，esMX = 拉美西语，ruRU = 俄语，其余 8 列 = 英文。布局写「@键」、宿主 dm_str 走原版取串 main+0x7f5630，
//     切语言时原版自己清空 + 重装字符串表（SetLanguage 0x317298，r34 起不剪），不用我们跟。
//   · 生成 dm_i18n_tab.h（本文件编进去）：游戏画不到的地方（UIKit 弹框、资源下载页、分享文件名、底栏字形）按 dm_lang_group() 取。
// 语言：游戏设过（SetLanguage 观测门）就以它为准；之前（资源下载页、进前端前）读存档 Settings/Settings.json 的 "Language"；
//   没有设置文件 = 首次安装，按 iOS 系统语言（dm_sys_lang）。
// String lookup for UI text we added ourselves (language follows the original settings: Chinese for Simplified / Traditional Chinese, Spanish for esES / esMX, Russian for ruRU, English otherwise; item annotations exist only in Chinese and Spanish (not shown in the Russian UI), old names only in Chinese).
// Single source dm/i18n/ui_text.tsv (key / Chinese / English / Español / Español MX / Russian / where used); dm/ios/tools/dm_i18n.py uses it for two things:
//   - Appended to the original string table ui.json (RomFS overlay): zhCN, zhTW columns = Chinese, esES = Spanish, esMX = Latin American Spanish, ruRU = Russian, the other 8 columns = English. Layouts write "@key", the host's dm_str uses the original lookup main+0x7f5630;
//     on language switch the original game clears and reloads the string table itself (SetLanguage 0x317298, not cut since r34), so we don't need to track it.
//   - Generates dm_i18n_tab.h (compiled into this file): places the game can't draw (UIKit alerts, resource download page, share file names, bottom-bar glyphs) look up by dm_lang_group().
// Language: once the game has set it (SetLanguage observation gate), that wins; before that (resource download page, before entering the front end) read "Language" from the save's Settings/Settings.json;
//   no settings file = first install, use the iOS system language (dm_sys_lang).
#include "dm_i18n.h"
#include "dm_i18n_tab.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Atomic int g_dm_lang = -1;

#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#endif
// 语言标记（BCP 47，如 en-US、zh-Hans-CN、zh-Hant-TW、yue-Hant-HK、es-419）→ 原版语言号。只落四种：语言码是中文（zh）或汉语方言
// （yue 粤、wuu 吴、hak 客、nan 闽南、cmn 官话、gan 赣、hsn 湘、lzh 文言）→ 8 简中，不看字形和地区——我们没有繁体的自有文字，
// 原版繁中 + 我们的简体字一屏两种字；西语（es）→ 地区是西班牙或没写地区 = 2 esES，其余地区（es-419、es-MX、es-US…）= 10 esMX；
// 其余一律 0 英文——我们自有的字只有中英西三套，德 / 日等原版语言配英文自有字没人验过。空标记 = 简中
// Language tag (BCP 47, e.g. en-US, zh-Hans-CN, zh-Hant-TW, yue-Hant-HK, es-419) → original language number. Maps to only four: language code Chinese (zh) or a Chinese dialect
// (yue Cantonese, wuu Wu, hak Hakka, nan Min Nan, cmn Mandarin, gan Gan, hsn Xiang, lzh Classical Chinese) → 8 Simplified Chinese, regardless of script and region — we have no Traditional text of our own,
// so original Traditional Chinese + our Simplified text would mix two scripts on one screen; Spanish (es) → region Spain or no region = 2 esES, any other region (es-419, es-MX, es-US…) = 10 esMX;
// everything else is 0 English — our own text comes only in Chinese, English and Spanish; nobody has verified German / Japanese etc. original languages paired with our English text. Empty tag = Simplified Chinese
int dm_lang_from_tag(const char *t) {
    if (!t || !*t) return 8;
    char l[4] = { 0 }; int i = 0; while (i < 3 && t[i] && t[i] != '-' && t[i] != '_') { l[i] = (char)(t[i] | 0x20); i++; }
    static const char *const zh[] = { "zh", "yue", "wuu", "hak", "nan", "cmn", "gan", "hsn", "lzh" };
    for (unsigned k = 0; k < sizeof zh / sizeof *zh; k++) if (!strcmp(l, zh[k])) return 8;
    if (!strcmp(l, "ru")) return 7;   // ruRU：俄罗斯官方文字，包里配音只有中英，语音按 voice_for 走英文 | ruRU: the official Russian text; the voice-over follows voice_for (Russian only when the resources carry it, otherwise English)
    if (!strcmp(l, "es")) {   // 地区子标记：跳过 4 个字母的字形子标记（es-Latn-ES），取紧跟的 2 个字母或 3 位数字 | Region subtag: skip a 4-letter script subtag (es-Latn-ES), take the 2 letters or 3 digits that follow
        const char *r = t + i; while (*r == '-' || *r == '_') { r++; size_t n = strcspn(r, "-_"); if (n == 4) { r += n; continue; }
            return (n == 2 && (r[0] | 0x20) == 'e' && (r[1] | 0x20) == 's') ? 2 : 10; }
        return 2; }
    return 0;
}
// 首次安装（还没有设置文件）时的语言：按 iOS 首选语言第一项（CFLocaleCopyPreferredLanguages，过 dm_lang_from_tag：中文及汉语方言 → 简中，其余 → 英文）。
// 曾一律简中（那时触屏版没有能改语言的入口，系统是英文的中文玩家新装进了英文版改不回来）；自定义页标题行有语言开关（dm_custom.c lang_en），放开。
// DM_LANG（宿主 / 测试模拟，同 hle_oe.c）优先；非 iOS 的宿主没有系统语言可问，照旧简中
// Language on first install (no settings file yet): by the first entry of the iOS preferred languages (CFLocaleCopyPreferredLanguages, through dm_lang_from_tag: Chinese and Sinitic varieties → Simplified Chinese, everything else → English).
// It used to be always Simplified Chinese (back then the touch version had no way to change the language, and a Chinese player whose system was in English got the English version on a fresh install and could not change it back); the Custom page title row now has a language switch (dm_custom.c lang_en), so this is opened up.
// DM_LANG (host / test simulation, same as hle_oe.c) takes priority; a non-iOS host has no system language to ask and stays Simplified Chinese as before
int dm_sys_lang(void) {
    const char *e = getenv("DM_LANG"); if (e && *e) return dm_lang_from_tag(e);
#if TARGET_OS_IPHONE
    int r = 8; CFArrayRef a = CFLocaleCopyPreferredLanguages();
    if (a) { if (CFArrayGetCount(a) > 0) { char t[64]; if (CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(a, 0), t, sizeof t, kCFStringEncodingUTF8)) r = dm_lang_from_tag(t); } CFRelease(a); }
    return r;
#else
    return 8;
#endif
}

static int settings_lang(void) {   // 设置文件里的 "Language"；文件不在 = 首次安装，按系统语言（hle_fs.c 首次建设置文件时也写这个值）；文件在但没有这个键 = -1 | "Language" in the settings file; file missing = first install, use the system language (hle_fs.c writes the same value when it first creates the settings file); file present but key missing = -1
    const char *sv = getenv("DM_SAVE"); if (!sv) return dm_sys_lang();
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", sv);
    FILE *f = fopen(path, "rb"); if (!f) return dm_sys_lang();
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char *k = strstr(buf, "\"Language\""); if (!k) return -1;
    k = strchr(k + 10, ':'); return k ? atoi(k + 1) : -1;
}

int dm_lang_index(void) {
    int l = atomic_load(&g_dm_lang); if (l >= 0) return l;
    static _Atomic int file = -2; int f = atomic_load(&file); if (f == -2) { f = settings_lang(); atomic_store(&file, f); }
    return f < 0 ? 8 : f;   // 有设置文件但没写语言 = 游戏缺省（宿主报 zh-Hans） | settings file exists but has no language entry = the game default (the host reports zh-Hans)
}
int dm_lang_zh(void) { int l = dm_lang_index(); return l == 8 || l == 9; }   // 繁体（zhTW）我们的字也用简中（繁体玩家读简体比读英文顺） | for Traditional Chinese (zhTW) our text also uses Simplified Chinese (Traditional Chinese players read Simplified more easily than English)
int dm_lang_group(void) { return dm_lang_group_of(dm_lang_index()); }

const char *dm_ui(const char *key) { return dm_ui_group(key, dm_lang_group()); }
int dm_lang_group_of(int l) { return l == 8 || l == 9 ? DM_LG_ZH : l == 2 ? DM_LG_ES : l == 10 ? DM_LG_MX : l == 7 ? DM_LG_RU : DM_LG_EN; }
const char *dm_ui_group(const char *key, int group) {
    for (unsigned i = 0; i < sizeof k_dm_i18n / sizeof *k_dm_i18n; i++) if (!strcmp(k_dm_i18n[i].k, key)) {
        switch (group) { case DM_LG_ZH: return k_dm_i18n[i].zh; case DM_LG_ES: return k_dm_i18n[i].es; case DM_LG_MX: return k_dm_i18n[i].mx; case DM_LG_RU: return k_dm_i18n[i].ru; default: return k_dm_i18n[i].en; } }
    return key;
}

// 西语等语言的原版名字串开头带语法性别标记（[ms] 阳单、[fs] 阴单、[mp] / [fp] 复数，可连写），原版画名字时自己解析掉；
// 我们自己取名字拼进标题 / 列表时要跳过它（西语截图：「[ms]Gorro (Nor)」）。标记总在串首，返回跳过后的指针、不拷贝
// Original name strings in Spanish and other languages start with grammatical gender markers ([ms] masc. sing., [fs] fem. sing., [mp] / [fp] plural, may be chained), which the original game parses out when drawing names;
// when we take a name ourselves and splice it into a title / list we must skip them (Spanish screenshot: "[ms]Gorro (Nor)"). The marker is always at the start of the string; returns a pointer past it, no copy
const char *dm_untag(const char *s) {
    if (!s) return s;
    while (s[0] == '[' && (s[1] == 'm' || s[1] == 'f') && (s[2] == 's' || s[2] == 'p') && s[3] == ']') s += 4;
    return s;
}

const char *dm_str(uint8_t *b, const char *key) {
    if (!b) return dm_ui(key);
    struct { const char *k; uint64_t n; } s = { key, strlen(key) };
    const char *r = ((const char *(*)(void *))(b + 0x7f5630))(&s);
    return r && *r && strcmp(r, key) ? r : dm_ui(key);   // 覆盖层里没有这个键（不该发生）就退回编译期表 | if the overlay lacks this key (should not happen), fall back to the compile-time table
}
