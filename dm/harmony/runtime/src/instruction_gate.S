/* Complete instruction boundary. x18 remains the host platform register.
 * Site stub allocated 256 bytes (including original-instruction stack headroom),
 * saved original x16/x30, and passed metadata x16.
 * All live state is saved BEFORE the first C/pthread call. No red-zone stores.
 */
.text
.p2align 4
.hidden xblr_instruction_gate
.global xblr_instruction_gate
.type xblr_instruction_gate,%function
xblr_instruction_gate:
    sub sp, sp, #832
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    str x17, [sp, #136]
    str x19, [sp, #152]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x16, [sp, #800]
    str x18, [sp, #808]
    add x9, sp, #832
    ldp x9, x10, [x9]
    str x9, [sp, #128]
    str x10, [sp, #240]
    add x9, sp, #1088
    str x9, [sp, #248]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    mrs x9, fpcr
    str x9, [sp, #776]
    mrs x9, fpsr
    str x9, [sp, #784]
    mov x0, sp
    bl xblr_instruction_dispatch
    /* Future guest SP may differ after a pre/post-index original instruction.
     * Place only deferred x16/x17 at the final allocated 16-byte tail frame.
     */
    ldr x9, [sp, #248]
    sub x9, x9, #16
    ldp x10, x11, [sp, #128]
    stp x10, x11, [x9]
    ldr x10, [sp, #776]
    msr fpcr, x10
    ldr x10, [sp, #784]
    msr fpsr, x10
    ldr x10, [sp, #768]
    msr nzcv, x10
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    mov x17, x9
    ldr x16, [sp, #792]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldr x19, [sp, #152]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    mov sp, x17
    br x16
.size xblr_instruction_gate,.-xblr_instruction_gate
.p2align 4
.global sw_thread_enter
.hidden sw_thread_enter
.type sw_thread_enter,%function
sw_thread_enter:
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl sw_thread_returned
    brk #0x733
.size sw_thread_enter,.-sw_thread_enter
.section .note.GNU-stack,"",@progbits
