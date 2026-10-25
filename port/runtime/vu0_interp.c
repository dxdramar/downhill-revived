/* VU0 micro mode: runs the microcode the game uploaded (VIF0 MPG, port/hle/vif.c) from micro
   memory, for the vcallms entries with no native version (vu0_micro.c). Instruction pairs as the
   VU User's Manual documents them; the decoding follows port/tools/vudis.py.

   Arithmetic is host float, as in the macro mode runtime (ps2asm.c): no fused multiply-add, the
   same operation order. Timing is modelled where results depend on it:
   - an FMAC or load result is ready 4 cycles after issue; an instruction reading a register that
     isn't ready stalls until it is (whole register, as the dest fields of in-flight writes);
   - MAC, status and clip flags from an instruction are seen by flag reads 4 cycles after it
     issues, counting stalls (finding 016);
   - Q is written when the division ends (DIV/SQRT 7 cycles, RSQRT 13); a new one waits for the
     last, WAITQ waits for it, q-operands read the Q of the moment.
   The integer unit's results are ready on the next instruction. */
#include "ps2asm.h"
#include "ps2float.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <intrin.h>

enum { FMAC_LATENCY = 4, FLAG_LATENCY = 4, DIV_LATENCY = 7, RSQRT_LATENCY = 13 };
enum { MAX_STEPS = 1 << 20 };  /* a program that never ends (bad upload) gives up */

#ifndef PS2_VU_REFERENCE
static ps2_vu_kick_trace_fn kick_trace;
void ps2_vu1_set_kick_trace(ps2_vu_kick_trace_fn trace) { kick_trace = trace; }
#endif

typedef struct {
    ps2_cpu *c;
    /* A run is bounded to MAX_STEPS and the longest stall is 54 cycles.
       32-bit timestamps cover the entire run, including the final drain. */
    uint32_t cycle;
    uint32_t vf_ready[32];
    uint32_t component_ready[32][4];
    struct { uint32_t at, mac, status, clip, mask, kind; } flags[16];
    int n_flags, flag_head;
    uint32_t flag_cycle;  /* the cycle the flags are read at: the current pair's issue */
    uint32_t clip_next;
    uint32_t mac, status, clip;  /* what flag reads see now */
    uint32_t q_ready;
    float q_next;
    uint32_t q_flags;
    uint32_t p_ready;
    float p_next;
    unsigned unit, code_mask, data_mask, top, itop;
    ps2_vu_kick_fn kick;
    void *opaque;
    unsigned vi_previous_reg;
    uint16_t vi_previous_value;
    uint32_t vi_written_at;
    int vu1_used;
    ps2_q128 vu1_file[64]; /* VU0 only: VU1 VF/VI as VU0 addresses them at 0x4000, filled on first use */
} Vu;

/* A pending upper result: written after the lower instruction of the same pair has read. */
typedef struct {
    int reg, dest, to_acc, raw;
    float v[4];
    uint32_t bits[4];
    int flags;          /* 1: MAC and status flags, 2: a clip judgement */
    uint32_t mac, clip_bits;
} Upper;

#define BIT(w, n) ((w) >> (n) & 1)
#define FT(w) ((w) >> 16 & 31)
#define FS(w) ((w) >> 11 & 31)
#define FD(w) ((w) >> 6 & 31)
#define DEST(w) ((w) >> 21 & 15)  /* bit 3 x, 2 y, 1 z, 0 w */
#define HAS(d, comp) ((d) >> (3 - (comp)) & 1)

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static float vf(Vu *u, int r, int comp)
{
    return r ? u->c->vf[r].f[comp] : comp == 3 ? 1.0f : 0.0f;
}

static uint32_t vf_bits(Vu *u, int r, int comp)
{
    float f = vf(u, r, comp);
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

/* Whole registers as vectors: one 16-byte load or store instead of four lanes through a stack
   array, which the host can't forward from the narrow stores into the wide load. */
static inline __attribute__((always_inline)) __m128 vf4(Vu *u, int r)
{
    return r ? _mm_loadu_ps(u->c->vf[r].f) : _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
}

static inline __attribute__((always_inline)) __m128 lane4(__m128 v, int comp)
{
    switch (comp) {
    case 0: return _mm_shuffle_ps(v, v, 0x00);
    case 1: return _mm_shuffle_ps(v, v, 0x55);
    case 2: return _mm_shuffle_ps(v, v, 0xAA);
    default: return _mm_shuffle_ps(v, v, 0xFF);
    }
}

/* Stores the fields in d (bit 3 x .. bit 0 w) of v at p, keeping the others. */
static inline __attribute__((always_inline)) void store_fields(void *p, __m128i v, int d)
{
    if (d == 15) { _mm_storeu_si128((__m128i *)p, v); return; }
    __m128i keep = _mm_setr_epi32(d & 8 ? 0 : -1, d & 4 ? 0 : -1, d & 2 ? 0 : -1, d & 1 ? 0 : -1);
    __m128i old = _mm_loadu_si128((const __m128i *)p);
    _mm_storeu_si128((__m128i *)p, _mm_or_si128(_mm_and_si128(keep, old), _mm_andnot_si128(keep, v)));
}

/* PCSX2's interpreter clamps exponent-255 arithmetic operands and flushes
   denormals. Bitwise moves, MIN/MAX and conversion instructions keep raw bits. */
static float arithmetic(Vu *u, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, 4);
    if (!(bits & 0x7f800000)) bits &= 0x80000000;
    else if ((bits & 0x7f800000) == 0x7f800000) bits = (bits & 0x80000000) | 0x7f7fffff;
    memcpy(&value, &bits, 4);
    return value;
}

static float arithmetic_vf(Vu *u, int reg, int comp) { return arithmetic(u, vf(u, reg, comp)); }

/* SSE2 lanes use the same MXCSR rounding and separate multiply/add as the
   scalar reference. Clamp raw exponent bits before performing arithmetic. */
static __m128 arithmetic4(__m128 value)
{
    __m128i bits = _mm_castps_si128(value), exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    __m128i sign = _mm_and_si128(bits, _mm_set1_epi32((int)0x80000000));
    __m128i small = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    __m128i large = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7f800000));
    bits = _mm_or_si128(_mm_andnot_si128(small, bits), _mm_and_si128(small, sign));
    bits = _mm_or_si128(_mm_andnot_si128(large, bits),
                        _mm_and_si128(large, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff))));
    return _mm_castsi128_ps(bits);
}

/* arithmetic4 of an FMAC operand, in fewer steps, for code running under the VU MXCSR
   (ps2_vu1_run: toward zero, FTZ, DAZ, exceptions masked). DAZ reads a denormal operand as
   the signed zero arithmetic4 would make of it, so only exponent 255 needs the clamp. The
   operation then sees finite operands, so its result is finite and normal or zero: overflow
   toward zero gives the largest finite magnitude and FTZ flushes. arithmetic4 of the result is
   therefore the identity and is skipped (fuzzed against arithmetic4 for ADD/SUB/MUL/MADD/MSUB,
   test_vu1.c). The empty asm hides the operand from the compiler: it must not fold the
   operation (x * 1.0 = x would let a denormal through) or evaluate it under its own rounding. */
static inline __attribute__((always_inline)) __m128 operand4(__m128 value)
{
    __m128i bits = _mm_castps_si128(value), magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    __m128i large = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f7fffff));
    __m128 result = _mm_castsi128_ps(_mm_xor_si128(bits, _mm_and_si128(large, _mm_xor_si128(magnitude, _mm_set1_epi32(0x7f7fffff)))));
#ifdef __wasm__
    __asm__("" : "+r"(result));  /* wasm: "r" holds a v128 too (finding 075) */
#else
    __asm__("" : "+x"(result));
#endif
    return result;
}

static __m128 minmax4(__m128 a, __m128 b, int maximum)
{
    __m128i x = _mm_castps_si128(a), y = _mm_castps_si128(b);
    __m128i both_negative = _mm_srai_epi32(_mm_and_si128(x, y), 31);
    __m128i take = _mm_or_si128(_mm_and_si128(both_negative, _mm_cmplt_epi32(x, y)),
                               _mm_andnot_si128(both_negative, _mm_cmpgt_epi32(x, y)));
    if (!maximum) take = _mm_xor_si128(take, _mm_set1_epi32(-1));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(take, x), _mm_andnot_si128(take, y)));
}

/* Scalar VU0 lanes share the EE FPU and VU0 macro ordering (ps2float.h). */
static float minmax_bits(float a, float b, int maximum) { return ps2_float_minmax(a, b, maximum); }

