// Persistencia pública de Custom: migración y guardado de otros ajustes no cambian el idioma.
#include "../dm_custom.h"
#include "../dm_i18n.h"
#include "../hle.h"
#include <unistd.h>
#include <sys/stat.h>
struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
const char *dm_save_commitdir(void) { return NULL; }
void dm_3d_quality_mode_set(int mode) { (void)mode; }
int dm_3d_quality_mode_get(void) { return 0; }
float dm_3d_scale_cur(void) { return 1; }
static int fails;
#define CHECK(c, what) do { int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", what); } while (0)
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    unsetenv("DM_LANG"); unsetenv("DM_APP"); atomic_store(&g_dm_lang, 2);
    char dir[] = "/tmp/d2r-custom-XXXXXX", path[128], contents[2048], save_dir[128], settings_dir[160], settings_file[192];
    if (!mkdtemp(dir)) return 2;
    snprintf(save_dir, sizeof save_dir, "%s/save", dir); mkdir(save_dir, 0700);
    snprintf(settings_dir, sizeof settings_dir, "%s/Settings", save_dir); mkdir(settings_dir, 0700);
    snprintf(settings_file, sizeof settings_file, "%s/Settings.json", settings_dir); setenv("DM_SAVE", save_dir, 1);
    snprintf(path, sizeof path, "%s/sw_custom.txt", dir);
    FILE *f = fopen(path, "w"); if (!f) return 2;
    int want;
    if (!strcmp(argv[1], "es")) { fputs("language_index=2\nauto_mana=0\n", f); want = 2; }
    else if (!strcmp(argv[1], "mac")) { setenv("DM_APP", "1", 1); fputs("language_index=2\nauto_mana=0\ndrop_haptic=0\ntouch_swap_above=1\n", f); want = 2; }
    else if (!strcmp(argv[1], "legacyen")) { fputs("lang_en=1\nauto_mana=0\n", f); want = 0; }
    else if (!strcmp(argv[1], "legacyzh")) { fputs("lang_en=0\nauto_mana=0\n", f); want = 8; }
    else if (!strcmp(argv[1], "legacyzh_tw") || !strcmp(argv[1], "legacyzh_es") || !strcmp(argv[1], "legacy_mix_9_2") || !strcmp(argv[1], "legacy_mix_2_9")) {
        int text = !strcmp(argv[1], "legacyzh_tw") || !strcmp(argv[1], "legacy_mix_9_2") ? 9 : 2;
        int voice = !strcmp(argv[1], "legacy_mix_9_2") ? 2 : !strcmp(argv[1], "legacy_mix_2_9") ? 9 : text;
        FILE *settings = fopen(settings_file, "w"); if (!settings) return 2;
        fprintf(settings, "{ \"Language\": %d, \"VoiceLanguage\": %d }", text, voice); fclose(settings);
        fputs("lang_en=0\nauto_mana=0\n", f); want = -1;
    }
    else if (!strcmp(argv[1], "precedence")) { fputs("language_index=2\nlang_en=0\nauto_mana=0\n", f); want = 2; }
    else if (!strcmp(argv[1], "esmx")) { fputs("language_index=10\nauto_mana=0\n", f); want = 10; }
    else if (!strcmp(argv[1], "none")) { fputs("auto_mana=0\n", f); want = -1; }
    else if (!strcmp(argv[1], "invalid")) { fputs("language_index=22\nauto_mana=0\n", f); want = -1; }
    else { fclose(f); unlink(path); rmdir(dir); return 2; }
    fclose(f); dm_custom_init(dir);
    CHECK(dm_custom_lang_explicit() == want, "Lee el índice original o migra el booleano anterior");
    dm_custom_on_uimsg("OptionsPanelMessage", "ResetToDefaults", NULL);
    dm_custom_on_uimsg("OptionsPanelMessage", "CancelChanges", NULL);
    CHECK(dm_custom_get("auto_mana") == 0, "Descartar conserva los ajustes guardados");
    CHECK(dm_custom_lang_explicit() == want, "Descartar conserva el idioma");
    dm_custom_on_uimsg("OptionsPanelMessage", "ResetToDefaults", NULL);
    dm_custom_on_uimsg("OptionsPanelMessage", "AcceptChanges", NULL);
    CHECK(dm_custom_get("auto_mana") == 1, "Aplicar guarda el cambio de otro ajuste");
    CHECK(dm_custom_lang_explicit() == want, "Predeterminados y Aplicar conservan el idioma");
    if (!strcmp(argv[1], "mac")) CHECK(dm_custom_get("drop_haptic") == 0 && dm_custom_get("touch_swap_above") == 1, "Mac conserva los ajustes móviles ocultos al aplicar predeterminados");
    f = fopen(path, "r"); if (!f) return 2;
    size_t n = fread(contents, 1, sizeof contents - 1, f); fclose(f); contents[n] = 0;
    char expected[48]; snprintf(expected, sizeof expected, "language_index=%d\n", want);
    CHECK(want < 0 ? !strstr(contents, "language_index=") : strstr(contents, expected) != NULL, "El archivo conserva el índice exacto sin convertir español en inglés");
    CHECK(!strstr(contents, "lang_en="), "El guardado usa el formato de tres idiomas");
    if (!strncmp(argv[1], "legacy_mix_", 11)) {
        int text = !strcmp(argv[1], "legacy_mix_9_2") ? 9 : 2, voice = text == 9 ? 2 : 9;
        CHECK(dm_lang_saved() == text && dm_voice_saved() == voice, "Guardar otras opciones conserva textos y voces mixtos por separado");
        dm_lang_startup_set(dm_custom_lang_explicit());
        CHECK(dm_voice_index() == voice, "El validador recibe el idioma de voz efectivo, independiente del texto");
        setenv("DM_LANG", "es-ES", 1); CHECK(dm_voice_index() == 2, "DM_LANG explícita prevalece sobre voz mixta guardada");
    }
    unlink(path); unlink(settings_file); rmdir(settings_dir); rmdir(save_dir); rmdir(dir);
    return fails ? 1 : 0;
}
