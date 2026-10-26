// 用合成模块和覆盖层标记验证 Windows 桌面模式：键盘键帽、鼠标不会切成缺少原生控件的触屏版。
// Verify Windows desktop mode with a synthetic module and overlay markers: keyboard glyphs and no mouse-triggered switch to a touch layout without native controls.
#include <assert.h>
#include "../compat/include/dm_windows.h"
#include "../input.h"
#include "../../host/dm_padmode.c"

module_t g_mod[MAX_MOD];
int g_nmod = 1;
static char sidecar[1100];

// 未覆盖的游戏执行路径一律中止，不能由成功桩放过。 | Untested game execution paths abort, never pass through success-shaped stubs.
uint64_t dm_guest_frame(void) { abort(); }
void dm_hud_get(dm_hud_info *out) { (void)out; abort(); }
void dm_hud_request_players(int count) { (void)count; abort(); }
int dm_ui_hover_now(int x, int y) { (void)x; (void)y; abort(); }
const char *dm_str(uint8_t *base, const char *key) { (void)base; (void)key; abort(); }
uint32_t dm_oe_ui_opmode_reads(void) { abort(); }
void dm_oe_ui_opmode(int mode) { (void)mode; abort(); }
int dm_ingame_now(void) { abort(); }
int dm_ui_fe_drag_ok(void) { abort(); }
int dm_ui_panel_state(void) { abort(); }
int dm_swapskill_wset(void) { abort(); }
int dm_opmode(void) { abort(); }
void dm_runeword_ui_rebuild(void) { abort(); }
void dm_ui_cache_mode_leave(void) { abort(); }
void dm_update_mode_enter(void) { abort(); }
void dm_dead_modal_mode_enter(void) { abort(); }
void dm_ui_auto_mode_leave(void) { abort(); }
void dm_cube_side_mode_leave(void) { abort(); }
unsigned char *CC_SHA1(const void *data, CC_LONG size, unsigned char *digest) {
    (void)data; (void)size; (void)digest; abort();
}
int gate_obs_present(int id) { assert(id == OBS_NOTICE_INPUT); return 0; }
void gate_obs_set(int id, void (*fn)(const uint64_t *)) { (void)id; (void)fn; abort(); }
int dm_overlay_file(const char *relative, char *out, size_t size) {
    assert(!strcmp(relative, "ctrl_touch") || !strcmp(relative, "kbd_glyphs") ||
           !strcmp(relative, "xbox_glyphs") || !strcmp(relative, "ps_glyphs") ||
           !strcmp(relative, "touch_strings.tsv"));
    assert(strlen(sidecar) < size);
    strcpy(out, sidecar); return 1;
}
int main(int argc, char **argv) {
    assert(dm_windows_key_bit('J') == 0 && dm_windows_key_bit(VK_RBUTTON) == 1);
    assert(dm_windows_key_bit(VK_RETURN) == 40 && dm_windows_key_bit(VK_ESCAPE) == 41);
    assert(dm_windows_key_bit(VK_OEM_PLUS) == 10 && dm_windows_key_bit(VK_RIGHT) == 14);
    assert(dm_windows_key_bit(VK_F12) == -1);
    float nx, ny;
    assert(dm_windows_pointer(1280, 720, 16.f / 9, 640, 360, &nx, &ny) && nx == .5f && ny == .5f);
    assert(dm_windows_pointer(1600, 720, 16.f / 9, 0, 360, &nx, &ny) && nx < 0 && ny == .5f);
    assert(dm_windows_pointer(1280, 900, 16.f / 9, 640, 0, &nx, &ny) && nx == .5f && ny < 0);
    assert(!dm_windows_pointer(0, 720, 16.f / 9, 0, 0, &nx, &ny));
    int desktop = argc == 2 && !strcmp(argv[1], "--desktop");
    wchar_t temp[MAX_PATH], file[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temp));
    assert(GetTempFileNameW(temp, L"dmp", 0, file));
    assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, file, -1, sidecar, sizeof sidecar, NULL, NULL));
    assert(!setenv("DM_ROMFS_OVERLAY", "synthetic", 1));
    if (desktop) assert(!setenv("DM_APP", "1", 1));
    else assert(!unsetenv("DM_APP"));
    strcpy(g_mod[0].name, "main");
    g_mod[0].base = calloc(1, 0x2000000); assert(g_mod[0].base);
    for (unsigned i = 0; i < sizeof k_pfx / sizeof *k_pfx; i++)
        strcpy((char *)g_mod[0].base + k_pfx[i].off, k_pfx[i].pad);
    dm_padmode_boot();
    assert(g_have && dm_padmode_current() == desktop);
    assert(dm_padmode_kbd() == desktop);
    assert(g_glyph_have[DM_GLYPH_KBD] == desktop);
    assert(g_glyph_have[DM_GLYPH_PS]);
    assert(!strcmp(k_glyph_name[DM_GLYPH_PS], "PlayStation"));
    dm_padmode_request(0);
    assert(atomic_load(&g_want) == desktop);
    assert(!dm_padmode_busy());
    dm_padmode_request_glyph(DM_GLYPH_XBOX);
    assert(atomic_load(&g_glyph_want) == DM_GLYPH_XBOX);
    dm_padmode_request_glyph(DM_GLYPH_PS);
    assert(atomic_load(&g_glyph_want) == DM_GLYPH_PS);
    g_glyph_have[DM_GLYPH_PS] = 0;
    dm_padmode_request_glyph(DM_GLYPH_PS);
    assert(atomic_load(&g_glyph_want) == DM_GLYPH_PAD);
    dm_padmode_request_glyph(DM_GLYPH_KBD);
    assert(atomic_load(&g_glyph_want) == (desktop ? DM_GLYPH_KBD : DM_GLYPH_PAD));
    free(g_mod[0].base);
    assert(DeleteFileW(file));
    return 0;
}