static void stall_for(Vu *u, int r)
{
    if (!u->unit && r && u->vf_ready[r] > u->cycle)
        u->cycle = u->vf_ready[r];
}

static void retire_flags(Vu *u)
{
    /* Issued flag results are ordered by cycle. Retire the ready prefix;
       do not copy all outstanding pipeline entries on every instruction. */
    while (u->n_flags && u->flags[u->flag_head].at <= u->flag_cycle) {
        unsigned i = (unsigned)u->flag_head;
        if (u->flags[i].kind == 1) u->mac = u->flags[i].mac;
        u->status = (u->status & ~u->flags[i].mask) | u->flags[i].status;
        if (u->flags[i].kind != 1) u->clip = u->flags[i].clip;
        u->flag_head = (u->flag_head + 1) & 15;
        u->n_flags--;
    }
}

/* Runs before every pair: flags read in this pair are those landed by its issue cycle. They
   are retired only when something reads or replaces them (flags_now): the Q writes to the
   status flag (bits 4, 5, 10 and 11) commute with the queued ones (bits 0-3 and 6-9). */
static inline __attribute__((always_inline)) void apply_flags(Vu *u)
{
    u->flag_cycle = u->cycle;
}

static inline __attribute__((always_inline)) void flags_now(Vu *u)
{
    if (u->n_flags && u->flags[u->flag_head].at <= u->flag_cycle) retire_flags(u);
}

/* Queues the flags an instruction issued now produces: a new MAC flag (and the status bits it
   implies), or a clip judgement shifted in. Each entry holds the whole flag state after it. */
static inline __attribute__((always_inline)) void queue_flags(Vu *u, const Upper *up)
{
    uint32_t mac, clip, st;
    if (u->n_flags == 16)
        flags_now(u);
    if (u->n_flags == 16)
        return;
    mac = u->n_flags ? u->flags[(u->flag_head + u->n_flags - 1) & 15].mac : u->mac;
    clip = u->clip_next;
    st = 0;
    if (up->flags == 1) {
        mac = up->mac;
        st = (mac & 0xF ? 1 : 0) | (mac & 0xF0 ? 2 : 0) | (mac & 0xF00 ? 4 : 0) | (mac & 0xF000 ? 8 : 0);
        st |= st << 6;  /* sticky */
    } else {
        clip = up->flags == 3 ? up->clip_bits : ((clip << 6) | up->clip_bits) & 0xFFFFFF;
        u->clip_next = clip;
    }
    unsigned tail = (unsigned)(u->flag_head + u->n_flags) & 15;
    u->flags[tail].at = u->cycle + FLAG_LATENCY;
    u->flags[tail].mac = mac;
    u->flags[tail].status = st;
    u->flags[tail].mask = up->flags == 1 ? 0xF : 0;  /* Z S U O replaced, sticky bits only set */
    u->flags[tail].clip = clip;
    u->flags[tail].kind = up->flags;
    u->n_flags++;
}

/* Latest flag values once every queued one lands (the program's end). */
static void drain_flags(Vu *u)
{
    u->cycle += 64;
    apply_flags(u);
    flags_now(u);
}

/* The MAC flags of an FMAC result written to the fields in d. Every caller passes the result of
   an operation on clamped operands under the VU MXCSR, which is finite and normal or zero
   (operand4), so the underflow (nonzero below FLT_MIN) and overflow (exponent 255) bits are 0. */
