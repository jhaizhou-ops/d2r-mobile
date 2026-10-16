#include "../hle_misc.c"
#include "../hle_fs.c"
#include <assert.h>

void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; abort(); }
struct dm_flags g_flag;
uint64_t g_t0_ns;
module_t g_mod[MAX_MOD];
int g_nmod;
uint64_t now_ns(void) { return 0; }
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) { (void)id; (void)ns; (void)bytes; }
void dm_guest_write_prepare(void *address, size_t size) { (void)address; (void)size; }
int dm_custom_lang_explicit(void) { return -1; }
int dm_sys_lang(void) { return 0; }

int main(int argc, char **argv) {
    (void)argv;
    int list[8] = {0};
    assert(h_aoc_List(NULL, 0, 8) == 0 && h_aoc_List(list, -1, 8) == 0);
    if (argc == 2) {
        assert(h_aoc_Count() == 0 && h_aoc_List(list, 0, 8) == 0);
        return 0;
    }
    assert(h_aoc_Count() == 1 && h_aoc_List(list, 0, 8) == 1 && list[0] == 1);
    char temporary[] = "/tmp/dm-addon-test.XXXXXX";
    assert(mkdtemp(temporary));
    int order[] = {6, 3, 1, 4, 2, 5};
    for (size_t i = 0; i < 6; i++) {
        char directory[1024], file[1100];
        snprintf(directory, sizeof directory, "%s/%d", temporary, order[i]);
        assert(mkdir(directory, 0700) == 0);
        snprintf(file, sizeof file, "%s/DATA0.bin", directory);
        FILE *output = fopen(file, "wb");
        assert(output && fputc(order[i], output) != EOF && fclose(output) == 0);
        assert(dm_addon_register(order[i], directory));
        assert(!dm_addon_register(order[i], directory));
    }
    assert(!dm_addon_register(0, temporary));
    assert(!dm_addon_register(7, "relative/path"));
    assert(h_aoc_Count() == 6 && h_aoc_List(list, 0, 8) == 6);
    for (int i = 0; i < 6; i++) assert(list[i] == i + 1);
    assert(h_aoc_List(list, 4, 8) == 2 && list[0] == 5 && list[1] == 6);
    assert(h_aoc_List(NULL, 0, 8) == 0 && h_aoc_List(list, -1, 8) == 0);
    assert(h_aoc_List(list, 9, 8) == 0 && h_aoc_List(list, 0, 0) == 0);
    size_t cache = 0;
    assert(h_fs_QueryAddonCache(&cache, 1) == RES_OK && cache == 0x1000);
    assert(h_fs_QueryAddonCache(&cache, 7) != RES_OK);
    assert(h_fs_MountAddon("extra", 1, NULL, cache) == RES_OK);
    uint64_t handle;
    unsigned char byte = 0;
    size_t count = 0;
    assert(h_fs_OpenFile(&handle, "extra:/DATA0.bin", MODE_READ) == RES_OK);
    assert(h_fs_ReadFile(&count, handle, 0, &byte, 1) == RES_OK && count == 1 && byte == 1);
    h_fs_CloseFile(handle);
    assert(h_fs_OpenFile(&handle, "extra:/DATA0.bin", MODE_WRITE) != RES_OK);
    assert(h_fs_OpenFile(&handle, "extra:/../DATA0.bin", MODE_READ) != RES_OK);
    assert(h_fs_MountAddon("extra", 2, NULL, cache) != RES_OK);
    h_fs_Unmount("extra");
    assert(h_fs_OpenFile(&handle, "extra:/DATA0.bin", MODE_READ) != RES_OK);
    assert(h_fs_MountAddon("extra", 2, NULL, cache) == RES_OK);
    assert(h_fs_OpenFile(&handle, "extra:/DATA0.bin", MODE_READ) == RES_OK);
    assert(h_fs_ReadFile(&count, handle, 0, &byte, 1) == RES_OK && byte == 2);
    h_fs_CloseFile(handle);
    assert(h_fs_MountAddon("rom", 1, NULL, cache) == RES_OK);
    assert(!g_mnt[g_nmnt - 1].overlay[0]);
    assert(mount("saved", temporary, 0) == RES_OK);
    assert(h_fs_MountAddon("saved", 1, NULL, cache) == RES_PATH_EXISTS);
    for (int i = 0; i < 100; i++) {
        h_fs_Unmount("extra");
        assert(h_fs_MountAddon("extra", i % 2 + 1, NULL, cache) == RES_OK);
    }
    assert(rm_tree(temporary) == 0);
    puts("DLC: sorted six-item enumeration, pagination, read-only mounting and mount-name reuse passed.");
}
