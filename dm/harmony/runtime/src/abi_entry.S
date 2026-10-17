// AAPCS64边界检查器：x19-x28和d8-d15设哨兵并检查，保存/恢复调用者原值。
// 不碰x18/TPIDR；并未验证任意游戏指令、SVE、异常或异步信号路径。
.text
.p2align 4
.global xblr_abi_enter
.hidden xblr_abi_enter
.type xblr_abi_enter,@function
.macro GPR_CHECK reg, value, bit
    cmp \reg, #\value
    cset x11, ne
    orr x10, x10, x11, lsl #\bit
.endm
.macro FP_CHECK reg, value, bit
    fmov x9, \reg
    cmp x9, #\value
    cset x11, ne
    orr x10, x10, x11, lsl #\bit
.endm
xblr_abi_enter:
    .cfi_startproc
    stp x29, x30, [sp, #-208]!
    .cfi_def_cfa_offset 208
    .cfi_offset x29, -208
    .cfi_offset x30, -200
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    .cfi_offset x19, -192
    .cfi_offset x20, -184
    .cfi_offset x21, -176
    .cfi_offset x22, -168
    .cfi_offset x23, -160
    .cfi_offset x24, -152
    .cfi_offset x25, -144
    .cfi_offset x26, -136
    .cfi_offset x27, -128
    .cfi_offset x28, -120
    .cfi_offset d8, -112
    .cfi_offset d9, -104
    .cfi_offset d10, -96
    .cfi_offset d11, -88
    .cfi_offset d12, -80
    .cfi_offset d13, -72
    .cfi_offset d14, -64
    .cfi_offset d15, -56
    str x2, [sp, #176]
    mov x19, #0x19
    mov x20, #0x20
    mov x21, #0x21
    mov x22, #0x22
    mov x23, #0x23
    mov x24, #0x24
    mov x25, #0x25
    mov x26, #0x26
    mov x27, #0x27
    mov x28, #0x28
    mov x9, #8
    fmov d8, x9
    mov x9, #9
    fmov d9, x9
    mov x9, #10
    fmov d10, x9
    mov x9, #11
    fmov d11, x9
    mov x9, #12
    fmov d12, x9
    mov x9, #13
    fmov d13, x9
    mov x9, #14
    fmov d14, x9
    mov x9, #15
    fmov d15, x9
    bl xblr_guest_entry
    str x0, [sp, #184]
    mov x10, #0
    GPR_CHECK x19, 0x19, 0
    GPR_CHECK x20, 0x20, 1
    GPR_CHECK x21, 0x21, 2
    GPR_CHECK x22, 0x22, 3
    GPR_CHECK x23, 0x23, 4
    GPR_CHECK x24, 0x24, 5
    GPR_CHECK x25, 0x25, 6
    GPR_CHECK x26, 0x26, 7
    GPR_CHECK x27, 0x27, 8
    GPR_CHECK x28, 0x28, 9
    FP_CHECK d8, 8, 10
    FP_CHECK d9, 9, 11
    FP_CHECK d10, 10, 12
    FP_CHECK d11, 11, 13
    FP_CHECK d12, 12, 14
    FP_CHECK d13, 13, 15
    FP_CHECK d14, 14, 16
    FP_CHECK d15, 15, 17
    ldr x9, [sp, #176]
    str x10, [x9]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldr x0, [sp, #184]
    ldp x29, x30, [sp], #208
    .cfi_def_cfa_offset 0
    ret
    .cfi_endproc
.size xblr_abi_enter, .-xblr_abi_enter
.hidden xblr_guest_entry
.section .note.GNU-stack,"",@progbits