static inline __attribute__((always_inline)) void fmac_flags4(Upper *out, __m128 v, int d)
{
    {
        static const unsigned reverse[16] = {0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
        __m128 zero = _mm_cmpeq_ps(v, _mm_setzero_ps());
        out->mac = (reverse[_mm_movemask_ps(zero)] & d) |
                   (reverse[_mm_movemask_ps(v)] & d) << 4;
        out->flags = 1;
        _mm_storeu_ps(out->v, v);  /* an operation on operand4 values needs no result clamp */
    }
}

static inline __attribute__((always_inline)) void fmac_flags(Vu *u, Upper *out, const float r[4], int d)
{
    if (u->unit) {  /* OPMULA/OPMSUB: scalar operands, so keep the result clamp */
        fmac_flags4(out, _mm_loadu_ps(r), d);
        _mm_storeu_ps(out->v, arithmetic4(_mm_loadu_ps(r)));
        return;
    }
    uint32_t mac = 0;
    int comp;
    for (comp = 0; comp < 4; comp++) {
        int bit = 3 - comp;
        if (!HAS(d, comp))
            continue;
        if (r[comp] == 0.0f)
            mac |= 1u << bit;
        uint32_t result_bits; memcpy(&result_bits, &r[comp], 4);
        if (result_bits >> 31)
            mac |= 1u << (bit + 4);
        if (r[comp] != 0.0f && fabsf(r[comp]) < FLT_MIN)
            mac |= 1u << (bit + 8);
        if (u->unit) {
            uint32_t bits; memcpy(&bits, &r[comp], 4);
            if ((bits & 0x7f800000) == 0x7f800000) mac |= 1u << (bit + 12);
            out->v[comp] = arithmetic(u, r[comp]);
        }
        if (isinf(r[comp]))
            mac |= 1u << (bit + 12);
    }
    out->flags = 1;
    out->mac = mac;
}

static inline __attribute__((always_inline)) void write_vf(Vu *u, int r, int d, const float v[4], uint32_t ready)
{
    if (!r)
        return;
    store_fields(u->c->vf[r].f, _mm_loadu_si128((const __m128i *)v), d);
    store_fields(u->component_ready[r], _mm_set1_epi32((int)ready), d);
    if (ready > u->vf_ready[r])
        u->vf_ready[r] = ready;
}

static inline __attribute__((always_inline)) void write_vf_bits(Vu *u, int r, int d, const uint32_t v[4], uint32_t ready)
{
    if (!r)
        return;
    store_fields(u->c->vf[r].w, _mm_loadu_si128((const __m128i *)v), d);
    store_fields(u->component_ready[r], _mm_set1_epi32((int)ready), d);
    if (ready > u->vf_ready[r])
        u->vf_ready[r] = ready;
}

static void set_vi(Vu *u, int r, uint32_t v)
{
    if (r & 15) u->c->vi[r & 15] = (uint16_t)v;
}

static void set_ialu(Vu *u, int r, uint32_t v)
{
    if (r & 15) {
        if (u->vi_previous_reg != (unsigned)(r & 15) || u->vi_written_at + 1 != u->cycle)
            u->vi_previous_value = u->c->vi[r & 15];
        u->vi_previous_reg = r & 15;
        u->vi_written_at = u->cycle;
        u->c->vi[r & 15] = (uint16_t)v;
    }
}

/* The VU1 register file as VU0 micro code addresses it (PCSX2 GET_VU_MEM): VF0..31, then
   VI0..31 one per quadword. VU1 must be idle; the graphics fence waits for it. The reference
   build (test_vu1_pcsx2.py --aot) links next to the native one and shares these. */
#ifndef PS2_VU_REFERENCE
ps2_cpu *ps2_vu1_regs;
uint64_t ps2_vu1_code_version;
int ps2_vu1_native_mode = -1;
int ps2_vu0_aot_mode = -1;
int ps2_vu0_native_mode = -1;
unsigned long long ps2_vu0_native_runs;
static unsigned long long vu0_native_entries[5];
#ifdef PS2_VU_COVERAGE  /* tests: VU1 pairs run by the interpreter, by pc, and pairs run in blocks */
unsigned long long ps2_vu1_interpreted[2048], ps2_vu1_translated;
#endif  /* tests: 0/1 force the VU0 blocks off/on, else DHD_VU0_AOT */
unsigned long long ps2_vu1_native_runs[PS2_VU1_NATIVE_KINDS];
void (*ps2_vu1_regs_sync)(void);

void ps2_vu0_vu1_file(ps2_q128 file[64])
{
    memset(file, 0, 64 * sizeof *file);
    const ps2_cpu *v = ps2_vu1_regs;
    if (!v) return;
    if (ps2_vu1_regs_sync) ps2_vu1_regs_sync();
    memcpy(file, v->vf, sizeof v->vf);
    file[0].f[0] = file[0].f[1] = file[0].f[2] = 0; file[0].f[3] = 1;
    for (unsigned i = 0; i < 16; i++) file[32 + i].w[0] = v->vi[i];
    for (unsigned i = 0; i < 16; i++) file[48 + i].w[0] = v->vu0_ctrl[i];
    file[52].w[0] = v->r; memcpy(&file[53].w[0], &v->i, 4);
    memcpy(&file[54].w[0], &v->q, 4); memcpy(&file[55].w[0], &v->p, 4);
}
#endif

static ps2_q128 *mem(Vu *u, uint32_t qword)
{
    if (!u->unit && (qword & 0x400)) {
        if (!u->vu1_used) { ps2_vu0_vu1_file(u->vu1_file); u->vu1_used = 1; }
        return &u->vu1_file[qword & 63];
    }
    return &u->c->vu0_mem[qword & u->data_mask];
}

static inline __attribute__((always_inline)) int upper(Vu *u, uint32_t w, Upper *out)
{
    int op = w & 0x3F, d = DEST(w), ft = FT(w), fs = FS(w), fd = FD(w), comp;
    float s[4], t[4], r[4], q = u->c->q, i = u->c->i;
    int kind, sets_flags = 1, from = 0;  /* t: 0 field bc of ft, 1 Q, 2 I, 3 ft */
    enum { ADD, SUB, MADD, MSUB, MAX, MINI, MUL };
    out->flags = out->to_acc = out->raw = 0;
    out->reg = -1;
    if (op >= 0x3C) {
        int sub = (fd << 2) | (op & 3);
        int bc = op & 3;
        stall_for(u, fs);
        if (sub == 0x2F)  /* NOP */
            return 0;
        for (comp = 0; comp < 4; comp++)
            s[comp] = vf(u, fs, comp);
        switch (sub) {
        case 0x10: case 0x11: case 0x12: case 0x13: {  /* ITOF0/4/12/15 */
            static const float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            for (comp = 0; comp < 4; comp++)
                out->v[comp] = (float)(int32_t)vf_bits(u, fs, comp) / scale[bc];
            out->reg = ft, out->dest = d;
            return 0;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {  /* FTOI0/4/12/15 */
            static const int shift[4] = {0, 4, 12, 15};
            for (comp = 0; comp < 4; comp++) {
                if (u->unit) {
                    float x = s[comp] * (float)(1 << shift[bc]);
                    uint32_t bits; memcpy(&bits, &x, 4);
                    out->bits[comp] = (bits & 0x7f800000) >= 0x4f000000
                        ? (bits >> 31 ? 0x80000000 : 0x7fffffff) : (uint32_t)(int32_t)x;
                } else {
                    double x = (double)s[comp] * (double)(1 << shift[bc]);
                    int32_t v = x >= 2147483647.0 ? 2147483647 : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
                    out->bits[comp] = (uint32_t)v;
                }
            }
            out->reg = ft, out->dest = d, out->raw = 1;
            return 0;
        }
        case 0x1D:  /* ABS */
            for (comp = 0; comp < 4; comp++)
                out->v[comp] = fabsf(s[comp]);
            out->reg = ft, out->dest = d;
            return 0;
        case 0x1F: {  /* CLIP fs.xyz, ft.w */
            float lim;
            uint32_t bits = 0;
            stall_for(u, ft);
            lim = fabsf(vf(u, ft, 3));
            for (comp = 0; comp < 3; comp++) {
                if (u->unit) {
                    uint32_t bound = vf_bits(u, ft, 3), value = vf_bits(u, fs, comp);
                    bound = bound & 0x7f800000 ? bound & 0x7fffffff : 0x007fffff;
                    if ((int32_t)value > (int32_t)bound) bits |= 1u << (comp * 2);
                    if ((int32_t)(value ^ 0x80000000) > (int32_t)bound) bits |= 2u << (comp * 2);
                } else {
                    if (s[comp] > lim) bits |= 1u << (comp * 2);
                    if (s[comp] < -lim) bits |= 2u << (comp * 2);
                }
            }
            out->flags = 2;
            out->clip_bits = bits;
            return 0;
        }
        }
        /* the accumulator forms */
        if (sub < 0x10 || (sub >= 0x18 && sub < 0x1C)) {
            kind = sub < 0x10 ? (sub >> 2) : MUL;
            from = 0;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, bc);
        } else if (sub == 0x1C || sub == 0x20 || sub == 0x21 || sub == 0x24 || sub == 0x25) {
            kind = sub == 0x1C ? MUL : sub == 0x20 ? ADD : sub == 0x21 ? MADD : sub == 0x24 ? SUB : MSUB;
            from = 1;
            for (comp = 0; comp < 4; comp++)
                t[comp] = q;
        } else if (sub == 0x1E || sub == 0x22 || sub == 0x23 || sub == 0x26 || sub == 0x27) {
            kind = sub == 0x1E ? MUL : sub == 0x22 ? ADD : sub == 0x23 ? MADD : sub == 0x26 ? SUB : MSUB;
            from = 2;
            for (comp = 0; comp < 4; comp++)
                t[comp] = i;
        } else if (sub == 0x28 || sub == 0x29 || sub == 0x2A || sub == 0x2C || sub == 0x2D) {
            kind = sub == 0x28 ? ADD : sub == 0x29 ? MADD : sub == 0x2A ? MUL : sub == 0x2C ? SUB : MSUB;
            from = 3;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, comp);
        } else if (sub == 0x2E) {  /* OPMULA ACC.xyz = fs.yzx * ft.zxy */
            stall_for(u, ft);
            r[0] = arithmetic(u, s[1]) * arithmetic_vf(u, ft, 2);
            r[1] = arithmetic(u, s[2]) * arithmetic_vf(u, ft, 0);
            r[2] = arithmetic(u, s[0]) * arithmetic_vf(u, ft, 1);
            r[3] = 0;
            out->to_acc = 1, out->dest = d & 0xE;
            memcpy(out->v, r, sizeof r);
            fmac_flags(u, out, r, d & 0xE);
            return 0;
        } else
            return -1;
        out->to_acc = 1;
    } else {
        int bc = op & 3;
        if (op < 0x1C) {
            kind = op >> 2;
            from = 0;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, bc);
        } else if (op == 0x1C || op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25) {
            kind = op == 0x1C ? MUL : op == 0x20 ? ADD : op == 0x21 ? MADD : op == 0x24 ? SUB : MSUB;
            from = 1;
            for (comp = 0; comp < 4; comp++)
                t[comp] = q;
        } else if (op == 0x1D || op == 0x1E || op == 0x1F || (op >= 0x22 && op <= 0x27 && op != 0x24 && op != 0x25)) {
            kind = op == 0x1D ? MAX : op == 0x1E ? MUL : op == 0x1F ? MINI : op == 0x22 ? ADD : op == 0x23 ? MADD
                 : op == 0x26 ? SUB : MSUB;
            from = 2;
            for (comp = 0; comp < 4; comp++)
                t[comp] = i;
        } else if (op >= 0x28 && op <= 0x2F && op != 0x2E) {
            static const int k[8] = {ADD, MADD, MUL, MAX, SUB, MSUB, 0, MINI};
            kind = k[op - 0x28];
            from = 3;
            stall_for(u, ft);
            for (comp = 0; comp < 4; comp++)
                t[comp] = vf(u, ft, comp);
        } else if (op == 0x2E) {  /* OPMSUB fd.xyz = ACC.xyz - fs.yzx * ft.zxy */
            stall_for(u, fs);
            stall_for(u, ft);
            r[0] = arithmetic(u, u->c->vacc[0]) - arithmetic_vf(u, fs, 1) * arithmetic_vf(u, ft, 2);
            r[1] = arithmetic(u, u->c->vacc[1]) - arithmetic_vf(u, fs, 2) * arithmetic_vf(u, ft, 0);
            r[2] = arithmetic(u, u->c->vacc[2]) - arithmetic_vf(u, fs, 0) * arithmetic_vf(u, ft, 1);
            r[3] = 0;
            out->reg = fd, out->dest = d & 0xE;
            memcpy(out->v, r, sizeof r);
            fmac_flags(u, out, r, d & 0xE);
            return 0;
        } else
            return -1;
        out->reg = fd;
    }
    stall_for(u, fs);
    if (u->unit) {
        __m128 a = vf4(u, fs), b, result;
        int bc = w & 3;
        b = from == 0 ? lane4(vf4(u, ft), bc) : from == 1 ? _mm_set1_ps(q) : from == 2 ? _mm_set1_ps(i) : vf4(u, ft);
        if (kind != MAX && kind != MINI) { a = operand4(a); b = operand4(b); }
        switch (kind) {
        case ADD: result = _mm_add_ps(a,b); break;
        case SUB: result = _mm_sub_ps(a,b); break;
        case MUL: result = _mm_mul_ps(a,b); break;
        case MAX: result = minmax4(a,b,1); sets_flags = 0; break;
        case MINI: result = minmax4(a,b,0); sets_flags = 0; break;
        case MADD: result = _mm_add_ps(operand4(_mm_loadu_ps(u->c->vacc)), _mm_mul_ps(a,b)); break;
        default: result = _mm_sub_ps(operand4(_mm_loadu_ps(u->c->vacc)), _mm_mul_ps(a,b)); break;
        }
        out->dest = d;
        if (sets_flags)
            fmac_flags4(out, result, d);
        else
            _mm_storeu_ps(out->v, result);
        return 0;
    }
    for (comp = 0; comp < 4; comp++)
        s[comp] = vf(u, fs, comp);
    for (comp = 0; comp < 4; comp++) {
        float prod;
        if (kind != MAX && kind != MINI) {
            s[comp] = arithmetic(u, s[comp]); t[comp] = arithmetic(u, t[comp]);
        }
        switch (kind) {
        case ADD: r[comp] = s[comp] + t[comp]; break;
        case SUB: r[comp] = s[comp] - t[comp]; break;
        case MUL: r[comp] = s[comp] * t[comp]; break;
        case MAX: r[comp] = minmax_bits(s[comp], t[comp], 1); sets_flags = 0; break;
        case MINI: r[comp] = minmax_bits(s[comp], t[comp], 0); sets_flags = 0; break;
        case MADD: prod = s[comp] * t[comp]; r[comp] = arithmetic(u, u->c->vacc[comp]) + prod; break;
        default: prod = s[comp] * t[comp]; r[comp] = arithmetic(u, u->c->vacc[comp]) - prod; break;
        }
    }
    out->dest = d;
    memcpy(out->v, r, sizeof r);
    if (sets_flags)
        fmac_flags(u, out, r, d);
    return 0;
}

