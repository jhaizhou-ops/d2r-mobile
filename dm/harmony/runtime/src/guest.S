// 人工合成AArch64客体：不是NSO翻译结果。context是显式参数，绝不访问硬件x18或TPIDR。
#include "context_offsets.h"
.section .xblr.guest.text,"ax",@progbits
.p2align 4
.global xblr_guest_entry
.hidden xblr_guest_entry
.type xblr_guest_entry,@function
xblr_guest_entry:
    .cfi_startproc
    stp x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset x29, -32
    .cfi_offset x30, -24
    mov x29, sp
    stp x19, x20, [sp, #16]
    .cfi_offset x19, -16
    .cfi_offset x20, -8
    mov x19, x0
    adrp x9, xblr_guest_ro
    add x9, x9, :lo12:xblr_guest_ro
    ldr x10, [x9]
    add x1, x1, x10
    ldr x10, [x19, #CTX_V18]
    eor x1, x1, x10
    str x1, [x19, #CTX_BEFORE]
    bl xblr_host_gate
    str x0, [x19, #CTX_AFTER]
    adrp x9, xblr_guest_data
    add x9, x9, :lo12:xblr_guest_data
    ldr x10, [x9, #16]
    eor x0, x0, x10
    str x0, [x9, #8]
    ldr x10, [x9]
    add x10, x10, #1
    str x10, [x9]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #32
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
.size xblr_guest_entry, .-xblr_guest_entry
.p2align 4
.global xblr_guest_callback
.hidden xblr_guest_callback
.type xblr_guest_callback,@function
xblr_guest_callback:
    .cfi_startproc
    ldr x9, [x0, #CTX_CALLBACKS]
    add x9, x9, #1
    str x9, [x0, #CTX_CALLBACKS]
    ldr x10, [x0, #CTX_V18]
    eor x0, x1, x10
    ret
    .cfi_endproc
.size xblr_guest_callback, .-xblr_guest_callback
.hidden xblr_host_gate

.section .xblr.guest.ro,"a",@progbits
.p2align 3
.global xblr_guest_ro
.hidden xblr_guest_ro
.type xblr_guest_ro,@object
xblr_guest_ro:
    .quad 0x1020304050607080
    .ascii "XBLR-SYNTH-V1\0\0\0"
    .quad 1
.size xblr_guest_ro, .-xblr_guest_ro

.section .xblr.guest.data,"aw",@progbits
.p2align 3
.global xblr_guest_data
.hidden xblr_guest_data
.type xblr_guest_data,@object
xblr_guest_data:
    .quad 0, 0, 0xfeedfacedeadbeef, 0
.size xblr_guest_data, .-xblr_guest_data
.section .note.GNU-stack,"",@progbits
