// 自造 ELF 记录验证弱导入：缺失时为零，已有定义仍解析，强导入保留桩。
// Synthetic ELF records: absent weak imports are null, defined ones resolve, and strong imports retain stubs.
#include "../loader.c"
#include <assert.h>

struct dm_flags g_flag;
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *function) {
    (void)kind; (void)name; return function;
}
HLE("host_provided", int, host_provided, void) { return 7; }

int main(void) {
    _Alignas(16) uint8_t importer[4096] = {0}, exporter[4096] = {0};
    const char names[] = "\0optional\0provided\0host_provided\0required";
    Elf64_Sym imports[5] = {
        {0}, {.st_name=1, .st_info=0x20}, {.st_name=10, .st_info=0x20},
        {.st_name=19, .st_info=0x20}, {.st_name=33, .st_info=0x10}
    };
    Elf64_Sym exports[2] = {{0}, {.st_name=10, .st_info=0x10, .st_shndx=1, .st_value=0x100}};
    Elf64_Dyn dynamic[] = {
        {.d_tag=DT_RELA, .d_un.d_ptr=0x300},
        {.d_tag=DT_RELASZ, .d_un.d_val=4*sizeof(Elf64_Rela)}, {0}
    };
    Elf64_Rela *rel = (Elf64_Rela *)(importer + 0x300);
    for (int i=0; i<4; i++) rel[i] = (Elf64_Rela){.r_offset=0x200+i*8,
        .r_info=((uint64_t)(i+1)<<32)|R_AARCH64_GLOB_DAT};
    g_mod[0]=(module_t){.base=importer, .size=sizeof importer, .symtab=imports,
        .strtab=names, .nsym=5, .dyn=dynamic};
    g_mod[1]=(module_t){.base=exporter, .size=sizeof exporter, .symtab=exports,
        .strtab=names, .nsym=2};
    g_nmod=2;
    relocate(&g_mod[0]);
    uint64_t *values=(uint64_t *)(importer+0x200);
    assert(values[0]==0); // 可选回调缺失，客体应跳过 | Optional callback absent: guest must skip it
    assert(values[1]==(uintptr_t)(exporter+0x100)); // 客体提供弱导入 | Weak import with guest definition
    assert(values[2]==(uintptr_t)host_provided); // 宿主提供弱导入 | Weak import with host definition
    assert(values[3]!=0); // 未解析的强导入保留记名桩 | Unresolved strong import retains a named stub
    assert(resolve_sym(&g_mod[0], "required", 1)==0); // 不能复用强导入的桩 | Must not reuse a strong import stub
    puts("Host ELF linking: absent weak, guest-defined weak, host-defined weak, strong named stub passed.");
    return 0;
}
