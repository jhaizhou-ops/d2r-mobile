// Tabla de textos del port: dm/i18n/ui_text.tsv; generador: dm/ios/tools/dm_i18n.py.
// El juego conserva sus trece idiomas. Los textos del port tienen chino, inglés y español.
#include "dm_i18n.h"
#include "dm_i18n_tab.h"
#include <TargetConditionals.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Atomic int g_dm_lang = -1;
static _Atomic int g_startup_lang = -1;
void dm_lang_startup_set(int index) { atomic_store(&g_startup_lang, index >= 0 && index < 13 ? index : -1); }

#if TARGET_OS_IPHONE
#include <CoreFoundation/CoreFoundation.h>
#endif

int dm_lang_from_tag(const char *tag) {
    if (!tag || !*tag) return 2;
    char t[64]; size_t i;
    for (i = 0; i < sizeof t - 1 && tag[i]; i++) t[i] = tag[i] == '_' ? '-' : (char)tolower((unsigned char)tag[i]);
    t[i] = 0;
    static const struct { const char *tag; int index; } exact[] = {
        { "enus", 0 }, { "dede", 1 }, { "eses", 2 }, { "frfr", 3 }, { "itit", 4 }, { "kokr", 5 },
        { "plpl", 6 }, { "ruru", 7 }, { "zhcn", 8 }, { "zhtw", 9 }, { "esmx", 10 }, { "jajp", 11 }, { "ptbr", 12 }
    };
    for (unsigned k = 0; k < sizeof exact / sizeof *exact; k++) if (!strcmp(t, exact[k].tag)) return exact[k].index;
    char lang[4] = { 0 }; i = 0; while (i < 3 && t[i] && t[i] != '-') { lang[i] = t[i]; i++; }
    if (!strcmp(lang, "es")) return strstr(t, "-mx") || strstr(t, "-419") ? 10 : 2;
    if (!strcmp(lang, "zh")) return strstr(t, "-hant") || !strcmp(t, "zh-tw") || !strcmp(t, "zh-hk") || !strcmp(t, "zh-mo") ? 9 : 8;
    static const char *const zh[] = { "yue", "wuu", "hak", "nan", "cmn", "gan", "hsn", "lzh" };
    for (unsigned k = 0; k < sizeof zh / sizeof *zh; k++) if (!strcmp(lang, zh[k])) return 8;
    static const struct { const char *tag; int index; } languages[] = {
        { "de", 1 }, { "fr", 3 }, { "it", 4 }, { "ko", 5 }, { "pl", 6 }, { "ru", 7 }, { "ja", 11 }, { "pt", 12 }
    };
    for (unsigned k = 0; k < sizeof languages / sizeof *languages; k++) if (!strcmp(lang, languages[k].tag)) return languages[k].index;
    return 0;
}

int dm_sys_lang(void) {
    const char *e = getenv("DM_LANG"); if (e && *e) return dm_lang_from_tag(e);
#if TARGET_OS_IPHONE
    int r = 8; CFArrayRef a = CFLocaleCopyPreferredLanguages();
    if (a) { if (CFArrayGetCount(a) > 0) { char t[64]; if (CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(a, 0), t, sizeof t, kCFStringEncodingUTF8)) r = dm_lang_from_tag(t); } CFRelease(a); }
    return r;
#else
    return 2; // Variante Mac en español; DM_LANG explícita conserva prioridad.
#endif
}

static int settings_language(const char *key) {
    const char *sv = getenv("DM_SAVE"); if (!sv) return -1;
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", sv);
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char quoted[32]; snprintf(quoted, sizeof quoted, "\"%s\"", key);
    const char *k = strstr(buf, quoted); if (!k) return -1;
    k = strchr(k + strlen(quoted), ':'); if (!k) return -1;
    char *end; long value = strtol(k + 1, &end, 10);
    if (end == k + 1 || value < 0 || value >= 13) return -1;
    while (isspace((unsigned char)*end)) end++;
    return *end == ',' || *end == '}' ? (int)value : -1;
}
int dm_lang_saved(void) { return settings_language("Language"); }
int dm_voice_saved(void) { return settings_language("VoiceLanguage"); }
int dm_voice_index(void) {
    const char *environment = getenv("DM_LANG"); if (environment && *environment) return dm_lang_from_tag(environment);
    int startup = atomic_load(&g_startup_lang); if (startup >= 0) return startup;
    int voice = dm_voice_saved(); return voice >= 0 ? voice : dm_sys_lang();
}

int dm_lang_index(void) {
    int l = atomic_load(&g_dm_lang); if (l >= 0 && l < 13) return l;
    const char *e = getenv("DM_LANG"); if (e && *e) return dm_lang_from_tag(e);
    int startup = atomic_load(&g_startup_lang); if (startup >= 0) return startup;
    if (!getenv("DM_SAVE")) return dm_sys_lang(); // Los diálogos iniciales no fijan el idioma de unos ajustes aún sin ruta.
    static _Atomic int file = -2; int f = atomic_load(&file); if (f == -2) { f = dm_lang_saved(); atomic_store(&file, f); }
    return f >= 0 && f < 13 ? f : dm_sys_lang();
}
int dm_lang_zh(void) { int l = dm_lang_index(); return l == 8 || l == 9; }
int dm_lang_es(void) { int l = dm_lang_index(); return l == 2 || l == 10; }

const char *dm_ui(const char *key) {
    for (unsigned i = 0; i < sizeof k_dm_i18n / sizeof *k_dm_i18n; i++) if (!strcmp(k_dm_i18n[i].k, key))
        return dm_lang_zh() ? k_dm_i18n[i].zh : dm_lang_es() ? k_dm_i18n[i].es : k_dm_i18n[i].en;
    return key;
}

const char *dm_str(uint8_t *b, const char *key) {
    if (!b) return dm_ui(key);
    struct { const char *k; uint64_t n; } s = { key, strlen(key) };
    const char *r = ((const char *(*)(void *))(b + 0x7f5630))(&s);
    return r && *r && strcmp(r, key) ? r : dm_ui(key);
}