static inline __attribute__((always_inline)) void commit_upper(Vu *u, const Upper *up, int lower_reg)
{
    int comp;
    if (up->flags)
        queue_flags(u, up);
    if (up->to_acc) {
        store_fields(u->c->vacc, _mm_loadu_si128((const __m128i *)up->v), up->dest);
        return;
    }
    (void)lower_reg;  /* the upper write wins over a lower one to the same register */
    if (up->reg < 0)
        return;
    if (up->raw)
        write_vf_bits(u, up->reg, up->dest, up->bits, u->cycle + FMAC_LATENCY);
    else
        write_vf(u, up->reg, up->dest, up->v, u->cycle + FMAC_LATENCY);
}

static void start_q(Vu *u, float value, int latency, uint32_t flags)
{
    if (u->q_ready > u->cycle) {  /* the divider is busy: wait for it */
        u->cycle = u->q_ready;
        u->c->q = u->q_next;
    }
    u->q_next = arithmetic(u, value);
    u->q_ready = u->cycle + latency;
    if (u->unit) u->q_flags = flags;
    else u->status = (u->status & ~0x30u) | flags | (flags << 6);
}

static inline __attribute__((always_inline)) void update_q(Vu *u)
{
    if (u->q_ready && u->q_ready <= u->cycle) {
        u->c->q = u->q_next;
        if (u->unit) u->status = (u->status & ~0x30u) | u->q_flags;
        u->q_ready = 0;
    }
}

static float vu_max(int negative) { return negative ? -FLT_MAX : FLT_MAX; }

/* Executes the lower instruction; returns the branch target (instruction index) or -1, and
   *bad if the instruction isn't one VU0 has. */
