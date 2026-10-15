.text
.p2align 2
.global dm_android_gate_check
.type dm_android_gate_check,%function
dm_android_gate_check:
    stp x19, x30, [sp, #-16]!
    mov x19, #0x7777
    bl dm_gate
.global dm_android_gate_check_return
dm_android_gate_check_return:
    mov x0, x19
    ldp x19, x30, [sp], #16
    ret
.size dm_android_gate_check, .-dm_android_gate_check
.section .note.GNU-stack,"",%progbits
