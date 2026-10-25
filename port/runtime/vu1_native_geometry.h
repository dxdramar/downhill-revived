static unsigned long long native_vertices, native_clipped_vertices;
/* Direct-write SIMD geometry loop with a directly threaded clipping path. */
typedef struct { __m128i sign, zero, overflow; __m128 last; int mask; } NativeFlags;
static __m128 native_mask(int d)
{
    return _mm_castsi128_ps(_mm_setr_epi32(d & 8 ? -1 : 0, d & 4 ? -1 : 0, d & 2 ? -1 : 0, d & 1 ? -1 : 0));
}
static __m128 native_merge(__m128 old, __m128 value, int d)
{
    __m128 mask = native_mask(d);
    return _mm_or_ps(_mm_andnot_ps(mask, old), _mm_and_ps(mask, value));
}
static __m128 native_result(NativeFlags *f, __m128 value, int d)
{
    __m128i bits = _mm_castps_si128(value), mask = _mm_castps_si128(native_mask(d));
    f->sign = _mm_or_si128(f->sign, _mm_and_si128(bits, mask));
    f->zero = _mm_or_si128(f->zero, _mm_and_si128(_mm_castps_si128(_mm_cmpeq_ps(value, _mm_setzero_ps())), mask));
    /* f->overflow stays 0: no result has exponent 255 (fmac_flags4). */
    f->last = value; f->mask = d;
    /* All values here are SSE arithmetic results on operand4 values under FTZ/DAZ and
       toward-zero rounding: no subnormal FMAC result survives to set the interpreter's U test,
       and arithmetic4 of the result is the identity (operand4, vu0_interp.c). */
    return value;
}
static __m128 native_add(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, _mm_add_ps(operand4(a), operand4(b)), d); }
static __m128 native_sub(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, _mm_sub_ps(operand4(a), operand4(b)), d); }
static __m128 native_mul(NativeFlags *f, __m128 a, __m128 b, int d)
{ return native_result(f, _mm_mul_ps(operand4(a), operand4(b)), d); }
static __m128 native_madd(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{
    return native_result(f, _mm_add_ps(operand4(acc), _mm_mul_ps(operand4(a), operand4(b))), d);
}
/* acc must have all four lanes written by a preceding FMAC under the VU1
   FTZ/DAZ, toward-zero environment. Such results are finite normals or zeros.
   Keep the optimizer barrier even though the exceptional-value clamp is dead. */
static __m128 native_madd_clean_acc(NativeFlags *f, __m128 acc, __m128 a, __m128 b, int d)
{
#ifdef __wasm__
    __asm__("" : "+r"(acc));  /* wasm: "r" holds a v128 too (finding 075) */
#else
    __asm__("" : "+x"(acc));
#endif
    return native_result(f, _mm_add_ps(acc, _mm_mul_ps(operand4(a), operand4(b))), d);
}
static __m128 native_itof(__m128 value, float scale)
{ return _mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(value)), _mm_set1_ps(scale)); }
static __m128 native_ftoi(__m128 value, float scale)
{
    __m128 scaled = _mm_mul_ps(value, _mm_set1_ps(scale));
    __m128i bits = _mm_castps_si128(scaled);
    __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7f800000));
    __m128i huge = _mm_cmpgt_epi32(exponent, _mm_set1_epi32(0x4effffff));
    __m128i saturated = _mm_xor_si128(_mm_srai_epi32(bits, 31), _mm_set1_epi32(0x7fffffff));
    return _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(huge, saturated), _mm_andnot_si128(huge, _mm_cvttps_epi32(scaled))));
}
static unsigned native_clip(__m128 value, __m128 limit)
{
    uint32_t v[4], l; _mm_storeu_si128((__m128i *)v, _mm_castps_si128(value));
    memcpy(&l, &limit, 4);
    l = l & 0x7f800000 ? l & 0x7fffffff : 0x007fffff;
    unsigned bits = 0;
    for (int c = 0; c < 3; c++) {
        if ((int32_t)v[c] > (int32_t)l) bits |= 1u << (c * 2);
        if ((int32_t)(v[c] ^ 0x80000000) > (int32_t)l) bits |= 2u << (c * 2);
    }
    return bits;
}
static unsigned native_geometry_loop(Vu *u)
{
    unsigned begin = u->c->vi[8], end = u->c->vi[10];
    unsigned distance = (end - begin) & 0xffff;
    if (!distance || distance % 3 || distance / 3 > 64 || (u->q_ready && u->q_ready > u->cycle)) { return 0; }
    ps2_q128 *memory = u->c->vu0_mem;
#define cpu (*u->c)
    drain_flags(u); update_q(u);
    unsigned entry_status = u->status;
    __m128 v[32], acc = _mm_loadu_ps(cpu.vacc);
    for (unsigned i = 0; i < 32; i++) v[i] = vf4(u, i);
    NativeFlags flags = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_ps(), 0};
    unsigned clip = u->clip, pairs = 0, index = begin;
    uint16_t *vi = cpu.vi;
    float q = cpu.q; unsigned q_flags = 0;
