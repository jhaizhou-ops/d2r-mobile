// 真实录制和登记入口；不用游戏文件或 GPU 图像验证池隔离及预扫描。
// Real recording and registration entry points; verify pool isolation and prescans without game assets or GPU images.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"
#include <assert.h>
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static size_t emit(uint8_t *buffer, size_t offset, unsigned op, const void *payload, size_t size) {
    op_hdr h = {op, (uint32_t)((size + 15) & ~(size_t)7)};
    memcpy(buffer + offset, &h, 8);
    if (size) memcpy(buffer + offset + 8, payload, size);
    return offset + h.size;
}
int main(void) { @autoreleasepool {
    NVNtexturePool large, small, invalid;
    assert(!h_nvnTexturePoolInitialize(&invalid, NULL, 0, 0));
    assert(!h_nvnTexturePoolInitialize(&invalid, NULL, 0, (1 << 20) + 1));
    assert(h_nvnTexturePoolInitialize(&large, NULL, 0, 12544));
    assert(h_nvnTexturePoolInitialize(&small, NULL, 0, 4096));
    nvn_descriptor_pool *a = ((dpool_t *)&large)->host, *b = ((dpool_t *)&small)->host;
    tex_t t1 = {.serial = 13}, t2 = {.serial = 448};
    view_t v = {.set_levels = 1, .base_level = 2, .nlevels = 1};
    h_nvnTexturePoolRegisterTexture(&large, 256, (NVNtexture *)&t1, (NVNtextureView *)&v);
    h_nvnTexturePoolRegisterTexture(&small, 256, (NVNtexture *)&t2, NULL);
    h_nvnTexturePoolRegisterTexture(&large, 4184, (NVNtexture *)&t1, NULL);
    assert(nvn_descriptor_serial(a, 256) == 13 && nvn_descriptor_serial(b, 256) == 448);
    assert(nvn_descriptor_serial(a, 4184) == 13 && nvn_descriptor_serial(b, 4184) == 0);
    t1.serial = 99; v.base_level = 9;
    assert(nvn_descriptor_serial(a, 256) == 13 && a->textures[256].v.base_level == 2);
    assert(!nvn_descriptor_texture(a, -1) && !nvn_descriptor_serial(a, 0xfffff));
    NVNsamplerPool sp1, sp2;
    assert(h_nvnSamplerPoolInitialize(&sp1, NULL, 0, 1024));
    assert(h_nvnSamplerPoolInitialize(&sp2, NULL, 0, 4096));
    samp_t sampler = {.p.minf = 1};
    h_nvnSamplerPoolRegisterSampler(&sp1, 257, (NVNsampler *)&sampler);
    sampler.p.minf = 0; h_nvnSamplerPoolRegisterSampler(&sp2, 257, (NVNsampler *)&sampler);
    assert(((dpool_t *)&sp1)->host->samplers[257].minf == 1);
    assert(((dpool_t *)&sp2)->host->samplers[257].minf == 0);

    NVNcommandBuffer command;
    _Alignas(8) uint8_t memory[4096] = {0}; pool_t storage = {.mem = memory};
    h_nvnCommandBufferInitialize(&command, NULL);
    h_nvnCommandBufferAddCommandMemory(&command, (NVNmemoryPool *)&storage, 0, sizeof memory);
    h_nvnCommandBufferBeginRecording(&command);
    h_nvnCommandBufferSetTexturePool(&command, &large);
    ((dpool_t *)&large)->host = b;
    h_nvnCommandBufferSetTexturePool(&command, &large);
    ((dpool_t *)&large)->host = a;
    h_nvnCommandBufferSetTexturePool(&command, NULL);
    const uint8_t *record = (const uint8_t *)h_nvnCommandBufferEndRecording(&command);
    nvn_descriptor_pool *expected[] = {a, b, NULL}; int count = 0;
    for (;;) {
        op_hdr h; memcpy(&h, record, 8); if (h.op == OP_END) break;
        if (h.op == OP_SET_TEX_POOL) {
            p_set_pool p; memcpy(&p, record + 8, sizeof p);
            assert(count < 3 && p.pool == expected[count++]);
            state_op(h.op, &p); assert(S.texture_pool == p.pool);
        }
        record += h.size;
    }
    assert(count == 3);

    // 同一个句柄未重绑，换池后的预扫描必须读到新池的序号。
    // Without rebinding the handle, a pool switch must change the serial observed by the prescan.
    _Alignas(8) uint8_t sequence[4096] = {0}; size_t offset = 0;
    p_set_pool pa = {a}, pb = {b}; p_bind_handles handle = {.stage = 1, .n = 1, .h = {256}};
    p_draw draw = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3};
    offset = emit(sequence, offset, OP_SET_TEX_POOL, &pa, sizeof pa);
    offset = emit(sequence, offset, OP_BIND_TEX, &handle, sizeof handle);
    offset = emit(sequence, offset, OP_DRAW, &draw, sizeof draw);
    offset = emit(sequence, offset, OP_SET_TEX_POOL, &pb, sizeof pb);
    offset = emit(sequence, offset, OP_DRAW, &draw, sizeof draw);
    emit(sequence, offset, OP_END, NULL, 0);
    g_sdx_ok = 1; sdx_bucket(13, 1); sdx_bucket(448, 1);
    sdx_walk(sequence, 0);
    assert(g_sdx_n == 2 && g_sdx[0].ser == 13 && g_sdx[1].ser == 448);
    fz_cursor cursor = {.p = sequence};
    assert(!fz_next(&cursor, NULL, NULL, NULL) && cursor.bad);
    assert(!tbdr_on());
    h_nvnTexturePoolFinalize(&large);
    assert(a->capacity == 0 && !a->textures && !nvn_descriptor_serial(a, 256));
    assert(nvn_descriptor_serial(b, 256) == 448);
    h_nvnTexturePoolFinalize(&small);
    h_nvnSamplerPoolFinalize(&sp1); h_nvnSamplerPoolFinalize(&sp2);
    puts("Descriptor pools: isolated slots/capacities, immutable recording, prescan switch and CPU storage release passed.");
    return 0;
} }
