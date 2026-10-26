#include "../dm_vault_ui.c"
#include <assert.h>

module_t g_mod[MAX_MOD]; int g_nmod;

int main(void) {
    _Alignas(8) uint8_t base[8] = {0}, vault[64] = {0}, tab[0xb00] = {0};
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base;
    *(const char **)(vault + 8) = "TouchVault";
    *(uint64_t *)(tab + 0x30) = (uint64_t)(uintptr_t)vault;
    *(uint64_t *)tab = (uint64_t)(uintptr_t)base + VT_TOGGLE;
    uint64_t w = (uint64_t)(uintptr_t)tab;
    for (int on = 0; on <= 1; on++) {
        vu_toggle(w, on, 0);
        assert(tab[0xa88] == on && *(int32_t *)(tab + 0x610) == on);
        for (int i = 0; i < 4; i++) assert(*(int32_t *)(tab + 0x6e8 + i * 4) == on);
        vu_toggle(w, on, 1);
        assert(tab[0xa88] == on && *(int32_t *)(tab + 0x610) == 0);
        for (int i = 0; i < 4; i++) assert(*(int32_t *)(tab + 0x6e8 + i * 4) == 0);
        vu_toggle(w, on, 0);
        assert(tab[0xa88] == on && *(int32_t *)(tab + 0x610) == on);
    }
    *(int32_t *)(tab + 0x6f0) = 3;
    vu_toggle(w, 1, 0);
    assert(tab[0xa88] == 1 && *(int32_t *)(tab + 0x610) == 4);
    vu_toggle(w, 1, 1);
    assert(tab[0xa88] == 1 && *(int32_t *)(tab + 0x610) == 4);
    vu_toggle(w, 0, 0);
    assert(tab[0xa88] == 0 && *(int32_t *)(tab + 0x610) == 0);
    puts("PASS vault toggle selection, covered frames, restoration and checkbox frames");
    return 0;
}