static inline __attribute__((always_inline)) int lower(Vu *u, uint32_t w, unsigned pc, int *bad)
{
    ps2_cpu *c = u->c;
    int op = w >> 25, it = FT(w), is = FS(w), d = DEST(w), comp;
    int32_t imm11 = sext(w & 0x7FF, 11);
    uint16_t vit = c->vi[it & 15], vis = c->vi[is & 15];
    int target = (int)pc + 1 + imm11;
    if (op == 0x40) {
        int sub = w & 0x3F;
        if (sub < 0x3C) {
            int id = FD(w);
            switch (sub) {
            case 0x30: set_ialu(u, id, vis + vit); return -1;
            case 0x31: set_ialu(u, id, vis - vit); return -1;
            case 0x32: set_ialu(u, it, vis + sext((uint32_t)id, 5)); return -1;
            case 0x34: set_ialu(u, id, vis & vit); return -1;
            case 0x35: set_ialu(u, id, vis | vit); return -1;
            }
            *bad = 1;
            return -1;
        }
        switch ((FD(w) << 2) | (sub & 3)) {
        case 0x30: case 0x31: {  /* MOVE, MR32 ft, fs */
            uint32_t v[4];
            stall_for(u, is);
            __m128 x = vf4(u, is);
            _mm_storeu_ps((float *)v, (sub & 3) ? _mm_shuffle_ps(x, x, 0x39) : x);
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x34: case 0x36: {  /* LQI ft, (is++) / LQD ft, (--is) */
            ps2_q128 *m;
            if ((sub & 3) == 2)
                set_ialu(u, is, --vis);
            m = mem(u, vis);
            write_vf_bits(u, it, d, m->w, u->cycle + FMAC_LATENCY);
            if ((sub & 3) == 0)
                set_ialu(u, is, vis + 1);
            return -1;
        }
        case 0x35: case 0x37: {  /* SQI fs, (it++) / SQD fs, (--it) */
            ps2_q128 *m;
            stall_for(u, is);
            if ((sub & 3) == 3)
                set_ialu(u, it, --vit);
            m = mem(u, vit);
            store_fields(m->w, _mm_castps_si128(vf4(u, is)), d);
            if ((sub & 3) == 1)
                set_ialu(u, it, vit + 1);
            return -1;
        }
        case 0x38: case 0x3A: {  /* DIV / RSQRT Q, fs.fsf, ft.ftf */
            float s, t;
            uint32_t fl = 0;
            stall_for(u, is);
            stall_for(u, it);
            s = arithmetic_vf(u, is, w >> 21 & 3);
            t = arithmetic_vf(u, it, w >> 23 & 3);
            if (u->unit && (sub & 3) == 2 && t == 0.0f) {
                uint32_t bits = (vf_bits(u, is, w >> 21 & 3) ^ vf_bits(u, it, w >> 23 & 3)) & 0x80000000;
                if (s != 0.0f) bits |= 0x7f7fffff;
                float result; memcpy(&result, &bits, 4);
                start_q(u, result, RSQRT_LATENCY, s == 0.0f ? 0x30 : 0x20);
                return -1;
            }
            if ((sub & 3) == 2) {
                if (t < 0.0f)
                    fl |= 0x10;
                t = sqrtf(fabsf(t));
            }
            if (t == 0.0f) {
                fl |= s == 0.0f && (sub & 3) == 0 ? 0x10 : 0x20;
                start_q(u, vu_max(signbit(s) != signbit(t)), (sub & 3) ? RSQRT_LATENCY : DIV_LATENCY, fl);
            } else
                start_q(u, s / t, (sub & 3) ? RSQRT_LATENCY : DIV_LATENCY, fl);
            return -1;
        }
        case 0x39: {  /* SQRT Q, ft.ftf */
            float t;
            stall_for(u, it);
            t = arithmetic_vf(u, it, w >> 23 & 3);
            start_q(u, sqrtf(fabsf(t)), DIV_LATENCY, t < 0.0f ? 0x10 : 0);
            return -1;
        }
        case 0x3B:  /* WAITQ */
            if (u->q_ready > u->cycle)
                u->cycle = u->q_ready;
            update_q(u);
            return -1;
        case 0x3C:  /* MTIR it, fs.fsf */
            stall_for(u, is);
            set_ialu(u, it, vf_bits(u, is, w >> 21 & 3));
            return -1;
        case 0x3D: {  /* MFIR ft, is */
            uint32_t v[4];
            for (comp = 0; comp < 4; comp++)
                v[comp] = (uint32_t)(int32_t)(int16_t)vis;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x3E:  /* ILWR it, (is) */
            for (comp = 0; comp < 4; comp++)
                if (HAS(d, comp))
                    set_vi(u, it, mem(u, vis)->w[comp]);
            return -1;
        case 0x3F:  /* ISWR it, (is) */
            for (comp = 0; comp < 4; comp++)
                if (HAS(d, comp))
                    mem(u, vis)->w[comp] = vit;
            return -1;
        case 0x40: case 0x41: {  /* RNEXT / RGET ft */
            uint32_t v[4];
            if (!(sub & 3)) {
                uint32_t x = c->r >> 4 & 1, y = c->r >> 22 & 1;
                c->r = (((c->r << 1) ^ x ^ y) & 0x7FFFFFu) | 0x3F800000u;
            }
            for (comp = 0; comp < 4; comp++)
                v[comp] = c->r;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x64: {  /* MFP ft, P (the old P remains readable while EFU runs) */
            if (!u->unit) break;
            uint32_t v[4], p;
            if (u->p_ready && u->p_ready <= u->cycle) {
                c->p = u->p_next; u->p_ready = 0;
            }
            memcpy(&p, &c->p, 4);
            for (comp = 0; comp < 4; comp++) v[comp] = p;
            write_vf_bits(u, it, d, v, u->cycle + FMAC_LATENCY);
            return -1;
        }
        case 0x68: case 0x69:  /* XTOP / XITOP */
            if (!u->unit) break;
            set_vi(u, it, (sub & 3) ? u->itop : u->top);
            return -1;
        case 0x6C:  /* XGKICK */
            if (!u->unit) break;
#ifndef PS2_VU_REFERENCE
            if (kick_trace) { flags_now(u); kick_trace(pc * 8, u->clip, vis & 1023); }
#endif
            if (u->kick && u->kick(u->opaque, vis & 1023)) *bad = 1;
            return -1;
        case 0x7B:  /* WAITP */
            if (!u->unit) break;
            if (u->p_ready > u->cycle) u->cycle = u->p_ready;
            if (u->p_ready) { c->p = u->p_next; u->p_ready = 0; }
            return -1;
        case 0x70: case 0x71: case 0x72: case 0x73:  /* ESADD/ERSADD/ELENG/ERLENG */
        case 0x76: case 0x78: case 0x79: case 0x7A: { /* ESUM/ESQRT/ERSQRT/ERCPR */
            if (!u->unit) break;
            unsigned efu = (FD(w) << 2) | (sub & 3), latency = 12;
            float p, x, y, z;
            stall_for(u, is);
            if (u->p_ready) {
                if (u->p_ready > u->cycle) u->cycle = u->p_ready;
                c->p = u->p_next;
            }
            x = vf(u, is, 0); y = vf(u, is, 1); z = vf(u, is, 2);
            if (efu <= 0x73) {
                float xx = x * x, yy = y * y, zz = z * z;
                p = (xx + yy) + zz;
                latency = efu == 0x70 ? 11 : efu == 0x73 ? 24 : 18;
                if (efu == 0x72 || efu == 0x73) p = sqrtf(p);
                if ((efu == 0x71 || efu == 0x73) && p != 0) p = 1.0f / p;
            } else if (efu == 0x76)
                p = ((x + y) + z) + vf(u, is, 3);
            else {
                p = vf(u, is, w >> 21 & 3);
                if ((efu == 0x78 || efu == 0x79) && p >= 0) p = sqrtf(p);
                if ((efu == 0x79 || efu == 0x7A) && p != 0) p = 1.0f / p;
                if (efu == 0x79) latency = 18;
            }
            u->p_next = p; u->p_ready = u->cycle + latency;
            return -1;
        }
        case 0x42:  /* RINIT R, fs.fsf */
            stall_for(u, is);
            c->r = 0x3F800000u | (vf_bits(u, is, w >> 21 & 3) & 0x7FFFFFu);
            return -1;
        case 0x43:  /* RXOR R, fs.fsf */
            stall_for(u, is);
            c->r = 0x3F800000u | ((c->r ^ vf_bits(u, is, w >> 21 & 3)) & 0x7FFFFFu);
            return -1;
        }
        *bad = 1;
        return -1;
    }
    /* Integer branches have a one-cycle bypass delay, unlike other VI readers. */
    if (u->unit && op >= 0x28 && op <= 0x2F && u->vi_previous_reg && u->vi_written_at + 1 == u->cycle) {
        if ((unsigned)(is & 15) == u->vi_previous_reg) vis = u->vi_previous_value;
        if ((unsigned)(it & 15) == u->vi_previous_reg) vit = u->vi_previous_value;
    }
    switch (op) {
    case 0x00: {  /* LQ ft, imm(is) */
        write_vf_bits(u, it, d, mem(u, (uint32_t)(vis + imm11))->w, u->cycle + FMAC_LATENCY);
        return -1;
    }
    case 0x01: {  /* SQ fs, imm(it) */
        ps2_q128 *m;
        stall_for(u, is);
        m = mem(u, (uint32_t)(vit + imm11));
        store_fields(m->w, _mm_castps_si128(vf4(u, is)), d);
        return -1;
    }
    case 0x04:  /* ILW it, imm(is) */
        for (comp = 0; comp < 4; comp++)
            if (HAS(d, comp))
                set_vi(u, it, mem(u, (uint32_t)(vis + imm11))->w[comp]);
        return -1;
    case 0x05:  /* ISW it, imm(is) */
        for (comp = 0; comp < 4; comp++)
            if (HAS(d, comp))
                mem(u, (uint32_t)(vis + imm11))->w[comp] = vit;
        return -1;
    case 0x08: case 0x09: {  /* IADDIU / ISUBIU it, is, imm15 */
        uint32_t imm15 = (w >> 10 & 0x7800) | (w & 0x7FF);
        set_ialu(u, it, op == 0x08 ? vis + imm15 : vis - imm15);
        return -1;
    }
    case 0x10: flags_now(u); set_vi(u, 1, (u->clip & 0xFFFFFF) == (w & 0xFFFFFF)); return -1;          /* FCEQ */
    case 0x11: { /* FCSET uses the same four-cycle flag pipeline as CLIP. */
        Upper flag = {0}; flag.flags = 3; flag.clip_bits = w & 0xFFFFFF;
        queue_flags(u, &flag); return -1;
    }
    case 0x12: flags_now(u); set_vi(u, 1, (u->clip & w & 0xFFFFFF) != 0); return -1;                   /* FCAND */
    case 0x13: flags_now(u); set_vi(u, 1, ((u->clip | w) & 0xFFFFFF) == 0xFFFFFF); return -1;         /* FCOR */
    case 0x1C: flags_now(u); set_vi(u, it, u->clip & 0xFFF); return -1;                                 /* FCGET */
    case 0x14: case 0x15: case 0x16: case 0x17: {  /* FSEQ / FSSET / FSAND / FSOR */
        uint32_t imm12 = (w >> 10 & 0x800) | (w & 0x7FF);
        flags_now(u);
        if (op == 0x14)
            set_vi(u, it, (u->status & 0xFFF) == imm12);
        else if (op == 0x15)
            u->status = (u->status & 0x3F) | (imm12 & 0xFC0);
        else if (op == 0x16)
            set_vi(u, it, u->status & imm12);
        else
            set_vi(u, it, (u->status | imm12) & 0xFFF);
        return -1;
    }
    case 0x18: flags_now(u); set_vi(u, it, (u->mac & 0xFFFF) == vis); return -1;  /* FMEQ */
    case 0x1A: flags_now(u); set_vi(u, it, u->mac & vis); return -1;              /* FMAND */
    case 0x1B: flags_now(u); set_vi(u, it, u->mac | vis); return -1;              /* FMOR */
    case 0x20: return target;                                      /* B */
    case 0x21: set_vi(u, it, pc + 2); return target;              /* BAL */
    case 0x24: return vis;                                         /* JR */
    case 0x25: set_vi(u, it, pc + 2); return vis;                 /* JALR */
    case 0x28: return vit == vis ? target : -1;                    /* IBEQ */
    case 0x29: return vit != vis ? target : -1;                    /* IBNE */
    case 0x2C: return (int16_t)vis < 0 ? target : -1;              /* IBLTZ */
    case 0x2D: return (int16_t)vis > 0 ? target : -1;              /* IBGTZ */
    case 0x2E: return (int16_t)vis <= 0 ? target : -1;             /* IBLEZ */
    case 0x2F: return (int16_t)vis >= 0 ? target : -1;             /* IBGEZ */
    }
    *bad = 1;
    return -1;
}

/* Both halves issue together, after all source and divider stalls. Disjoint
   fields of a VF register do not stall each other. */
static inline __attribute__((always_inline)) void read_fields(Vu *u, unsigned reg, unsigned fields)
{
    if (!reg) return;
    for (unsigned c = 0; c < 4; c++)
        if (HAS(fields, c) && u->component_ready[reg][c] > u->cycle)
            u->cycle = u->component_ready[reg][c];
}

static inline __attribute__((always_inline)) void pair_stalls(Vu *u, uint32_t hi, uint32_t lo)
{
    unsigned op = hi & 63, sub = FD(hi) * 4 + (op & 3), d = DEST(hi);
    if (!(op >= 60 && sub == 0x2f)) {
        if (op >= 60 && sub == 0x1f) {
            read_fields(u, FS(hi), 14); read_fields(u, FT(hi), 1);
        } else {
            read_fields(u, FS(hi), d);
            if (op < 28) read_fields(u, FT(hi), 8 >> (op & 3));
            else if (op >= 40 && op < 48) read_fields(u, FT(hi), d);
            else if (op >= 60 && (sub < 16 || (sub >= 24 && sub < 28)))
                read_fields(u, FT(hi), 8 >> (op & 3));
            else if (op >= 60 && sub >= 40 && sub <= 46) read_fields(u, FT(hi), d);
        }
    }
    if (hi >> 31) return;
    op = lo >> 25; d = DEST(lo);
    if (op == 1) read_fields(u, FS(lo), d);
    if (op != 0x40 || (lo & 63) < 60) return;
    sub = FD(lo) * 4 + (lo & 3);
    switch (sub) {
    case 0x30: case 0x35: case 0x37: read_fields(u, FS(lo), d); break;
    case 0x31: read_fields(u, FS(lo), (d >> 1) | ((d & 1) << 3)); break;
    case 0x38: case 0x3a:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3));
        /* fall through */
    case 0x39:
        read_fields(u, FT(lo), 8 >> (lo >> 23 & 3));
        /* fall through */
    case 0x3b:
        if (u->q_ready > u->cycle) u->cycle = u->q_ready;
        break;
    case 0x3c: case 0x42: case 0x43:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3)); break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x76:
        read_fields(u, FS(lo), sub == 0x76 ? 15 : 14);
        goto efu;
    case 0x78: case 0x79: case 0x7a:
        read_fields(u, FS(lo), 8 >> (lo >> 21 & 3));
        /* fall through */
    case 0x7b:
    efu:
        if (u->p_ready) {
            if (u->p_ready - 1 > u->cycle) u->cycle = u->p_ready - 1;
            u->c->p = u->p_next; u->p_ready = 0;
        }
        break;
    }
}

