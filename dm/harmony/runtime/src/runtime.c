// 独立合成客体原型。没有原游戏逻辑、NSO执行或Apple线程槽布局。
#include "xblr_runtime.h"
#include "context_offsets.h"
#include <stdatomic.h>

typedef struct guest_context {
    uint64_t virtual_x18;
    uint64_t *tls;
    uint64_t *tlr;
    uint64_t host_calls, callbacks, before_host, after_host, host_tls;
} guest_context;
#define CHECK_OFFSET(field, offset) _Static_assert(offsetof(guest_context, field) == offset, "guest context ABI")
CHECK_OFFSET(virtual_x18, CTX_V18); CHECK_OFFSET(tls, CTX_TLS); CHECK_OFFSET(tlr, CTX_TLR);
CHECK_OFFSET(host_calls, CTX_HOST_CALLS); CHECK_OFFSET(callbacks, CTX_CALLBACKS);
CHECK_OFFSET(before_host, CTX_BEFORE); CHECK_OFFSET(after_host, CTX_AFTER); CHECK_OFFSET(host_tls, CTX_HOST_TLS);
_Static_assert(sizeof(void *) == 8, "AArch64 only");

extern const unsigned char __xblr_guest_base[], __xblr_guest_text_end[], __xblr_guest_ro_end[], __xblr_guest_data_end[];
extern const uint64_t xblr_guest_ro[];
extern uint64_t xblr_guest_data[];
extern uint64_t xblr_guest_entry(guest_context *, uint64_t);
extern uint64_t xblr_guest_callback(guest_context *, uint64_t);
extern uint64_t xblr_abi_enter(guest_context *, uint64_t, uint64_t *);
static atomic_flag run_lock = ATOMIC_FLAG_INIT;

// AAPCS64 callback. 显式传递客体context，不覆盖宿主TPIDR/x18，不用pthread内部布局。
__attribute__((noinline, visibility("hidden")))
uint64_t xblr_host_gate(guest_context *ctx, uint64_t arg)
{
    ctx->host_calls++;
    ctx->host_tls = ctx->tls[0];
    ctx->virtual_x18 += 7;
    ctx->tls[0] ^= arg;
    return xblr_guest_callback(ctx, arg + ctx->tlr[0]) + ctx->tls[0];
}

int xblr_get_layout(xblr_layout *out, size_t out_bytes)
{
    if (!out || out_bytes != sizeof *out) return XBLR_BAD_ARGUMENT;
    uintptr_t base = (uintptr_t)__xblr_guest_base;
    *out = (xblr_layout){ .abi_version = XBLR_ABI_VERSION, .struct_bytes = sizeof *out,
        .input_kind = XBLR_EMBEDDED_SYNTHETIC, .image_base = base,
        .text_offset = 0, .text_bytes = (uintptr_t)__xblr_guest_text_end - base,
        .ro_offset = (uintptr_t)xblr_guest_ro - base,
        .ro_bytes = (uintptr_t)__xblr_guest_ro_end - (uintptr_t)xblr_guest_ro,
        .data_offset = (uintptr_t)xblr_guest_data - base,
        .data_bytes = (uintptr_t)__xblr_guest_data_end - (uintptr_t)xblr_guest_data,
        .entry_offset = (uintptr_t)xblr_guest_entry - base,
        .callback_offset = (uintptr_t)xblr_guest_callback - base };
    if (out->ro_offset != 0x4000 || out->data_offset != 0x8000 || out->text_bytes > 0x4000 ||
        out->ro_bytes != 32 || out->data_bytes != 32) return XBLR_LAYOUT_MISMATCH;
    return XBLR_OK;
}

int xblr_run(uint32_t input_kind, uint64_t seed, xblr_result *out, size_t out_bytes)
{
    if (!out || out_bytes != sizeof *out) return XBLR_BAD_ARGUMENT;
    *out = (xblr_result){ .abi_version = XBLR_ABI_VERSION, .struct_bytes = sizeof *out,
        .input_kind = input_kind, .seed = seed };
    if (input_kind != XBLR_EMBEDDED_SYNTHETIC) {
        out->status = input_kind == XBLR_ORIGINAL_NSO ? XBLR_REAL_NSO_NOT_READY : XBLR_BAD_ARGUMENT;
        return (int)out->status;
    }
    if (atomic_flag_test_and_set_explicit(&run_lock, memory_order_acquire)) {
        out->status = XBLR_BUSY;
        return XBLR_BUSY;
    }
    xblr_layout layout;
    int status = xblr_get_layout(&layout, sizeof layout);
    if (status == XBLR_OK) {
        const uint64_t v18 = seed ^ UINT64_C(0x1818181818181818);
        uint64_t tls[2] = { seed + 3, UINT64_C(0x7171717171717171) };
        uint64_t tlr[2] = { seed ^ UINT64_C(0x7474747474747474), UINT64_C(0x7272727272727272) };
        guest_context ctx = { .virtual_x18 = v18, .tls = tls, .tlr = tlr };
        const uint64_t before = (seed + UINT64_C(0x1020304050607080)) ^ v18;
        const uint64_t after = ((before + tlr[0]) ^ (v18 + 7)) + ((seed + 3) ^ before);
        const uint64_t runs_before = xblr_guest_data[0];
        out->expected = after ^ UINT64_C(0xfeedfacedeadbeef);
        out->actual = xblr_abi_enter(&ctx, seed, &out->abi_bad_mask);
        out->host_calls = ctx.host_calls; out->guest_callbacks = ctx.callbacks;
        out->virtual_x18_before = v18; out->virtual_x18_after = ctx.virtual_x18;
        out->tls_before = seed + 3; out->tls_after = tls[0]; out->host_observed_tls = ctx.host_tls;
        out->tlr_value = tlr[0]; out->guest_before_host = ctx.before_host; out->guest_after_host = ctx.after_host;
        out->embedded_runs = xblr_guest_data[0]; out->embedded_last = xblr_guest_data[1];
        if (out->abi_bad_mask) status = XBLR_ABI_MISMATCH;
        else if (out->actual != out->expected || ctx.host_calls != 1 || ctx.callbacks != 1 ||
            ctx.virtual_x18 != v18 + 7 || tls[0] != ((seed + 3) ^ before) || ctx.host_tls != seed + 3 ||
            ctx.before_host != before || ctx.after_host != after || xblr_guest_data[0] != runs_before + 1 ||
            xblr_guest_data[1] != out->actual || tls[1] != UINT64_C(0x7171717171717171) ||
            tlr[1] != UINT64_C(0x7272727272727272)) status = XBLR_RESULT_MISMATCH;
    }
    out->status = (uint32_t)status;
    atomic_flag_clear_explicit(&run_lock, memory_order_release);
    return status;
}
