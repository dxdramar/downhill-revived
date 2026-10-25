/* EE float arithmetic for natively compiled game code. port/tools/build.py includes this in every
   unit and rewrites the unit's IR (port/tools/eefloat.py): float add, subtract and divide, and all
   double arithmetic, become calls to these. Multiplies and int-to-float conversions stay native:
   EE code runs rounding toward zero (kernel.c ee_float_mode), as PCSX2 runs the EE FPU, and that
   is already their result.
   - ADD/SUB: PCSX2's single guard bit (iFPU.cpp FPU_ADD_SUB): the smaller operand loses the
     mantissa bits alignment would shift out past it, then the sum rounds toward zero.
   - DIV: rounds to nearest (PCSX2's FPUDivFPCR).
   - double: the game's doubles were IEEE soft-float, rounded to nearest; the conversion to float
     (__truncdfsf2) too.
   Same rules as port/runtime/ps2float.h's ps2_ee_*, without its exceptional-value clamps: native
   code didn't have them before either. Builtin types only, so no header can clash with the game's. */
#ifndef DHD_EE_NATIVE_FLOAT_H
#define DHD_EE_NATIVE_FLOAT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Unmangled in both languages, for the rewritten IR to call: C++ gives static functions
   decorated names, and a gnu89 C inline function would be defined in every object. */
#ifdef __cplusplus
#define PS2_NATIVE inline __attribute__((always_inline, used))
#else
#define PS2_NATIVE static inline __attribute__((always_inline, used))
#endif

PS2_NATIVE void ps2_native_align(float *a, float *b)
{
    __UINT32_TYPE__ x, y;
    __builtin_memcpy(&x, a, 4); __builtin_memcpy(&y, b, 4);
    int shift = (int)((x >> 23) & 255) - (int)((y >> 23) & 255);
    if (shift >= 25) y &= 0x80000000u;
    else if (shift > 0) y &= 0xffffffffu << (shift - 1);
    else if (shift <= -25) x &= 0x80000000u;
    else if (shift < 0) x &= 0xffffffffu << (-shift - 1);
    __builtin_memcpy(a, &x, 4); __builtin_memcpy(b, &y, 4);
}

PS2_NATIVE float ps2_native_fadd(float a, float b) { ps2_native_align(&a, &b); return a + b; }
PS2_NATIVE float ps2_native_fsub(float a, float b) { ps2_native_align(&a, &b); return a - b; }

/* To the nearest float by its bits, whatever MXCSR's mode (ps2float.h ps2_float_nearest). */
PS2_NATIVE float ps2_native_dtof(double value)
{
    __UINT64_TYPE__ bits;
    __builtin_memcpy(&bits, &value, 8);
    bits += 0x0fffffffu + ((bits >> 29) & 1);
    bits &= ~(__UINT64_TYPE__)0x1fffffff;
    __builtin_memcpy(&value, &bits, 8);
    return (float)value;
}

PS2_NATIVE float ps2_native_fdiv(float a, float b)
{
    if (b == 0)  /* as before: the host's infinity or NaN */
        return a / b;
    return ps2_native_dtof((double)a / (double)b);
}

#ifdef __wasm__
/* wasm has no rounding modes: the Mac host does the operation at round-to-nearest, FPCR restored
   after it (port/mac/fenv.c). The wasm code's own float operations run in the host's FPCR mode,
   round toward zero like the EE (wasm2c makes them plain C). */
#define PS2_NATIVE_DOUBLE(name, insn) \
    __attribute__((import_module("host"), import_name(#name))) double name##_host(double, double); \
    PS2_NATIVE double name(double a, double b) { return name##_host(a, b); }
#else
/* One SSE2 double operation at round-to-nearest, MXCSR restored after it. */
#define PS2_NATIVE_DOUBLE(name, insn) \
    PS2_NATIVE double name(double a, double b) \
    { \
        unsigned saved, nearest; \
        __asm__ volatile("stmxcsr %0" : "=m"(saved)); \
        nearest = saved & ~0x6000u; \
        __asm__ volatile("ldmxcsr %2\n\t" insn " %3, %0\n\tldmxcsr %1" \
                         : "+x"(a) : "m"(saved), "m"(nearest), "x"(b)); \
        return a; \
    }
#endif
PS2_NATIVE_DOUBLE(ps2_native_dadd, "addsd")
PS2_NATIVE_DOUBLE(ps2_native_dsub, "subsd")
PS2_NATIVE_DOUBLE(ps2_native_dmul, "mulsd")
PS2_NATIVE_DOUBLE(ps2_native_ddiv, "divsd")
#undef PS2_NATIVE_DOUBLE
#undef PS2_NATIVE

#ifdef __cplusplus
}
#endif

#endif