static inline void trace_pair(Vu *u, unsigned pc)
{
#ifdef PS2_VU_TRACE
    if (getenv("DHD_VU_TRACE")) {
        fprintf(stderr, "N %04X %llu", pc * 8, (unsigned long long)u->cycle);
        for (unsigned v = 1; v < 32; v++) for (unsigned c = 0; c < 4; c++) fprintf(stderr, " %08X", u->c->vf[v].w[c]);
        fprintf(stderr, "\n");
    }
#else
    (void)u; (void)pc;
#endif
}

/* Both callers select a constant unit. Specialize the shared interpreter so
   VU1 does not test the VU0 arithmetic/pipeline mode in every component op. */
#ifdef PS2_VU_AOT
/* The blocks run VU1 only. Stores through u->c can alias u, so restate the unit before each step:
   the helpers then drop their VU0 paths (scalar lanes, stall checks) instead of testing it. */
#define VU1(u) do { if ((u)->unit != 1) __builtin_unreachable(); } while (0)
/* The VU0 blocks (aot0_*) likewise state unit 0 for the helpers. */
#define VU0(u) do { if ((u)->unit != 0) __builtin_unreachable(); } while (0)
#include "vu1_aot.inc"
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
#include "vu1_native_clip.inc"
#include "vu1_native_geometry.h"
#include "vu1_native_extra.h"
#endif
#endif

#if defined(PS2_VU_AOT) && !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
/* The native kernels run only where micro memory holds the exact words they were written from.
   In the game's VU1 micro memory (0x11008000, changed only by VIF1 MPG, which bumps
   ps2_vu1_code_version) the answer is kept until the next change, as aot_find does; any other
   code buffer (the replay tests) is compared each time. */
static int native_code_compare(const uint8_t *code, unsigned kind, unsigned start, unsigned end)
{
    const uint32_t *words = kind == 2048 ? native_loop2e48_words : kind == 1024 ? native_loop0e10_words : kind == 512 ? native_loop2dc8_words : kind == 256 ? native_loop1430_words : kind == 128 ? native_loop468_words : kind == 64 ? native_loop800_words : kind == 16 ? native_loop20c_words :
                            kind == 32 ? native_loop260_words : kind == 4 ? native_loop1638_words :
                            kind == 8 ? native_loop17e0_words : native_loop_words;
    /* 1430's signature includes its short exit through 1630. Bulk flags have
       already retired: neither that tail nor the common GIF tail may gain an
       earlier MAC/status read without making this kernel decline. */
    size_t bytes = kind == 256 ? sizeof native_loop1430_words : end - start;
    if (memcmp(code + start, words, bytes)) return 0;
    if ((kind == 256 || kind == 512) && memcmp(code + 0xff8, native_giftail_words, sizeof native_giftail_words)) return 0;
    /* 2DC8 also carries a pending Q through its own branch and delay slot. */
    if (kind == 512 && memcmp(code + 0x2fc8, native_tail2dc8_words, sizeof native_tail2dc8_words)) return 0;
    if (kind == 1024 && memcmp(code + 0xfd0, native_tail0e10_words, sizeof native_tail0e10_words)) return 0;
    if (kind == 2048 && memcmp(code + 0x30c0, native_tail2e48_words, sizeof native_tail2e48_words)) return 0;
    if (kind == 64 || kind == 128 || kind == 1024 || kind == 2048) return 1;  /* no clipping path */
    if (kind <= 8 && (kind == 4 ? memcmp(code + 0x17c0, native_zero1638_words, sizeof native_zero1638_words) :
                      kind == 8 ? memcmp(code + 0x1938, native_zero17e0_words, sizeof native_zero17e0_words) :
                      memcmp(code + 0x270, native_zero_words, sizeof native_zero_words)))
        return 0;
    return !memcmp(code + 0x1948, native_clip1_words, sizeof native_clip1_words) &&
           !memcmp(code + 0x1cc0, native_clip2_words, sizeof native_clip2_words);
}

static int native_code_matches(const uint8_t *code, unsigned kind, unsigned start, unsigned end)
{
    static uint64_t checked[4096];  /* ps2_vu1_code_version + 1 */
    static uint8_t matched[4096];
    if ((uintptr_t)code != 0x11008000) return native_code_compare(code, kind, start, end);
    if (checked[kind] != ps2_vu1_code_version + 1) {
        matched[kind] = (uint8_t)native_code_compare(code, kind, start, end);
        checked[kind] = ps2_vu1_code_version + 1;
    }
    return matched[kind];
}
#endif

void ps2_vu_aot_load(const void *vutext)
{
#if defined(PS2_VU_AOT) && !defined(PS2_VU_AOT_EMBED)
    aot_words_load(vutext);
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
    native_words_load(vutext);
#endif
#else
    (void)vutext;
#endif
}

