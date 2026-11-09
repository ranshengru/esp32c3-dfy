.section .text.entry, "ax", @progbits
.global _start
_start:
    csrci mstatus, 8
    li t0, 0x600080a8
    li t1, 0x50d83aa1
    sw t1, 0(t0)
    li t0, 0x600080a4
    li t1, 0x80000000
    sw t1, 0(t0)
    li t0, 0x60008090
    lw t1, 0(t0)
    li t2, 0x7fffefff
    and t1, t1, t2
    sw t1, 0(t0)
    li t0, 0x600080a8
    sw zero, 0(t0)
    li sp, 0x3fcce000
    tail screen_test

.section .app_desc, "a", @progbits
.align 4
.global companion_app_desc
companion_app_desc:
    .long 0xabcd5432
    .long 0
    .long 0, 0
    .asciz "v0.1"
    .zero 27
    .asciz "companion"
    .zero 22
    .zero 16
    .zero 16
    .zero 32
    .zero 32
    .zero 80
