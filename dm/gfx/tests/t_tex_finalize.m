// 销毁旧场景纹理：两个别名都必须退役，另一个纹理不受影响；已编码的 GPU 读仍须完成。
// Finalize an old-scene texture: retire both aliases, preserve an unrelated texture, and finish an encoded GPU read.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failures;
#define CHECK(c, name) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", name); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t old = {.serial = 7301}, other = {.serial = 7302};
    old.d.width = old.d.height = 4;
    old.d.depth = old.d.levels = old.d.samples = 1;
    old.d.target = NVN_TEXTURE_TARGET_2D; old.d.format = NVN_FORMAT_RGBA8;
    other.d = old.d;
    mtl_sub_begin();
    mtl_reg_tex(10, &old, NULL); mtl_reg_tex(11, &old, NULL); mtl_reg_tex(12, &other, NULL);
    pthread_mutex_lock(&g_rmu);
    tex_by_id(10); tex_by_id(11); tex_by_id(12);
    pthread_mutex_unlock(&g_rmu);
    CHECK(g_treg[10].view && g_treg[11].view, "both aliases populated");
    void *untouched = g_treg[12].view;
    uint64_t generation = g_reg_gen;

    memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &old;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    rp_begin_ex(1); rp_end();
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(readback);
    [blit() copyFromTexture:tex_mtl(&old) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1)
                  toBuffer:readback destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:64];
    mtl_tex_release(&old);
    CHECK(!g_treg[10].view && !g_treg[11].view, "finalized aliases released");
    CHECK(g_treg[12].view == untouched, "unrelated alias preserved");
    CHECK(g_treg[10].ok && g_treg[10].t.serial == old.serial, "guest descriptor snapshot preserved");
    CHECK(g_reg_gen > generation, "argument buffer cache invalidated");
    CHECK(!serial_get(old.serial), "finalized serial removed");
    CHECK(serial_get(other.serial) != nil, "unrelated serial preserved");
    uint64_t fence = mtl_sub_commit();
    CHECK(mtl_wait(fence, 5000000000ull), "GPU finished after finalize");
    CHECK(!mtl_last_commit_error(), "no GPU resource fault");
    const uint8_t *pixel = readback.contents;
    CHECK(pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 && pixel[3] == 255, "encoded read retained the old texture contents");
    mtl_tex_release(&old); // 重复销毁不应碰其他别名 | Repeated finalization must not affect another alias.
    CHECK(g_treg[12].view == untouched, "repeated finalize safe");
    printf("tex_finalize: %d checks, %d failures\n", checks, failures);
    return failures != 0;
} }