static inline __attribute__((always_inline)) int run(ps2_cpu *cpu, unsigned addr, unsigned unit, unsigned top, unsigned itop,
               ps2_vu_kick_fn kick, void *opaque)
{
    Vu u;
    unsigned mask = unit ? 0x7FF : 0x1FF;
    unsigned pc = (addr / 8) & mask;
    int branch = -1, ending = 0;
    long steps;
    unsigned history[16] = {0}, dispatched = 0;  /* the last 16 pairs or blocks run */
#ifdef PS2_VU_AOT
    static int interpreter_only = -1, vu0_aot = -1;
    if (unit && interpreter_only < 0) interpreter_only = getenv("DHD_VU_INTERPRETER") != NULL;
    /* DHD_VU0_AOT=1: VU0 microprograms (vcallms) through the translated blocks. Off by default
       until port/runtime/vu0_aot_diff.c and a race gate have checked them against the interpreter. */
    if (!unit && vu0_aot < 0) {
        const char *value = getenv("DHD_VU0_AOT");
        vu0_aot = value && atoi(value) && !getenv("DHD_VU_INTERPRETER");
    }
#ifndef PS2_VU_REFERENCE
    if (!unit && ps2_vu0_aot_mode >= 0) vu0_aot = ps2_vu0_aot_mode;
#endif
    const int use_aot = unit ? !interpreter_only : vu0_aot;
#if !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
    static int native_enabled = -1;
    static unsigned native_mask;
    if (native_enabled < 0) {
        const char *value = getenv("DHD_VU1_NATIVE");
        native_enabled = (!value || atoi(value)) && !getenv("DHD_VU_TRACE");
        const char *mask = getenv("DHD_VU1_NATIVE_MASK");
        native_mask = mask ? (unsigned)strtoul(mask, NULL, 0) : 4095;
    }
    unsigned native_kind = 0;
    if (use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && (addr == 0x1040 || addr == 0x1cb0)) {
        if (cpu->vi[13] == 0x11) native_kind = addr == 0x1040 ? 1 : 2;
        else if (cpu->vi[13] == 0x2c9) native_kind = 4;
        else if (cpu->vi[13] == 0x2fe) native_kind = 8;
        else if (cpu->vi[13] == 0x20c) native_kind = 16;
        else if (cpu->vi[13] == 0x260) native_kind = 32;
        else if (cpu->vi[13] == 0x100) native_kind = 64;
        else if (cpu->vi[13] == 0x288) native_kind = 256;
        else if (cpu->vi[13] == 0x5bd) native_kind = 512;
    }
    /* MSCAL 0010/0020 branch to the skinning setup at 03C8 (overlay 2), then its loop. */
    if (use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && (addr == 0x10 || addr == 0x20))
        native_kind = 128;
    if (unit && use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && addr == 0xe10) native_kind = 1024;
    if (unit && use_aot && (ps2_vu1_native_mode >= 0 ? ps2_vu1_native_mode : native_enabled) && addr == 0x2e48) native_kind = 2048;
    const int native_geometry = (native_kind & native_mask) != 0;
    unsigned native_start = native_kind == 2048 ? 0x2f90 : native_kind == 1024 ? 0xee0 : native_kind == 512 ? 0x2f00 : native_kind == 256 ? 0x1510 : native_kind == 128 ? 0x468 : native_kind == 64 ? 0x8b0 : native_kind == 16 ? 0x1110 : native_kind == 32 ? 0x13a0 : native_kind == 4 ? 0x1708 : native_kind == 8 ? 0x18b0 : 0x170;
    unsigned native_end = native_kind == 2048 ? 0x30c0 : native_kind == 1024 ? 0xfd0 : native_kind == 512 ? 0x2fc8 : native_kind == 256 ? 0x1628 : native_kind == 128 ? 0x578 : native_kind == 64 ? 0x938 : native_kind == 16 ? 0x11d8 : native_kind == 32 ? 0x1420 : native_kind == 4 ? 0x17b0 : native_kind == 8 ? 0x1928 : 0x260;
    static unsigned long long native_target_calls;
    static unsigned long long native_key_targets[4096], native_key_calls[4096], native_key_vertices[4096], native_key_clipped[4096];
    if (native_geometry) native_target_calls++;
    if (native_geometry) native_key_targets[native_kind]++;
#endif
#endif
    memset(&u, 0, offsetof(Vu, vu1_file));  /* about 60k VU0 and 30k VU1 runs a second in a race */
    u.c = cpu;
    u.unit = unit; u.code_mask = mask; u.data_mask = unit ? 0x3FF : 0xFF;
    u.top = top; u.itop = itop; u.kick = kick; u.opaque = opaque;
    u.status = cpu->vu0_ctrl[0] & 0xFFF;
    u.mac = cpu->vu0_ctrl[1] & 0xFFFF;
    u.clip = u.clip_next = cpu->vu0_ctrl[2] & 0xFFFFFF;
    for (steps = 0; steps < MAX_STEPS; steps++) {
#ifdef PS2_VU_GEOMETRY_BENCH
        /* Test-only capture of a pipeline boundary, absent from game builds. */
        extern int vu_geometry_capture(void *, unsigned);
        if (vu_geometry_capture(&u, pc)) return 0;
#endif
#if defined(PS2_VU_AOT) && !defined(PS2_VU_GEOMETRY_BENCH) && !defined(PS2_VU_REFERENCE)
        unsigned long long before_vertices = native_vertices, before_clipped = native_clipped_vertices;
        unsigned native_count = 0;
        if (native_geometry && pc == native_start / 8 && branch < 0 && !ending &&
            steps < MAX_STEPS - (native_kind >= 256 ? 262144 : 16384) &&
            native_code_matches(cpu->vu0_code, native_kind, native_start, native_end) &&
            ((native_count = native_kind == 2048 ? native_particles_2e48(&u) : native_kind == 1024 ? native_particles_0e10(&u) : native_kind == 512 ? native_geometry_2dc8(&u) : native_kind == 256 ? native_geometry_1430(&u) : native_kind == 128 ? native_skin_468(&u) : native_kind == 64 ? native_geometry_800(&u) : native_kind == 16 ? native_geometry_20c(&u) :
                             native_kind == 32 ? native_geometry_260(&u) : native_kind == 4 ? native_geometry_1638(&u) :
                             native_kind == 8 ? native_geometry_17e0(&u) : native_geometry_loop(&u)) ||
             native_kind < 64)) {  /* 08B0 and 0468 decline before writing anything: then they run pair by pair */
            unsigned count = native_count;
            if (!count || count > (unsigned)(MAX_STEPS - steps)) {
                fprintf(stderr, "native geometry failed after direct writes\n");
                return -1; /* No rollback after an unsupported direct-write path. */
            }
            static unsigned long long native_calls;
            native_calls++;
            ps2_vu1_native_runs[__builtin_ctz(native_kind)]++;
            native_key_calls[native_kind]++;
            native_key_vertices[native_kind] += native_vertices-before_vertices;
            native_key_clipped[native_kind] += native_clipped_vertices-before_clipped;
            if ((native_key_calls[native_kind] < 3 || !(native_key_calls[native_kind] % 4096)) && getenv("DHD_VU1_NATIVE_STATS"))
                fprintf(stderr, "native key=%u calls=%llu target_calls=%llu vertices=%llu clipped=%llu\n", native_kind,
                        native_key_calls[native_kind], native_key_targets[native_kind], native_key_vertices[native_kind], native_key_clipped[native_kind]);
            if ((native_calls < 3 || !(native_calls % 4096)) && getenv("DHD_VU1_NATIVE_STATS"))
                fprintf(stderr, "native geometry calls=%llu target_calls=%llu vertices=%llu "
                        "clipped=%llu rollback=0 fallback=0\n", native_calls, native_target_calls,
                        native_vertices, native_clipped_vertices);
            steps += count - 1;
            pc = native_end / 8;
            continue;
        }
#endif
        history[dispatched++ & 15] = pc * 8;
        const uint32_t *pair = (const uint32_t *)(cpu->vu0_code + pc * 8);
        uint32_t lo, hi;
        int next = -1, bad = 0;
        Upper up;
#ifdef PS2_VU_AOT
        /* Blocks start without a pending transition. Simple branch/E slots
           may be fused; a pending transition still selects a one-pair block. */
        const AotBlock *translated = !use_aot ? NULL : unit ? aot_find(pc, pair, MAX_STEPS - steps, branch >= 0 || ending)
                                                            : aot0_find(pc, pair, MAX_STEPS - steps, branch >= 0 || ending);
        if (translated) {
            next = translated->run(&u, pc, &bad);
            if (bad) return -1;
#ifdef PS2_VU_COVERAGE
            if (unit) ps2_vu1_translated += translated->count;
#endif
            steps += translated->count - 1;
            pc += translated->count - 1;
            lo = translated->words[translated->count * 2 - 2];
            hi = translated->words[translated->count * 2 - 1];
        } else {
#endif
        lo = pair[0], hi = pair[1];
#ifdef PS2_VU_COVERAGE
        if (unit) ps2_vu1_interpreted[pc]++;
#endif
        if (unit) pair_stalls(&u, hi, lo);
        apply_flags(&u);
        update_q(&u);
        if (upper(&u, hi, &up) < 0) {
            fprintf(stderr, "vu%u: unknown upper instruction %08X at 0x%04X\n", unit, hi, pc * 8);
            return -1;
        }
        if (BIT(hi, 31))  /* I bit: the lower word is a float for I */
            memcpy(&cpu->i, &lo, 4);
        else if (!(up.flags == 2 && (lo >> 25) == 0x11))
            next = lower(&u, lo, pc, &bad);
        if (bad) {
            fprintf(stderr, "vu%u: failed lower instruction %08X at 0x%04X\n", unit, lo, pc * 8);
            return -1;
        }
        commit_upper(&u, &up, FT(lo));
#ifdef PS2_VU_AOT
        }
#endif
        trace_pair(&u, pc);
        u.cycle++;
#ifdef PS2_VU_AOT
        if (translated && translated->fused) {
            pc = next >= 0 ? (unsigned)next & mask : (pc + 1) & mask;
            if (translated->fused == 2) break;
            continue;
        }
#endif
        if (ending) {
            pc = branch >= 0 ? (unsigned)branch & mask : (pc + 1) & mask;
            break;
        }
        if (BIT(hi, 30))  /* E bit: the next pair runs, then the program ends */
            ending = 1;
        if (branch >= 0) {
            pc = (unsigned)branch & mask;
            branch = -1;
        } else
            pc = (pc + 1) & mask;
        if (next >= 0)
            branch = next;
    }
    if (steps == MAX_STEPS) {
        fprintf(stderr, "vu%u: microprogram at 0x%04X didn't end\n", unit, addr);
        fprintf(stderr, "last PCs:");
        for (unsigned n = 0; n < 16; n++) fprintf(stderr, " %04X", history[(dispatched + n) & 15]);
        fprintf(stderr, "\nVI:");
        for (unsigned n = 0; n < 16; n++) fprintf(stderr, " %04X", cpu->vi[n]);
        fprintf(stderr, "\n");
        return -1;
    }
    if (u.q_ready) {  /* a division still running finishes */
        cpu->q = u.q_next;
        if (unit) u.status = (u.status & ~0x30u) | u.q_flags;
    }
    if (u.p_ready) cpu->p = u.p_next;
    cpu->vu0_ctrl[10] = pc * 8;
    drain_flags(&u);
    cpu->vu0_ctrl[0] = u.status;
    cpu->vu0_ctrl[1] = u.mac;
    cpu->vu0_ctrl[2] = u.clip;
    return 0;
}