#define NATIVE_SAVE() do { \
    for (unsigned i = 1; i < 32; i++) _mm_storeu_ps(cpu.vf[i].f,v[i]); \
    _mm_storeu_ps(cpu.vacc,acc); cpu.q = q; \
    Upper last = {0}; fmac_flags4(&last,flags.last,flags.mask); \
    unsigned mac = last.mac; \
    unsigned status = (mac & 15 ? 1 : 0) | (mac & 240 ? 2 : 0) | (mac & 3840 ? 4 : 0) | (mac & 61440 ? 8 : 0); \
    unsigned sticky = (_mm_movemask_ps(_mm_castsi128_ps(flags.zero)) ? 1 : 0) | \
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.sign)) ? 2 : 0) | \
                      (_mm_movemask_ps(_mm_castsi128_ps(flags.overflow)) ? 8 : 0); \
    u->status = (entry_status & ~0x3fu) | status | q_flags | (sticky << 6); \
    u->mac = mac; u->clip = u->clip_next = clip; u->n_flags = 0; u->flag_head = 0; \
    u->q_ready = 0; u->cycle += 64; u->flag_cycle = u->cycle; \
} while(0)
#define NADD(a,b,d) native_add(&flags,(a),(b),(d))
#define NSUB(a,b,d) native_sub(&flags,(a),(b),(d))
#define NMUL(a,b,d) native_mul(&flags,(a),(b),(d))
#define NMADD_CLEAN_ACC(a,b,c,d) native_madd_clean_acc(&flags,(a),(b),(c),(d))
#define SET(r,x,d) v[r] = native_merge(v[r], (x), (d))
#define LOAD(a) _mm_loadu_ps(memory[(a) & 1023].f)
#define STORE(a,x,d) store_fields(memory[(a) & 1023].f, _mm_castps_si128(x), (d))
    do {
        v[15] = NMUL(v[15], v[31], 15); cpu.i = 255.0f;
        SET(16, NSUB(v[24], v[10], 14), 14); vi[9] = (uint16_t)memory[(index + 6) & 1023].w[3];
        SET(5, NADD(v[13], lane4(v[25], 3), 2), 2); v[10] = v[24];
        SET(5, NADD(v[24], v[13], 12), 12);
        float denominator = _mm_cvtss_f32(arithmetic4(lane4(v[26], 3)));
        q_flags = denominator == 0 ? 0x20 : 0;
        q = denominator == 0 ? vu_max(signbit(denominator)) : _mm_cvtss_f32(arithmetic4(_mm_div_ss(_mm_set_ss(1.0f), _mm_set_ss(denominator))));
        v[15] = minmax4(v[15], _mm_set1_ps(cpu.i), 0); v[25] = v[26];
        SET(17, native_ftoi(v[23], 16.0f), 1); SET(26, lane4(v[18], 3), 1);
        SET(7, NADD(v[7], v[19], 12), 12); v[20] = LOAD(181 + vi[9]);
        SET(6, NSUB(v[0], lane4(v[6], 0), 8), 8); v[9] = LOAD(8 + index);
        __m128 a = _mm_shuffle_ps(v[16], v[16], _MM_SHUFFLE(3,0,2,1));
        __m128 b = _mm_shuffle_ps(v[14], v[14], _MM_SHUFFLE(3,1,0,2));
        acc = native_merge(acc, NMUL(a,b,14), 14); vi[3] = 0x20;
        a = operand4(_mm_shuffle_ps(v[14], v[14], _MM_SHUFFLE(3,0,2,1)));
        b = operand4(_mm_shuffle_ps(v[16], v[16], _MM_SHUFFLE(3,1,0,2)));
        __m128 cross = native_result(&flags, _mm_sub_ps(operand4(acc), _mm_mul_ps(a,b)), 14);
        SET(12, cross, 14);
        uint32_t packed[4]; _mm_storeu_si128((__m128i *)packed, _mm_castps_si128(v[17])); vi[7] = (uint16_t)packed[3];
        unsigned backface = (_mm_movemask_ps(flags.last) & 4) ? 0x20 : 0;
        clip = ((clip << 6) | native_clip(v[5], lane4(v[13], 3))) & 0xffffff;
        vi[1] = q_flags;
        /* All ACC lanes overwritten here; the earlier XYZ cross-product is not clean in W. */
        acc = NMUL(v[0], v[22], 15); STORE(index, v[8], 14);
        v[23] = NMADD_CLEAN_ACC(acc, v[26], _mm_set1_ps(q), 15);
        v[14] = NMUL(v[16], lane4(v[6], 0), 15); vi[3] = backface;
        if(q_flags) { SET(8, NADD(v[7],lane4(v[0],0),14),14); }
        else { SET(8, NMUL(v[7], _mm_set1_ps(q), 14), 14); } vi[1] = (clip & 0x02fbef) != 0;
        v[15] = native_ftoi(v[15], 1.0f);
        SET(23, minmax4(v[23], lane4(v[18],1),0), 1); v[28] = v[5];
        if (backface) vi[7] |= vi[4];
        STORE(1014, v[6], 15);
        native_vertices++;
        STORE(index + 1, v[15], 15);
        if (vi[1]) {
            native_clipped_vertices++;
            NATIVE_SAVE();
            unsigned clip_pairs = native_clip_run(u, 0x218/8);
            if(!clip_pairs) return 0;
            pairs += clip_pairs;
            for(unsigned i=0;i<32;i++)v[i]=vf4(u,i);
            acc=_mm_loadu_ps(cpu.vacc);q=cpu.q;
            clip=u->clip;entry_status=u->status;q_flags=u->status&0x30;
            memset(&flags,0,sizeof flags);
        }
        /* Reestablish all-lane provenance after any clipping-path state reload. */
        acc = NMUL(v[4], lane4(v[0],3),15); v[11] = v[7];
        v[7] = native_itof(v[9], 1.0f/4096.0f); v[15] = LOAD(4 + index);
        SET(23, minmax4(v[23], lane4(v[0],0),1),1); index = (index + 3) & 0xffff; vi[8] = (uint16_t)index;
        SET(30,native_ftoi(v[24],16.0f),14); SET(24,v[23],14);
        acc = NMADD_CLEAN_ACC(acc,v[1],lane4(v[20],0),15); v[29] = v[28];
        v[15] = native_itof(v[15],1.0f); v[6] = LOAD(1014);
        acc = NMADD_CLEAN_ACC(acc,v[2],lane4(v[20],1),15); memory[(index-1)&1023].w[3] = vi[7];
        v[26] = NMADD_CLEAN_ACC(acc,v[3],lane4(v[20],2),15);
        SET(23,NADD(v[23],lane4(v[11],3),1),1); STORE(index-1,v[30],14);
        pairs += 29 + !!backface;
    } while (index != end);
    NATIVE_SAVE();
    return pairs;
#undef NATIVE_SAVE
#undef cpu
#undef NADD
#undef NSUB
#undef NMUL
#undef NMADD_CLEAN_ACC
#undef SET
#undef LOAD
#undef STORE
}
