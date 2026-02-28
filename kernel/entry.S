        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        # a0 contains hartid from firmware/qemu entry.
        la sp, stack0
        mv a1, a0
        addi t0, a1, 1
        li t1, 1024*4
        mul t0, t0, t1
        add sp, sp, t0
        mv a0, a1
        # jump to start() in start.c
        call start
spin:
        j spin
