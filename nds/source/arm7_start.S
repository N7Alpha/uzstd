    .section .init, "ax"
    .arm
    .global _start

_start:
    ldr sp, =0x0380ff00
1:
    b 1b
