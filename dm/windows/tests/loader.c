// 复用真实 ELF 重定位测试；未测试的游戏执行路径一律中止，不用成功桩掩盖依赖。
// Reuse the actual ELF relocation test; untested game execution paths abort instead of using success-shaped stubs.
#define main weak_import_test
#include "../../host/tests/t_loader_weak.c"
#undef main
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
uint64_t guest_ns(void) { abort(); }
void hlecount_seal(void) { abort(); }
uint64_t svc_dispatch(int number, uint64_t *registers, uint64_t origin) {
    (void)number; (void)registers; (void)origin; abort();
}
int main(int argc, char **argv) {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 3 && !strcmp(argv[1], "--layout")) {
        module_t module = {.name = "synthetic"};
        read_layout(&module, argv[2]);
        assert(DM_PATCH_TARGET == 3 && module.patch_off == 0x10000);
        return 0;
    }
    return weak_import_test();
}
