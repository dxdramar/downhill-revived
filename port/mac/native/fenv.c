/* The float environment (finding 075). The port was written for SSE's MXCSR: EE code runs with
   round toward zero and flush to zero (kernel.c ee_float_mode), and port/runtime saves and sets
   MXCSR around its work. On the Mac the module's float operations are plain arm64 ones, which
   follow FPCR, so mac_getcsr/mac_setcsr speak MXCSR and keep FPCR and FPSR in step:

     MXCSR RC (bits 13-14: nearest, down, up, zero)  <->  FPCR RMode (bits 22-23: RN, RP, RM, RZ)
     MXCSR FZ (bit 15) or DAZ (bit 6)                 <->  FPCR FZ (bit 24: inputs and results)
     MXCSR flags IE ZE OE UE PE DE (bits 0-5)         <->  FPSR IOC DZC OFC UFC IXC IDC
   The exception masks always read as set: arm64 Macs don't trap float exceptions. */
#include "native.h"

struct w2c_host;

static inline uint64_t fpcr_get(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void fpcr_set(uint64_t v) { __asm__ volatile("msr fpcr, %0" : : "r"(v)); }
static inline uint64_t fpsr_get(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v; }
static inline void fpsr_set(uint64_t v) { __asm__ volatile("msr fpsr, %0" : : "r"(v)); }

enum { RMODE_SHIFT = 22, FPCR_FZ = 1u << 24 };
static const uint32_t rc_of_rmode[4] = {0, 2, 1, 3};  /* RN, RP (up), RM (down), RZ -> MXCSR RC */
static const uint32_t rmode_of_rc[4] = {0, 2, 1, 3};  /* nearest, down, up, zero -> RMode */

u32 w2c_mac_getcsr(struct w2c_mac *unused)
{
    (void)unused;
    uint64_t c = fpcr_get(), s = fpsr_get();
    uint32_t x = 0x1F80u | rc_of_rmode[(c >> RMODE_SHIFT) & 3] << 13;
    if (c & FPCR_FZ) x |= 0x8040u;
    x |= (s & 1) | (s & 2) << 1 | (s & 4) << 1 | (s & 8) << 1 | (s & 16) << 1 | (s & 0x80) >> 6;
    return x;
}

void w2c_mac_setcsr(struct w2c_mac *unused, u32 x)
{
    (void)unused;
    /* msr fpcr serializes the core: the port sets MXCSR around much of its work, mostly to what it
       already is, so a register is written only when it changes */
    uint64_t was = fpcr_get(), c = was & ~((3ull << RMODE_SHIFT) | FPCR_FZ);
    c |= (uint64_t)rmode_of_rc[(x >> 13) & 3] << RMODE_SHIFT;
    if (x & 0x8040u) c |= FPCR_FZ;
    if (c != was) fpcr_set(c);
    uint64_t old = fpsr_get(), s = old & ~0x9Full;
    s |= (x & 1) | (x & 4) >> 1 | (x & 8) >> 1 | (x & 16) >> 1 | (x & 32) >> 1 | (x & 2) << 6;
    if (s != old) fpsr_set(s);
}

/* A new host thread starts as Windows threads do: round to nearest, no flushing. kernel.c sets
   the EE's mode on the game's fibers. */
void fpcr_ee_default(void) { fpcr_set(fpcr_get() & ~((3ull << RMODE_SHIFT) | FPCR_FZ)); }

/* port/runtime/ee_native_float.h: one double operation at round to nearest, FPCR restored after
   it (the flush bit kept, as the SSE version keeps MXCSR's). The asm keeps the operation between
   the two FPCR writes. */
#define DOUBLE_OP(name, insn) \
    f64 w2c_host_##name##_host(struct w2c_host *unused, f64 a, f64 b) \
    { \
        (void)unused; \
        uint64_t saved = fpcr_get(); \
        double r; \
        fpcr_set(saved & ~(3ull << RMODE_SHIFT)); \
        __asm__ volatile(insn " %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); \
        fpcr_set(saved); \
        return r; \
    }
DOUBLE_OP(ps2_native_dadd, "fadd")
DOUBLE_OP(ps2_native_dsub, "fsub")
DOUBLE_OP(ps2_native_dmul, "fmul")
DOUBLE_OP(ps2_native_ddiv, "fdiv")
