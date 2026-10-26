/* Cooperative EE fibers on POSIX: one native stack per fiber plus a small
   assembly context switch (callee-saved registers only). Same fixed guest
   stacks (256 KB slots at 0x06000000, lowest free) and the same
   __stack_pointer / wasm_rt_call_stack_depth save/restore per fiber as
   fiber_x64.c. No ucontext (slow and deprecated); the switch below is written
   for this task, no third-party code. The FP control word is saved per fiber
   like FIBER_FLAG_FLOAT_SWITCH does on Windows: MXCSR on x86_64, FPCR on
   aarch64. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(__aarch64__)
#else
#error Implement POSIX fibers for this architecture
#endif
#include "wasm-rt.h"
#include "core_host.h"
#if !WASM_RT_STACK_DEPTH_COUNT
#error Build all wasm2c runtime units with WASM_RT_STACK_DEPTH_COUNT=1
#endif

/* Each slot's lowest page stays reserved: an overflow faults (the OS fault
   reporter) instead of running into the slot below. */
#if defined(__APPLE__) && defined(__aarch64__)
enum { GUARD=0x4000 };  /* a whole 16 KiB page: commits round to pages */
#else
enum { GUARD=0x1000 };
#endif
enum { SLOTS=64, ROOT=65, STACK_BASE=0x06000000, STACK_BYTES=0x40000 };
enum { NATIVE_STACK=8*1024*1024, NATIVE_GUARD=0x1000 };
typedef struct {
    void *save_sp; void *stack; void (*fn)(void *); void *arg;
    uint32_t sp, depth; uint64_t fp; int active;
} Fiber;
static Fiber fibers[ROOT+1];
static uint32_t *module_sp;
static int current, owned;
static pthread_t owner_thread;

#if defined(__x86_64__)
/* rdi=&save_sp, rsi=new_sp. New fibers enter through fiber_trampoline with
   rbx holding their Fiber; fiber_entry never returns. */
__asm__(
".text\n"
".globl fiber_switch_asm\n"
".type fiber_switch_asm,@function\n"
"fiber_switch_asm:\n"
"  push %rbp\n"
"  push %rbx\n"
"  push %r12\n"
"  push %r13\n"
"  push %r14\n"
"  push %r15\n"
"  mov %rsp,(%rdi)\n"
"  mov %rsi,%rsp\n"
"  pop %r15\n"
"  pop %r14\n"
"  pop %r13\n"
"  pop %r12\n"
"  pop %rbx\n"
"  pop %rbp\n"
"  ret\n"
".globl fiber_trampoline\n"
".type fiber_trampoline,@function\n"
"fiber_trampoline:\n"
"  mov %rbx,%rdi\n"
"  call fiber_entry\n"
"  ud2\n"
);
#elif defined(__aarch64__)
/* x0=&save_sp, x1=new_sp. The callee-saved registers of AAPCS64: x19-x28, the frame pair and
   d8-d15 (the low halves of v8-v15). New fibers enter through fiber_trampoline with x19 holding
   their Fiber; fiber_entry never returns. Mach-O names carry a leading underscore and have no
   .type. */
#if defined(__APPLE__)
#define FIBER_SYM(name) "_" #name
#define FIBER_TYPE(name) ""
#else
#define FIBER_SYM(name) #name
#define FIBER_TYPE(name) ".type " #name ",@function\n"
#endif
__asm__(
".text\n"
".p2align 2\n"
".globl " FIBER_SYM(fiber_switch_asm) "\n"
FIBER_TYPE(fiber_switch_asm)
FIBER_SYM(fiber_switch_asm) ":\n"
"  stp x29, x30, [sp,#-16]!\n"
"  stp x19, x20, [sp,#-16]!\n"
"  stp x21, x22, [sp,#-16]!\n"
"  stp x23, x24, [sp,#-16]!\n"
"  stp x25, x26, [sp,#-16]!\n"
"  stp x27, x28, [sp,#-16]!\n"
"  stp d8, d9, [sp,#-16]!\n"
"  stp d10, d11, [sp,#-16]!\n"
"  stp d12, d13, [sp,#-16]!\n"
"  stp d14, d15, [sp,#-16]!\n"
"  mov x2, sp\n"
"  str x2, [x0]\n"
"  mov sp, x1\n"
"  ldp d14, d15, [sp],#16\n"
"  ldp d12, d13, [sp],#16\n"
"  ldp d10, d11, [sp],#16\n"
"  ldp d8, d9, [sp],#16\n"
"  ldp x27, x28, [sp],#16\n"
"  ldp x25, x26, [sp],#16\n"
"  ldp x23, x24, [sp],#16\n"
"  ldp x21, x22, [sp],#16\n"
"  ldp x19, x20, [sp],#16\n"
"  ldp x29, x30, [sp],#16\n"
"  ret\n"
".globl " FIBER_SYM(fiber_trampoline) "\n"
FIBER_TYPE(fiber_trampoline)
FIBER_SYM(fiber_trampoline) ":\n"
"  mov x0, x19\n"
"  bl " FIBER_SYM(fiber_entry) "\n"
"  brk #0\n"
);
#endif
void fiber_switch_asm(void **save_sp, void *new_sp);
void fiber_trampoline(void);
void fiber_entry(Fiber *f) {
    f->fn(f->arg);
    /* EE wrappers normally never return. Returning here resumes the root. */
    host_fiber_switch(ROOT); abort();
}

