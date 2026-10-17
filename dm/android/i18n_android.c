/* Android 的语言面（P5）：hle_fs.c 首次建设置文件时要问系统语言（dm_sys_lang），
 * 语言开关（自定义页那一格，没移植前 = 没点过「应用」→ 返回 -1 = 一个字不动）。
 * 口径与 dm/host/dm_i18n.c 的 dm_lang_from_tag 一致：中文及汉语方言 → 8（简中），其余 → 0（英文）；
 * DM_LANG（宿主 / 测试）优先，其次 app_jni 传进来的系统 locale tag（DM_LOCALE_TAG），都没有按简中。
 * Android language side (P5): the settings seeding asks the system language (dm_sys_lang);
 * the language switch (the Custom page cell, not ported yet = never tapped -> -1 = touch nothing).
 * Same convention as dm_lang_from_tag: Chinese / Sinitic -> 8 (Simplified Chinese), everything else -> 0 (English). */
#include "../host/hle.h"
#include <stdlib.h>
#include <string.h>

static int tag_lang(const char *t) {
    if (!t) return 8;
    /* 只看语言子标签与可能的脚本 / 地区后缀：zh / zh-Hans / zh_CN / zh-Hant … */
    for (const char *p = t; *p; p++) {
        if (p[0] != 'z' && p[0] != 'Z') continue;
        if (p[1] != 'h' && p[1] != 'H') continue;
        char c = p[2];
        if (c == '_' || c == '-' || c == 0) return 8;   /* zh / zh-CN / zh-Hant … 一律简中（与 iOS 同口径） */
    }
    return 0;
}

int dm_sys_lang(void) {
    const char *e = getenv("DM_LANG"); if (e && *e) return tag_lang(e);
    const char *t = getenv("DM_LOCALE_TAG"); if (t && *t) return tag_lang(t);
    return 8;
}

/* 自定义页的语言开关（dm_custom.c lang_en）：安卓还没移植那一页 → 永远「没点过应用」→ 设置文件的语言一个字不动 */
int dm_custom_lang_explicit(void) { return -1; }