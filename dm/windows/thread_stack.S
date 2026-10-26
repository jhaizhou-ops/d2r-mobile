// Windows ARM64 客体栈桥：保存宿主非易失寄存器与 TEB 栈界限，退出时先回宿主栈再跑 pthread 析构。
// Windows ARM64 guest-stack bridge: save host nonvolatile registers and TEB stack bounds; return before pthread destructors.
// TEB+0x1478 是 ARM64 DeallocationStack，GetCurrentThreadStackLimits 用它作为预留栈底。
// TEB+0x1478 is ARM64 DeallocationStack, the reservation limit returned by GetCurrentThreadStackLimits.
    .text
    .globl dm_windows_guest_enter
    .p2align 2
dm_windows_guest_enter:
    stp x19, x20, [x3, #0]
    stp x21, x22, [x3, #16]
    stp x23, x24, [x3, #32]
    stp x25, x26, [x3, #48]
    stp x27, x28, [x3, #64]
    stp x29, x30, [x3, #80]
    stp d8, d9, [x3, #96]
    stp d10, d11, [x3, #112]
    stp d12, d13, [x3, #128]
    stp d14, d15, [x3, #144]
    mov x9, sp
    str x9, [x3, #160]
    ldp x9, x10, [x18, #8]
    stp x9, x10, [x3, #168]
    ldr x9, [x18, #0x1478]
    str x9, [x3, #184]
    stp x2, x4, [x18, #8]
    str x4, [x18, #0x1478]
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl dm_thread_returned
    brk #0

    .globl dm_windows_guest_leave
    .p2align 2
dm_windows_guest_leave:
    ldp x9, x10, [x0, #168]
    stp x9, x10, [x18, #8]
    ldr x9, [x0, #184]
    str x9, [x18, #0x1478]
    ldr x9, [x0, #160]
    mov sp, x9
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldp d8, d9, [x0, #96]
    ldp d10, d11, [x0, #112]
    ldp d12, d13, [x0, #128]
    ldp d14, d15, [x0, #144]
    ret
