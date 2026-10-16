// 合成宿主着色器，验证阶段绑定不再读取退役后被复用的客体快照。
// Synthetic host shaders verify that stage bindings do not read retired and reused guest snapshots.
#include "../nvn/nvn_mtl.m"
#include <assert.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(void) { @autoreleasepool {
    prog_t vertex = {.magic = MAGIC('P','R','O','G'), .serial = nvn_new_serial()};
    // 上游渲染单测使用无 magic 的合成程序，只由序号表提供着色器。
    // Upstream renderer tests use synthetic programs without magic, with shaders supplied by the serial table.
    prog_t fragment = {.serial = nvn_new_serial()};
    NvProg *v = [NvProg new], *f = [NvProg new];
    v->sh[0] = [NvShader new]; f->sh[1] = [NvShader new];
    serial_put(vertex.serial, v); serial_put(fragment.serial, f);
    const prog_t *bound = bind_program(NULL, &(p_bind_program){&vertex, 1});
    bound = bind_program(bound, &(p_bind_program){&fragment, 2});
    assert(prog_get(bound)->sh[0] == v->sh[0] && prog_get(bound)->sh[1] == f->sh[1]);
    mtl_prog_release(&vertex); mtl_prog_release(&fragment);
    memset(&vertex, 0, sizeof vertex); memset(&fragment, 0, sizeof fragment);
    const prog_t *same = bind_program(bound, &(p_bind_program){NULL, 32});
    assert(same == bound);
    assert(prog_get(same)->sh[0] == v->sh[0] && prog_get(same)->sh[1] == f->sh[1]);
    const prog_t *only_vertex = bind_program(bound, &(p_bind_program){NULL, 2});
    assert(prog_get(only_vertex)->sh[0] == v->sh[0] && !prog_get(only_vertex)->sh[1]);
    vertex = (prog_t){.magic = MAGIC('P','R','O','G'), .serial = nvn_new_serial()};
    NvProg *replacement = [NvProg new]; replacement->sh[0] = [NvShader new];
    serial_put(vertex.serial, replacement);
    const prog_t *changed = bind_program(bound, &(p_bind_program){&vertex, 1});
    assert(changed != bound && prog_get(changed)->sh[0] == replacement->sh[0]);
    assert(prog_get(changed)->sh[1] == f->sh[1] && prog_get(bound)->sh[0] == v->sh[0]);
    assert(!bind_program(changed, &(p_bind_program){NULL, 63}));
    puts("Stage binding survives snapshot retirement/reuse and preserves unrelated stages.");
    return 0;
} }