/* DHD_VU0_CAPTURE=dir: save the input of interpreted VU0 microprograms (registers, both 4 KB
   memories and the VU1 VF/VI file VU0 sees at 0x4000) for port/tools/test_vu0_pcsx2.py.
   DHD_VU0_CAPTURE_AT delays it (seconds since start); DHD_VU0_CAPTURE_COUNT per entry, 8 by
   default. The files hold game microcode, so the directory must stay outside version control. */
static void capture(const ps2_cpu *cpu, unsigned addr)
{
    static int initialized; static const char *dir; static double after; static unsigned limit;
    static unsigned seen[512];
    if (!initialized) {
        const char *at = getenv("DHD_VU0_CAPTURE_AT"), *count = getenv("DHD_VU0_CAPTURE_COUNT");
        initialized = 1; dir = getenv("DHD_VU0_CAPTURE"); after = at ? atof(at) : 0;
        limit = count ? (unsigned)atoi(count) : 8;
    }
    if (!dir || (double)clock() / CLOCKS_PER_SEC < after || seen[(addr / 8) & 511] >= limit) return;
    unsigned n = seen[(addr / 8) & 511]++;
    char path[1024];
    snprintf(path, sizeof path, "%s/vu0-%04X-%u.bin", dir, addr, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    ps2_q128 vu1_file[64];
    ps2_vu0_vu1_file(vu1_file);
    unsigned header[4] = {addr, sizeof *cpu, sizeof vu1_file, 0};
    fwrite(header, sizeof header, 1, f); fwrite(cpu, sizeof *cpu, 1, f);
    fwrite(cpu->vu0_code, 4096, 1, f); fwrite(cpu->vu0_mem, 4096, 1, f);
    fwrite(vu1_file, sizeof vu1_file, 1, f);
    fclose(f);
}

/* DHD_VU0_STATS=N: every N interpreted VU0 microprograms, the calls and time stamp counter
   ticks so far by entry address, on stderr (a diagnostic for choosing native routines). */
static void vu0_stats(unsigned addr, unsigned long long ticks)
{
    static long long every = -1;
    static unsigned long long calls[512], spent[512], total;
    if (every < 0) { const char *s = getenv("DHD_VU0_STATS"); every = s ? atoll(s) : 0; }
    if (!every) return;
    calls[addr / 8 & 511]++; spent[addr / 8 & 511] += ticks;
    if (++total % every) return;
#ifndef PS2_VU_REFERENCE
    fprintf(stderr, "vu0 native after %llu: 0000=%llu 02E0=%llu 05E8=%llu 07B8=%llu 0808=%llu\n", total,
            vu0_native_entries[0], vu0_native_entries[1], vu0_native_entries[2], vu0_native_entries[3], vu0_native_entries[4]);
#endif
    unsigned long long all = 0;
    for (unsigned i = 0; i < 512; i++) all += spent[i];
    for (unsigned i = 0; i < 512; i++)
        if (calls[i])
            fprintf(stderr, "vu0 stats after %llu: entry %04X %llu calls, %.1f%% of ticks, %.0f ticks a call\n", total, i * 8,
                    calls[i], 100.0 * spent[i] / all, (double)spent[i] / calls[i]);
}

#if defined(PS2_VU_AOT) && !defined(PS2_VU_REFERENCE)
#include "vu0_native.h"
#endif

int ps2_vu0_run(ps2_cpu *cpu, unsigned addr)
{
    capture(cpu, addr);
    /* Timing is a launch-time diagnostic. Avoid two timestamp reads around
       short native routines when no statistics have been requested. */
    static int timing = -1;
    if (timing < 0) {
        const char *value = getenv("DHD_VU0_STATS");
        timing = value && atoll(value) != 0;
    }
    unsigned long long start = timing ? __rdtsc() : 0;
    /* VU0 micro mode rounds like VU1 (PCSX2's default VU0FPCR is toward zero, FTZ/DAZ). */
    unsigned control = _mm_getcsr();
    _mm_setcsr((control & ~0x603F) | 0xFFC0);
    int result;
#if defined(PS2_VU_AOT) && !defined(PS2_VU_REFERENCE)
    /* Native VU0 routines (vu0_native.h, finding 066). DHD_VU0_NATIVE=0, DHD_VU_INTERPRETER
       or DHD_VU_TRACE keep every program on the interpreter. */
    static int native = -1;
    if (native < 0) {
        const char *value = getenv("DHD_VU0_NATIVE");
        native = (!value || atoi(value)) && !getenv("DHD_VU_INTERPRETER") && !getenv("DHD_VU_TRACE");
    }
    if ((ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native) && addr == 0 && !cpu->vi[0] && vu0_cull_words[0] &&
        !memcmp(cpu->vu0_code, vu0_cull_words, sizeof vu0_cull_words)) {
        native0_cull(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[0]++;
        result = 0;
    } else if ((ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native) && addr == 0x5e8 && !cpu->vi[0] && vu0_basis_words[0] &&
               !memcmp(cpu->vu0_code + 0x5e8, vu0_basis_words, sizeof vu0_basis_words)) {
        native0_transform_basis(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[2]++;
        result = 0;
    } else if ((ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native) && addr == 0x2e0 && !cpu->vi[0] && vu0_bounds_words[0] &&
               !memcmp(cpu->vu0_code + 0x2e0, vu0_bounds_words, sizeof vu0_bounds_words)) {
        native0_bounds(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[1]++;
        result = 0;
    } else if ((ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native) && addr == 0x7b8 && !cpu->vi[0] && vu0_length_words[0] &&
               !memcmp(cpu->vu0_code + 0x7b8, vu0_length_words, sizeof vu0_length_words)) {
        native0_length_squared(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[3]++;
        result = 0;
    } else if ((ps2_vu0_native_mode >= 0 ? ps2_vu0_native_mode : native) && addr == 0x808 && !cpu->vi[0] && vu0_direction_words[0] &&
               !memcmp(cpu->vu0_code + 0x808, vu0_direction_words, sizeof vu0_direction_words)) {
        native0_transform_direction(cpu);
        ps2_vu0_native_runs++;
        vu0_native_entries[4]++;
        result = 0;
    } else
#endif
        result = run(cpu, addr, 0, 0, 0, NULL, NULL);
    _mm_setcsr(control);
    if (timing) vu0_stats(addr, __rdtsc() - start);
    return result;
}

int ps2_vu1_run(ps2_cpu *cpu, unsigned addr, unsigned top, unsigned itop,
                ps2_vu_kick_fn kick, void *opaque)
{
    /* VU arithmetic truncates rather than rounding to nearest. Keep this local
       to the VU so the EE/host and the native VU0 routines (vu0_micro.c) retain theirs. */
    unsigned control = _mm_getcsr();
    /* Its FMACs intentionally overflow before the existing bitwise clamp.
       Keep host exceptions masked here even during EE first-fault diagnostics. */
    _mm_setcsr((control & ~0x603F) | 0xFFC0); /* toward zero, FTZ/DAZ, host masks */
    int result = run(cpu, addr, 1, top, itop, kick, opaque);
    _mm_setcsr(control);
    return result;
}