#if defined(__x86_64__)
static uint64_t fp_get(void) { return _mm_getcsr(); }
static void fp_set(uint64_t v) { _mm_setcsr((unsigned)v); }
#elif defined(__aarch64__)
static uint64_t fp_get(void) {
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v;
}
static void fp_set(uint64_t v) {
    __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory");
}
#endif

static void check_thread(void) {
    if (!owned || !pthread_equal(owner_thread,pthread_self())) abort();
}
void host64_fiber_bind(uint32_t *sp) {
    if (current || !sp) abort();
    module_sp=sp; owner_thread=pthread_self(); owned=1;
}
HostFiber host_fiber_main(void) {
    check_thread(); if (!module_sp) abort(); if (current) return current;
    fibers[ROOT].sp=*module_sp; fibers[ROOT].depth=wasm_rt_call_stack_depth;
    fibers[ROOT].fp=fp_get(); fibers[ROOT].active=1; fibers[ROOT].save_sp=NULL;
    current=ROOT; return ROOT;
}
HostFiber host_fiber_create(void (*fn)(void *),void *arg) {
    check_thread(); if (!current || !fn) return 0;
    for (int i=1;i<=SLOTS;i++) if (!fibers[i].active) {
        Fiber *f=&fibers[i]; f->fn=fn; f->arg=arg;
        f->sp=STACK_BASE+i*STACK_BYTES; f->depth=0;
        f->fp=fp_get();
        if (!host64_memory_commit(f->sp-STACK_BYTES+GUARD,STACK_BYTES-GUARD)) return 0;
        memset((void *)(guest_base+f->sp-STACK_BYTES+GUARD),0,STACK_BYTES-GUARD);
        void *map=mmap(NULL,NATIVE_STACK+NATIVE_GUARD,PROT_READ|PROT_WRITE,
            MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if (map==MAP_FAILED) return 0;
        if (mprotect(map,NATIVE_GUARD,PROT_NONE)<0) { munmap(map,NATIVE_STACK+NATIVE_GUARD); return 0; }
        f->stack=map;
#if defined(__x86_64__)
        /* Six popped registers plus the trampoline return address; the base
           sits at rsp%16==8 so the trampoline's call keeps the ABI. */
        uintptr_t base=(((uintptr_t)map+NATIVE_STACK+NATIVE_GUARD-64)&~15ull)+8;
        void **slot=(void **)base;
        slot[0]=slot[1]=slot[2]=slot[3]=NULL; slot[4]=f; slot[5]=NULL;
        slot[6]=fiber_trampoline;
        f->save_sp=(void *)base;
#elif defined(__aarch64__)
        /* What fiber_switch_asm pops, in its order: d8-d15 (slots 0-7), x27/x28, x25/x26,
           x23/x24, x21/x22 (8-15), x19 = the Fiber and x20 (16-17), then x29 = 0 and x30 =
           the trampoline (18-19), where ret goes. The base stays 16-aligned as the ABI requires. */
        uintptr_t base=((uintptr_t)map+NATIVE_STACK+NATIVE_GUARD-160)&~15ull;
        void **slot=(void **)base;
        for (int k=0;k<20;k++) slot[k]=NULL;
        slot[16]=f;
        slot[19]=(void *)fiber_trampoline;
        f->save_sp=(void *)base;
#endif
        f->active=1;
        return i;
    }
    return 0;
}
void host_fiber_switch(HostFiber to) {
    check_thread(); if (!current || to<1 || to>ROOT || !fibers[to].active) abort();
    if (current==to) return;
    int from=current;
    fibers[from].sp=*module_sp; fibers[from].depth=wasm_rt_call_stack_depth;
    fibers[from].fp=fp_get();
    *module_sp=fibers[to].sp; wasm_rt_call_stack_depth=fibers[to].depth;
    fp_set(fibers[to].fp);
    current=to; fiber_switch_asm(&fibers[from].save_sp,fibers[to].save_sp);
}
void host_fiber_delete(HostFiber id) {
    check_thread(); if (id<1 || id>SLOTS || id==current) abort();
    if (fibers[id].active) munmap(fibers[id].stack,NATIVE_STACK+NATIVE_GUARD);
    memset(&fibers[id],0,sizeof fibers[id]);
}
HostFiber host_fiber_current(void) { check_thread(); return current; }
int host64_fiber_id(void) { return current; }  /* diagnostics: any thread */
uint32_t host64_fiber_stack_top(HostFiber id) {
    return id>=1 && id<=SLOTS ? STACK_BASE+id*STACK_BYTES : 0;
}
int host_stack_ranges(uintptr_t (**ranges)[2]) {
    static uintptr_t result[SLOTS][2]; int n=0; check_thread();
    for (int i=1;i<=SLOTS;i++) if (fibers[i].active) {
        result[n][0]=STACK_BASE+(i-1)*STACK_BYTES;
        result[n++][1]=STACK_BASE+i*STACK_BYTES;
    }
    *ranges=result; return n;
}
void host64_fiber_shutdown(void) {
    check_thread(); if (current!=ROOT) abort();
    for (int i=1;i<=SLOTS;i++) host_fiber_delete(i);
    memset(fibers,0,sizeof fibers); current=0; module_sp=NULL; owned=0;
}
