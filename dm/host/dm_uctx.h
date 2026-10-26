// 信号现场（ucontext_t）里的寄存器：苹果与 Linux（安卓）布局不同，宿主一律经这几个宏读写。
// UC_ESR：出错时的 ESR_EL1（缺页的 WnR 位 = 写访问）；Linux 上它在 __reserved 的扩展记录链里（ESR_MAGIC），没有时为 0。
// Registers in the signal context (ucontext_t): the layouts differ between Apple and Linux (Android), so the host always reads and writes them through these macros.
// UC_ESR: ESR_EL1 at the time of the fault (the WnR bit of a page fault = write access); on Linux it is in the extension record chain in __reserved (ESR_MAGIC), 0 when absent.
#pragma once
#include <stdint.h>
#include <sys/ucontext.h>
#ifdef _WIN32
#define UC_PC(uc) ((uc)->context->Pc)
#define UC_LR(uc) ((uc)->context->Lr)
#define UC_SP(uc) ((uc)->context->Sp)
#define UC_FP(uc) ((uc)->context->Fp)
#define UC_X(uc, i) ((uc)->context->X[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) { return uc->esr; }
#elif defined(__APPLE__)
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define UC_LR(uc) ((uc)->uc_mcontext->__ss.__lr)
#define UC_SP(uc) ((uc)->uc_mcontext->__ss.__sp)
#define UC_FP(uc) ((uc)->uc_mcontext->__ss.__fp)
#define UC_X(uc, i) ((uc)->uc_mcontext->__ss.__x[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) { return uc->uc_mcontext->__es.__esr; }
#else
#define UC_PC(uc) ((uc)->uc_mcontext.pc)
#define UC_LR(uc) ((uc)->uc_mcontext.regs[30])
#define UC_SP(uc) ((uc)->uc_mcontext.sp)
#define UC_FP(uc) ((uc)->uc_mcontext.regs[29])
#define UC_X(uc, i) ((uc)->uc_mcontext.regs[i])
static inline uint64_t UC_ESR(const ucontext_t *uc) {
    const uint8_t *p = (const uint8_t *)uc->uc_mcontext.__reserved, *end = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= end) {
        uint32_t magic = *(const uint32_t *)p, size = *(const uint32_t *)(p + 4);
        if (!magic || size < 8) break;
        if (magic == 0x45535201u && size >= 16) return *(const uint64_t *)(p + 8);   // ESR_MAGIC
        p += size;
    }
    return 0;
}
#endif
