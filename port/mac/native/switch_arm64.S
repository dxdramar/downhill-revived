// Fiber stacks on arm64 macOS (port/mac/native/fiber.c, finding 075).
//
// void fiber_switch(void **save_sp, void *load_sp)
//   Pushes the callee-saved registers, the frame and link registers and FPCR on the current
//   stack, stores sp in *save_sp, then loads load_sp and pops the same frame from it. FPCR goes
//   with each fiber: the EE threads keep their own float mode, as on Windows (host.c).
//
// void fiber_start(void)
//   Where a new fiber's first switch returns to: x19 holds its entry, x20 its argument
//   (fiber.c builds that first frame); entry(arg) must not return.

    .text
    .p2align 2
    .globl _fiber_switch
_fiber_switch:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    str x9, [sp, #160]
    mov x9, sp
    str x9, [x0]

    mov sp, x1
    ldr x9, [sp, #160]
    msr fpcr, x9
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #176
    ret

    .p2align 2
    .globl _fiber_start
_fiber_start:
    mov x0, x20
    blr x19
    brk #0
