// dm_i18n.c 的语言来源（UIKit 文字用）：游戏设过语言（SetLanguage 观测门写 g_dm_lang）以它为准；没设过读存档 Settings/Settings.json 的 "Language"；
// 没有设置文件 = 首次安装，按系统语言（测试用 DM_LANG 模拟）。我们的字：zhCN、zhTW 用中文，其余英文。
// 期望值来自规则本身（8 zhCN / 9 zhTW 中文，其余英文；语言号表见 diag.c 名表 main+0x20ae478），不跑游戏。每个情形单独起进程（文件读一次就缓存），所以本程序按 argv 跑一个情形，Makefile 逐个调。
// Language source of dm_i18n.c (for UIKit text): once the game has set the language (the SetLanguage observation gate writes g_dm_lang), that value
// wins; if not, read "Language" from Settings/Settings.json in the save data;
// no settings file = first install, use the system language (simulated in tests with DM_LANG). Our text: Chinese for zhCN and zhTW, English for
// everything else.
// Expected values come from the rule itself (8 zhCN / 9 zhTW are Chinese, everything else is English; for the language index table see the name table at
// main+0x20ae478 in diag.c), without running the game. Each case needs its own process (the file is read once and then cached), so this program runs one
// case per argv and the Makefile invokes them one by one.
#include "../dm_i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int fails;
static void expect(const char *what, const char *got, const char *want) { int ok = !strcmp(got, want); if (!ok) fails++; printf("%s %s：期望「%s」→ 真结果「%s」\n", ok ? "✓" : "✗", what, want, got); }
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    unsetenv("DM_LANG");
    char dir[512]; snprintf(dir, sizeof dir, "%s/i18n_%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", mode);
    char sd[600], f[700]; snprintf(sd, sizeof sd, "%s/Settings", dir); snprintf(f, sizeof f, "%s/Settings.json", sd);
    mkdir(dir, 0755); mkdir(sd, 0755); remove(f); setenv("DM_SAVE", dir, 1);
    if (!strcmp(mode, "en") || !strcmp(mode, "game") || !strcmp(mode, "startup")) { FILE *o = fopen(f, "w"); fputs("{\n    \"Item Name Display\": 1,\n    \"Language\": 0,\n    \"AutoMapMode\": 2\n}\n", o); fclose(o); }
    if (!strcmp(mode, "es") || !strcmp(mode, "esmx")) { FILE *o = fopen(f, "w"); fprintf(o, "{ \"Language\": %d }", !strcmp(mode, "es") ? 2 : 10); fclose(o); }
    if (!strcmp(mode, "zh")) { FILE *o = fopen(f, "w"); fputs("{ \"Language\": 8 }", o); fclose(o); }
    if (!strcmp(mode, "en")) expect("设置文件 Language 0、游戏还没设", dm_ui("dmConfirm"), "OK");
    else if (!strcmp(mode, "zh")) expect("设置文件 Language 8", dm_ui("dmConfirm"), "确定");
    else if (!strcmp(mode, "es") || !strcmp(mode, "esmx")) expect("Ajustes en español (esES / esMX)", dm_ui("dmConfirm"), "Aceptar");
    else if (!strcmp(mode, "none")) { unsetenv("DM_LANG"); expect("Mac sin ajustes: español predeterminado", dm_ui("dmConfirm"), "Aceptar"); }
    else if (!strcmp(mode, "syses")) { setenv("DM_LANG", "es-ES", 1); expect("DM_LANG=es-ES", dm_ui("dmConfirm"), "Aceptar"); }
    else if (!strcmp(mode, "sysen")) { setenv("DM_LANG", "en-US", 1); expect("没有设置文件（首次安装）、系统语言 en-US", dm_ui("dmConfirm"), "OK"); }
    else if (!strcmp(mode, "systw")) { setenv("DM_LANG", "zh-Hant-TW", 1); expect("没有设置文件、系统语言 zh-Hant-TW（繁体用中文）", dm_ui("dmConfirm"), "确定"); }
    else if (!strcmp(mode, "startup")) {
        expect("Ajustes anteriores en inglés", dm_ui("dmConfirm"), "OK");
        dm_lang_startup_set(2); expect("Custom español antes de montar: menú nativo en español", dm_ui("dmConfirm"), "Aceptar");
        setenv("DM_LANG", "en-US", 1); expect("DM_LANG prevalece sobre la preferencia provisional", dm_ui("dmConfirm"), "OK"); unsetenv("DM_LANG");
        atomic_store(&g_dm_lang, 8); expect("El idioma real del juego prevalece al cargar", dm_ui("dmConfirm"), "确定");
    }
    else if (!strcmp(mode, "tags")) {   // 语言标记 → 原版语言号 | language tag → the original game's language index
        static const struct { const char *t; int want; } c[] = { { "en-US", 0 }, { "en-GB", 0 }, { "zh-Hans-CN", 8 }, { "zh-Hans", 8 }, { "zh", 8 }, { "zh-Hans-HK", 8 }, { "zh-Hans-TW", 8 }, { "zh-Hans-MO", 8 },
            { "zh-Hant-TW", 9 }, { "zh-Hant-HK", 9 }, { "zh-Hant", 9 }, { "zh-HK", 9 }, { "zh_TW", 9 }, { "yue-Hant-HK", 8 }, { "yue-Hans-CN", 8 }, { "yue", 8 }, { "wuu-Hans-CN", 8 },
            { "de-DE", 1 }, { "es-ES", 2 }, { "es-419", 10 }, { "es-MX", 10 }, { "es_ES", 2 }, { "esES", 2 }, { "esMX", 10 }, { "ES-es", 2 }, { "fr-CA", 3 }, { "it-IT", 4 }, { "ko-KR", 5 }, { "pl-PL", 6 }, { "ru-RU", 7 }, { "ja-JP", 11 }, { "pt-BR", 12 }, { "nl-NL", 0 }, { "ar", 0 }, { "zu-ZA", 0 } };
        for (unsigned i = 0; i < sizeof c / sizeof *c; i++) { char w[8], g[8]; snprintf(w, sizeof w, "%d", c[i].want); snprintf(g, sizeof g, "%d", dm_lang_from_tag(c[i].t)); expect(c[i].t, g, w); } }
    else if (!strcmp(mode, "game")) {   // 设置文件写的英文，游戏随后切回简中：以游戏为准；再切到 zhTW（9）：简中以外一律英文 | the settings file says English, then the game switches back to Simplified Chinese: the game wins; then switch to zhTW (9): everything other than Simplified Chinese is English
        expect("游戏没设前按设置文件", dm_ui("dmConfirm"), "OK");
        atomic_store(&g_dm_lang, 8); expect("游戏切到 zhCN", dm_ui("dmConfirm"), "确定");
        atomic_store(&g_dm_lang, 9); expect("游戏切到 zhTW（繁体用中文）", dm_ui("dmConfirm"), "确定");
        atomic_store(&g_dm_lang, 1); expect("游戏切到 deDE（按英文）", dm_ui("dmConfirm"), "OK");
        atomic_store(&g_dm_lang, 2); expect("Cambio del juego a esES", dm_ui("dmConfirm"), "Aceptar");
        atomic_store(&g_dm_lang, 10); expect("Cambio del juego a esMX", dm_ui("dmConfirm"), "Aceptar");
        expect("表里没有的键原样返回", dm_ui("dmNoSuchKey"), "dmNoSuchKey"); }
    else { fprintf(stderr, "用法：t_i18n en|zh|es|esmx|none|sysen|syses|systw|startup|tags|game\n"); return 2; }
    return fails ? 1 : 0;
}
