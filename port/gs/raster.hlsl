// GS raster operations over its original swizzled VRAM. One dispatch per
// batch uses ordered tile lists; texture reads see a coherent VRAM snapshot.
#ifndef DHD_EARLY_DEPTH
#define DHD_EARLY_DEPTH 0
#endif
#ifndef DHD_EXCLUSIVE_WORD_STORE
#define DHD_EXCLUSIVE_WORD_STORE 0
#endif
#ifndef DHD_ADDRESS_SHIFT
#define DHD_ADDRESS_SHIFT 0
#endif

cbuffer State : register(b0) {
    uint4 regs[128];
    uint4 position[3];
    uint4 uv[3];
    float4 stq[3];
    uint4 bounds;
    uint4 info;
    uint4 palette[64];
};
ByteAddressBuffer texture_memory : register(t0);
#if DHD_HARDWARE_PIXEL
RasterizerOrderedByteAddressBuffer memory : register(u0);
#else
RWByteAddressBuffer memory : register(u0);
#endif
struct Primitive {
    uint4 position[3];
    uint4 uv[3];
    float4 stq[3];
    uint4 info;
    uint4 bounds;
    uint4 depth[2];
};
StructuredBuffer<Primitive> primitives : register(t1);
StructuredBuffer<uint> tile_list : register(t2);
cbuffer Display : register(b1) { uint4 display_state[3]; };
cbuffer Copy : register(b2) { uint4 copy_state[3]; };
RWTexture2D<float4> screen : register(u1);


uint bits_per_pixel(uint psm) {
    if (psm == 1 || psm == 49) return 24;
    if ((psm & 7) == 2) return 16;
    if (psm == 19 || psm == 27) return 8;
    if (psm == 20 || psm == 36 || psm == 44) return 4;
    return 32;
}

uint address(uint psm, uint bp, uint bw, uint x, uint y) {
    uint pw = 64, ph = 32, bits = 32, high = 0, block = 0, column = 0, bx = 0, by = 0;
    if (psm == 19 || psm == 20) { pw = 128; ph = psm == 19 ? 64 : 128; }
    else if ((psm & 7) == 2) ph = 64;
#if DHD_ADDRESS_SHIFT
    uint sx = pw == 128 ? 7 : 6;
    uint sy = ph == 128 ? 7 : ph == 64 ? 6 : 5;
    // The swizzle below selects only bits inside a page in each format;
    // higher coordinate bits are consumed solely by the page calculation.
    uint px = x, py = y;
#else
    uint px = x % pw, py = y % ph;
#endif
    if (psm == 19) {
        bits = 8; bx = px / 16; by = py / 16;
        block = (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
        column = (px & 1) << 2 | (px & 2) << 3 | (px & 4) << 3 | (px & 8) >> 2 |
                 (py & 1) << 3 | (py & 2) >> 1 | (py & 4) << 4 | (py & 8) << 4;
        column ^= ((py >> 1 ^ py >> 2) & 1) << 5;
    } else if (psm == 20) {
        bits = 4; bx = px / 32; by = py / 16;
        block = (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
        column = (px & 1) << 3 | (px & 2) << 4 | (px & 4) << 4 | (px & 8) >> 2 | (px & 16) >> 2 |
                 (py & 1) << 4 | (py & 2) >> 1 | (py & 4) << 5 | (py & 8) << 5;
        column ^= ((py >> 1 ^ py >> 2) & 1) << 6;
    } else if ((psm & 7) == 2) {
        bits = 16; bx = px / 16; by = py / 8;
        if (psm & 8)
            block = (by & 1) | (bx & 1) << 1 | (by & 4) | (by & 2) << 2 | (bx & 2) << 3;
        else block = (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
        column = (px & 1) << 1 | (px & 2) << 2 | (px & 4) << 2 | (px & 8) >> 3 |
                 (py & 1) << 2 | (py & 6) << 4;
    } else {
        bx = px / 8; by = py / 8;
        block = (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
        column = (px & 1) | (px & 6) << 1 | (py & 1) << 1 | (py & 6) << 3;
        if (psm == 27 || psm == 36) high = 24;
        if (psm == 44) high = 28;
    }
#if DHD_ADDRESS_SHIFT
    // Keep the original uint multiply before shifting, including its wrap.
    block += bp + ((y >> sy) * ((bw * 64u) >> sx) + (x >> sx)) * 32;
#else
    block += bp + ((y / ph) * (bw * 64 / pw) + x / pw) * 32;
#endif
    if ((psm & 48) == 48) block ^= 24;
    return (block * 2048 + column * bits + high) & 0x1FFFFFF;
}

uint pixel(uint psm, uint bp, uint bw, uint2 xy, bool tex) {
    uint a = address(psm, bp, bw, xy.x, xy.y), bits = bits_per_pixel(psm);
    uint value = tex ? texture_memory.Load((a >> 5) * 4) : memory.Load((a >> 5) * 4);
    return (value >> (a & 31)) & (bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1);
}

void write_pixel_impl(uint psm, uint bp, uint bw, uint2 xy, uint value, uint preserve, bool exclusive) {
    uint a = address(psm, bp, bw, xy.x, xy.y), bits = bits_per_pixel(psm);
    uint mask = (bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1) & ~preserve;
    mask <<= a & 31; value = (value << (a & 31)) & mask;
    uint old = memory.Load((a >> 5) * 4), prior;
#if DHD_EXCLUSIVE_WORD_STORE
    // The raster host proof gives this invocation the entire storage word.
    // Keep an explicit canonical whitelist; packed formats always use CAS.
    if (exclusive && (psm == 0 || psm == 1 || psm == 48 || psm == 49)) {
        memory.Store((a >> 5) * 4, (old & ~mask) | value);
        return;
    }
#endif
    // Packed pixels may share a word; masked writes must be atomic.
    [allow_uav_condition] for (;;) {
        memory.InterlockedCompareExchange((a >> 5) * 4, old, (old & ~mask) | value, prior);
        if (old == prior) break;
        old = prior;
    }
}
// Non-raster callers never inherit the raster cbuffer's ownership flag.
void write_pixel(uint psm, uint bp, uint bw, uint2 xy, uint value, uint preserve) {
    write_pixel_impl(psm, bp, bw, xy, value, preserve, false);
}

void write_raster_pixel(uint psm, uint bp, uint bw, uint2 xy, uint value, uint preserve) {
    write_pixel_impl(psm, bp, bw, xy, value, preserve, (info.y & 1) != 0);
}

int4 rgba(uint v) { return int4(v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24); }
uint pack32(int4 v) { return (uint)v.r | (uint)v.g << 8 | (uint)v.b << 16 | (uint)v.a << 24; }
uint pack16(uint v) { return (v >> 3 & 31) | (v >> 6 & 0x3E0) | (v >> 9 & 0x7C00) | (v >> 16 & 0x8000); }
uint expand(uint value, uint psm, bool tex) {
    if ((psm & 7) == 2) {
        uint a = value & 0x8000 ? regs[59].y & 255 : regs[59].x & 255;
        if (!tex) a = value & 0x8000 ? 128 : 0;
        if (tex && (regs[59].x & 0x8000) && value == 0) a = 0;
        return (value & 31) << 3 | (value & 0x3E0) << 6 | (value & 0x7C00) << 9 | a << 24;
    }
    if ((psm & 7) == 1) {
        uint a = tex ? regs[59].x & 255 : 128;
        if (tex && (regs[59].x & 0x8000) && !value) a = 0;
        return value | a << 24;
    }
    return value;
}

int clamp_coordinate(int v, uint mode, uint size, uint low, uint high) {
    if (mode == 0) return v & (size - 1);
    if (mode == 1) return clamp(v, 0, (int)size - 1);
    if (mode == 2) return clamp(v, (int)low, (int)high);
    return (v & low) | high;
}

int4 texel(int2 xy, uint ctxt, uint level) {
    uint2 t = regs[6 + ctxt].xy, c = regs[8 + ctxt].xy;
    uint tw = t.x >> 26 & 15, th = (t.x >> 30) | (t.y & 3) << 2;
    tw = tw > level ? tw - level : 0; th = th > level ? th - level : 0;
    xy.x = clamp_coordinate(xy.x, c.x & 3, 1u << tw, (c.x >> 4 & 1023) >> level, (c.x >> 14 & 1023) >> level);
    xy.y = clamp_coordinate(xy.y, c.x >> 2 & 3, 1u << th, ((c.x >> 24) | (c.y & 3) << 8) >> level, (c.y >> 2 & 1023) >> level);
    uint psm = t.x >> 20 & 63;
    uint location = t.x;
    if (level) {
        uint2 mip = regs[(level <= 3 ? 52 : 54) + ctxt].xy;
        uint part = (level - 1) % 3;
        location = part == 0 ? mip.x : part == 1 ? (mip.x >> 20) | (mip.y << 12) : mip.y >> 8;
    }
    uint v = pixel(psm, location & 0x3FFF, location >> 14 & 63, (uint2)xy & 2047, true);
    if (psm == 19 || psm == 20 || psm == 27 || psm == 36 || psm == 44) {
        uint4 entry = palette[(v & 255) >> 2];
        uint component = v & 3;
        v = component == 0 ? entry.x : component == 1 ? entry.y : component == 2 ? entry.z : entry.w;
    } else v = expand(v, psm, true);
    return rgba(v);
}

int4 sample_level(float2 st, uint ctxt, uint level, bool filtered) {
    st *= exp2(-(float)level);
    int4 result = 0;
    if (filtered) {
        float2 at = st - 0.5;
        int2 p = (int2)floor(at), f = (int2)(frac(at) * 16);
        int4 a = texel(p, ctxt, level), b = texel(p + int2(1,0), ctxt, level);
        int4 c = texel(p + int2(0,1), ctxt, level), d = texel(p + 1, ctxt, level);
        result = ((a * (16 - f.x) + b * f.x) * (16 - f.y) + (c * (16 - f.x) + d * f.x) * f.y) >> 8;
    } else result = texel((int2)floor(st), ctxt, level);
    return result;
}

int4 sample_texture(float2 st, float q, uint ctxt) {
    uint2 tex1 = regs[20 + ctxt].xy;
    uint mmin = tex1.x >> 6 & 7, mxl = min(tex1.x >> 2 & 7, 6u);
    float k = (float)((int)(tex1.y << 20) >> 20) / 16.0;
    float lod = k;
    if (!(tex1.x & 1)) lod -= log2(max(abs(q), 1.17549435e-38)) * (1u << (tex1.x >> 19 & 3));
    // MXL=0 forces magnification filtering (verified on PS2 by PCSX2).
    bool filtered = !mxl || lod <= 0 ? (tex1.x & 32) != 0 : mmin == 1 || mmin == 4 || mmin == 5;
    int4 result = 0;
    // The GS keeps 16 fractional LOD bits; nearest rounds at half a level.
    uint fixed_lod = (uint)(clamp(lod, 0, (float)mxl) * 65536.0);
    if (mmin < 2 || mmin > 5 || !mxl) result = sample_level(st, ctxt, 0, filtered);
    else if (!(mmin & 1)) result = sample_level(st, ctxt, (fixed_lod + 32768) >> 16, filtered);
    else {
        uint level = fixed_lod >> 16;
        int4 a = sample_level(st, ctxt, level, filtered);
        int4 b = sample_level(st, ctxt, min(level + 1, mxl), filtered);
        result = a + (((b - a) * (int)((fixed_lod & 65535) >> 1)) >> 15);
    }
    return result;
}

float edge(int2 a, int2 b, int2 p) {
    int2 d = b - a, q = p - a;
    // Each GS coordinate difference fits 16 unsigned bits. Subtract magnitudes
    // before conversion so even nearly cancelling products retain the exact sign.
    uint first = (uint)abs(d.x) * (uint)abs(q.y);
    uint second = (uint)abs(d.y) * (uint)abs(q.x);
    bool first_negative = (d.x < 0) != (q.y < 0);
    bool second_negative = (d.y < 0) != (q.x < 0);
    float result;
    if (first_negative == second_negative)
        result = first >= second ? (float)(first - second) : -(float)(second - first);
    else result = (float)first + (float)second;
    return first_negative ? -result : result;
}
bool top_left(int2 a, int2 b) { int2 d = b - a; return d.y < 0 || (d.y == 0 && d.x > 0); }
bool inside(float e, int2 a, int2 b, int sign) {
    return e * sign > 0 || (e == 0 && (sign > 0 ? top_left(a,b) : top_left(b,a)));
}

#if DHD_NO_DOUBLE
// Metal has no doubles (the Mac build, finding 075): the depth sums run in float, the double's
// bits truncated to a float's. Not exact for 24- and 32-bit Z far from the camera.
float asdouble_float(uint lo, uint hi) {
    uint exponent = hi >> 20 & 0x7FF;
    int e = (int)exponent - 1023 + 127;
    if (exponent == 0 || e <= 0) return 0;
    if (e >= 255) return asfloat((hi & 0x80000000) | 0x7F800000);
    return asfloat((hi & 0x80000000) | (uint)e << 23 | (hi & 0xFFFFF) << 3 | lo >> 29);
}
#define double float
#define asdouble asdouble_float
#endif

uint depth_integer(double z) {
    if (z <= 0.0) return 0;
    if (z >= 4294967295.0) return 0xFFFFFFFF;
    // SM5's base double feature has no direct double-to-uint conversion.
    // Convert two exact 16-bit pieces, correcting float rounding at boundaries.
    uint hi = (uint)(float)(z * (1.0 / 65536.0));
    if ((double)(float)hi * 65536.0 > z) hi--;
    double remainder = z - (double)(float)hi * 65536.0;
    uint lo = (uint)(float)remainder;
    if ((double)(float)lo > remainder) lo--;
    return (hi << 16) | lo;
}

void draw_pixel(uint2 xy, Primitive d) {
    if (any(xy < d.bounds.xy) || any(xy >= d.bounds.xy + d.bounds.zw)) return;
    uint scanmask = regs[34].x & 3;
    if ((scanmask == 2 && !(xy.y & 1)) || (scanmask == 3 && (xy.y & 1))) return;
    uint prim = d.info.y, ctxt = prim >> 9 & 1, type = d.info.x, last = d.info.z - 1;
    int2 p = (int2)xy * 16, a = (int2)d.position[0].xy, b = (int2)d.position[1].xy, c = (int2)d.position[2].xy;
    float3 weight = float3(1,0,0);
    float2 sprite_fraction = 0;
    if (type == 0) {
        if (any((a >> 4) != (int2)xy)) return;
    } else if (type == 6) {
        if (any(p < min(a,b)) || any(p >= max(a,b))) return;
        sprite_fraction = (float2)(p - a) / (float2)(b - a);
        weight = float3(0,1,0);
    } else if (type == 1 || type == 2) {
        int2 d = b - a;
        int major = max(abs(d.x), abs(d.y));
        if (!major) return;
        float along = abs(d.x) >= abs(d.y) ? (float)(p.x - a.x) / d.x : (float)(p.y - a.y) / d.y;
        if (along < 0 || along >= 1 || abs((float)edge(a,b,p)) >= major * 8.0) return;
        weight = float3(1 - along, along, 0);
    } else {
        if (any(p < min(a,min(b,c))) || any(p > max(a,max(b,c)))) return;
        float area = edge(a,b,c), e0 = edge(b,c,p), e1 = edge(c,a,p), e2 = edge(a,b,p);
        if (!area) return;
        int sign = area > 0 ? 1 : -1;
        if (!inside(e0,b,c,sign) || !inside(e1,c,a,sign) || !inside(e2,a,b,sign)) return;
        weight = float3(e0,e1,e2) / (float)area;
    }
    int4 color = rgba(d.position[last].w);
    if ((prim & 8) && type != 6)
        color = (int4)(rgba(d.position[0].w) * weight.x + rgba(d.position[1].w) * weight.y + rgba(d.position[2].w) * weight.z);
    uint z = d.position[last].z;
    bool constant_z = d.position[0].z == d.position[1].z && (d.info.z < 3 || d.position[0].z == d.position[2].z);
    if (type != 0 && type != 6 && !constant_z) {
        precise double depth = asdouble(d.depth[0].x, d.depth[0].y);
        depth += asdouble(d.depth[0].z, d.depth[0].w) * (double)(float)(p.x - a.x);
        depth += asdouble(d.depth[1].x, d.depth[1].y) * (double)(float)(p.y - a.y);
        z = depth_integer(depth);
    }
#if DHD_EARLY_DEPTH
    // info.y bit 0 is the host's independent_pixels proof for this dispatch.
    // Unsafe aliases retain the original late load: another pixel may write Z
    // while this invocation shades. Texture reads use a separate snapshot SRV.
    bool early_depth = (info.y & 1) != 0;
    if (early_depth) {
        uint early_test = regs[71 + ctxt].x;
        if (early_test & 0x10000) {
            uint early_frame = regs[76 + ctxt].x, early_zbuf = regs[78 + ctxt].x;
            uint early_zpsm = (early_zbuf >> 24 & 15) | 48;
            uint early_bits = bits_per_pixel(early_zpsm);
            uint early_z = min(z, early_bits == 32 ? 0xFFFFFFFF : (1u << early_bits) - 1);
            uint old_z = pixel(early_zpsm, (early_zbuf & 511) * 32, early_frame >> 16 & 63, xy, false);
            uint ztest = early_test >> 17 & 3;
            if (ztest == 0 || (ztest == 2 && early_z < old_z) || (ztest == 3 && early_z <= old_z)) return;
        }
    }
#endif
    if (prim & 16) {
        float2 st;
        float3 interp = d.stq[0].xyz * weight.x + d.stq[1].xyz * weight.y + d.stq[2].xyz * weight.z;
        if (type == 6) interp = float3(lerp(d.stq[0].xy, d.stq[1].xy, sprite_fraction), d.stq[1].z);
        if (prim & 256) {
            st = ((float2)d.uv[0].xy * weight.x + (float2)d.uv[1].xy * weight.y + (float2)d.uv[2].xy * weight.z) / 16;
            if (type == 6) st = lerp((float2)d.uv[0].xy, (float2)d.uv[1].xy, sprite_fraction) / 16;
        } else {
            uint2 t = regs[6 + ctxt].xy;
            float2 size = float2(1u << (t.x >> 26 & 15), 1u << ((t.x >> 30) | (t.y & 3) << 2));
            st = interp.xy * size / (interp.z == 0 ? 1.17549435e-38 : interp.z);
        }
        int4 tex = sample_texture(st, interp.z, ctxt);
        uint tfx = regs[6 + ctxt].y >> 3 & 3, tcc = regs[6 + ctxt].y >> 2 & 1;
        int alpha = color.a;
        if (tfx == 0) color = (color * tex) >> 7;
        else if (tfx == 1) color = tex;
        else {
            color.rgb = ((color.rgb * tex.rgb) >> 7) + alpha;
            color.a = tfx == 2 ? tex.a + alpha : tex.a;
        }
        if (!tcc) color.a = alpha;
        color = clamp(color, 0, 255);
    }
    if (prim & 32) {
        uint fog = (uint)((float)d.uv[0].z * weight.x + (float)d.uv[1].z * weight.y + (float)d.uv[2].z * weight.z);
        int3 fc = rgba(regs[61].x).rgb;
        color.rgb = fc + (((color.rgb - fc) * (int)fog) >> 8);
    }
    uint test = regs[71 + ctxt].x;
    bool write_frame = true, write_z = !(regs[78 + ctxt].y & 1), rgb_only = false;
    if (test & 1) {
        uint atst = test >> 1 & 7, ref = test >> 4 & 255;
        bool alpha_pass = atst == 1 || (atst == 2 && color.a < (int)ref) || (atst == 3 && color.a <= (int)ref) ||
                    (atst == 4 && color.a == (int)ref) || (atst == 5 && color.a >= (int)ref) ||
                    (atst == 6 && color.a > (int)ref) || (atst == 7 && color.a != (int)ref);
        if (!alpha_pass) {
            uint fail = test >> 12 & 3;
            if (!fail) return;
            write_frame = fail != 2; write_z = write_z && fail == 2; rgb_only = fail == 3;
        }
    }
    uint frame = regs[76 + ctxt].x, bw = frame >> 16 & 63, bp = (frame & 511) * 32, psm = frame >> 24 & 63;
    uint zbuf = regs[78 + ctxt].x, zpsm = (zbuf >> 24 & 15) | 48, zbp = (zbuf & 511) * 32;
    uint zb = bits_per_pixel(zpsm);
    z = min(z, zb == 32 ? 0xFFFFFFFF : (1u << zb) - 1);
    uint raw = pixel(psm, bp, bw, xy, false);
    int4 dst = rgba(expand(raw, psm, false));
    if ((test & 0x4000) && psm != 1 && ((dst.a >> 7) != (int)(test >> 15 & 1))) return;
    if (test & 0x10000) {
#if DHD_EARLY_DEPTH
        if (!early_depth) {
#endif
        uint ztest = test >> 17 & 3, old_z = pixel(zpsm, zbp, bw, xy, false);
        if (ztest == 0 || (ztest == 2 && z < old_z) || (ztest == 3 && z <= old_z)) return;
#if DHD_EARLY_DEPTH
        }
#endif
    } else write_z = false;
    if (write_frame) {
        if ((prim & 64) && (!(regs[73].x & 1) || (color.a & 128))) {
            uint alpha = regs[66 + ctxt].x;
            uint aa = alpha & 3, bb = alpha >> 2 & 3, cc = alpha >> 4 & 3, dd = alpha >> 6 & 3;
            int3 av = aa == 0 ? color.rgb : aa == 1 ? dst.rgb : int3(0,0,0);
            int3 bv = bb == 0 ? color.rgb : bb == 1 ? dst.rgb : int3(0,0,0);
            int cv = cc == 0 ? color.a : cc == 1 ? dst.a : (int)(regs[66 + ctxt].y & 255);
            int3 dv = dd == 0 ? color.rgb : dd == 1 ? dst.rgb : int3(0,0,0);
            color.rgb = (((av - bv) * cv) >> 7) + dv;
        }
        if ((psm & 7) == 2 && (regs[69].x & 1)) {
            uint index = (xy.y & 3) * 4 + (xy.x & 3), word = regs[68][index >> 3];
            int dither = (int)((word >> ((index & 7) * 4) & 7) << 29) >> 29;
            color.rgb += dither;
        }
        color = regs[70].x & 1 ? clamp(color, 0, 255) : color & 255;
        if (regs[74 + ctxt].x & 1) color.a |= 128;
        uint mask = regs[76 + ctxt].y | (rgb_only ? 0xFF000000 : 0), value = pack32(color);
        if ((psm & 7) == 2) { value = pack16(value); mask = pack16(mask); }
        write_raster_pixel(psm, bp, bw, xy, value, mask);
    }
    if (write_z) write_raster_pixel(zpsm, zbp, bw, xy, z, 0);
}

[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 group : SV_GroupID) {
    uint2 xy = bounds.xy + tid.xy;
    uint tile = (group.y * info.x + group.x) * 2;
    uint first = tile_list[tile], count = tile_list[tile + 1];
    [loop] for (uint i = 0; i < count; i++) draw_pixel(xy, primitives[tile_list[first + i]]);
}

// A local-to-local transfer (TRXDIR 2) between rectangles on different pages: each pixel is
// independent, so the GS's scan order (TRXPOS DIR) does not matter.
[numthreads(8,8,1)]
void local_copy(uint3 tid : SV_DispatchThreadID) {
    uint4 a = copy_state[0], b = copy_state[1], c = copy_state[2];
    if (tid.x >= c.x || tid.y >= c.y) return;
    uint v = pixel(a.x, a.y, a.z, (uint2(a.w, b.x) + tid.xy) & 2047, false);
    write_pixel(b.y, b.z, b.w, (uint2(c.z, c.w) + tid.xy) & 2047, v, 0);
}

// Scanout stays on the GPU. Only screenshot requests map a staging texture.
[numthreads(8,8,1)]
void scanout(uint3 tid : SV_DispatchThreadID) {
    uint4 a = display_state[0], b = display_state[1];
    if (tid.x >= b.y || tid.y >= b.z) return;
    uint2 xy = uint2(a.w + tid.x, b.x + (tid.y >> b.w)) & 2047;
    uint v = pixel(a.x, a.y, a.z, xy, true);
    if (a.x == 2 || a.x == 10) v = ((v & 31) << 3) | ((v & 0x3e0) << 6) | ((v & 0x7c00) << 9);
    screen[tid.xy] = float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, 255) / 255.0;
}

// Hardware supplies a conservative coverage superset only. Pixel operations,
// including the exact GS integer edge test, remain shared with compute.
struct HardwareVertex {
    float4 location : SV_Position;
    nointerpolation uint primitive : TEXCOORD0;
};
HardwareVertex hardware_vertex(uint vertex : SV_VertexID) {
    HardwareVertex o;
    o.primitive = vertex / 3;
    int2 fixed_position = (int2)primitives[o.primitive].position[vertex % 3].xy;
    float2 screen_position = (float2)fixed_position * (1.0 / 16.0) + 0.5;
    o.location = float4(screen_position * float2(1.0 / 1024.0, -1.0 / 1024.0) + float2(-1, 1), 0, 1);
    return o;
}
#if DHD_HARDWARE_PIXEL
void hardware_pixel(HardwareVertex input) {
    draw_pixel((uint2)input.location.xy, primitives[input.primitive]);
}
#endif

#ifdef DHD_NATIVE
// Native renderer (DHD_RENDER=native, gpu_native.inc): ordinary hardware triangles into real
// render targets. Colours stay in GS units (0..255); stored alpha is GS alpha / 128, so 0x80
// is 1.0 for blending. Pictures are close to the GS, not exact; EE-visible state never uses them.
// native_scale (DHD_SCALE, resolution scale): the target's scale T, the texture's S (an upscaled
// surface, else 1), and E = min(S, T), the sub-texels per GS texel a draw samples. A target at
// scale T holds GS pixel i in pixels [i*T, (i+1)*T): GS pixel centres are at integer positions, so
// a vertex at GS x lands at (x + 0.5) * T (the viewport does the multiplication).
cbuffer Native : register(b3) {
    float2 native_target; uint native_flags; uint native_atst;
    uint native_aref; float native_alpha_scale; float native_texa_alpha; uint native_scale;
    float4 native_fog;
    int2 native_tex_offset; uint2 native_tex_size;
    uint4 native_wrap;
    uint4 native_region;
};
Texture2D<float4> native_texture : register(t4);
Texture2D<float4> native_source : register(t5);
SamplerState native_replace_sampler : register(s1);   // texture packs (flag 2048)

struct NativeVaryings {
    float4 position : SV_Position;
    noperspective float4 stqf : TEXCOORD0;
    noperspective float4 color : COLOR0;
};

NativeVaryings native_vertex(float3 position : POSITION, float4 stqf : TEXCOORD0, uint4 color : COLOR0) {
    NativeVaryings o;
    // Upscaled sprites (flag 1024) cover whole GS pixels, as at 1x: corners move to the edge of
    // the first pixel they cover (x + 0.5 is stored). Otherwise the half pixel left of a sprite at
    // an integer x stays undrawn, a line of stale pixels along full-screen passes (PCSX2's
    // "align sprite" problem).
    if ((native_flags & 1024) && (native_scale & 255) > 1) position.xy = ceil(position.xy - 0.5);
    o.position = float4(position.xy / native_target * float2(2, -2) + float2(-1, 1), position.z, 1);
    o.stqf = stqf;
    o.color = (float4)color;
    return o;
}

// CLAMP modes: repeat, clamp, region clamp, region repeat (MINU is the mask, MAXU the fix).
uint native_wrap_coordinate(int c, uint mode, uint size, uint low, uint high) {
    if (mode == 0) return (uint)c & (size - 1);
    if (mode == 1) return (uint)clamp(c, 0, (int)size - 1);
    if (mode == 2) return (uint)clamp(c, (int)low, (int)high);
    return ((uint)c & low) | high;
}

// c counts sub-texels, E to a GS texel. Wrapping works on whole GS texels and keeps the sub-texel;
// a texture at scale S read with E = 1 (into a 1x target) takes the middle of each texel's block.
float4 native_texel(int2 c) {
    uint S = native_scale >> 8 & 255, E = native_scale >> 16 & 255, step = S / E;
    int2 whole = (int2)floor((float2)c / E), sub = c - whole * (int)E;
    uint2 at = uint2(native_wrap_coordinate(whole.x, native_wrap.x, native_tex_size.x, native_region.x, native_region.y),
                     native_wrap_coordinate(whole.y, native_wrap.y, native_tex_size.y, native_region.z, native_region.w));
    int2 xy = (native_tex_offset + (int2)at) * (int)S + sub * (int)step + (int)(step / 2);
    float4 t = native_texture.Load(int3(xy, 0));
    t.rgb *= 255;
    t.a = native_flags & 256 ? native_texa_alpha : t.a * native_alpha_scale;
    return t;
}

// A replacement from a texture pack (texpack.c, flag 2048) in native_texture: any size, with
// mipmaps. It covers the same normalised texture space as the GS texture (as PCSX2 maps one) and
// the sampler filters it; region clamp stays inside the region's replacement texels.
float4 native_replacement(float2 uv, float2 across, float2 down) {
    float2 size, gs_size = (float2)native_tex_size;
    native_texture.GetDimensions(size.x, size.y);
    float2 half_texel = gs_size / size * 0.5;
    if (native_wrap.x == 2) uv.x = clamp(uv.x, native_region.x + half_texel.x, native_region.y + 1 - half_texel.x);
    if (native_wrap.y == 2) uv.y = clamp(uv.y, native_region.z + half_texel.y, native_region.w + 1 - half_texel.y);
    float4 t = native_texture.SampleGrad(native_replace_sampler, uv / gs_size, across / gs_size, down / gs_size);
    t.rgb *= 255;
    t.a *= native_alpha_scale;
    return t;
}

// flags: 1 textured, 2 TCC, 4-8 TFX, 16 fog, 32 bilinear, 64 alpha test, 128 keep the
// pixels that fail it (second pass of AFAIL), 256 texture alpha from TEXA (24-bit source),
// 512 colour premultiplied by alpha (Cs * As blending, where As can exceed 1), 1024 a sprite,
// 2048 a texture pack's replacement.
// Targets keep the whole GS alpha (0..255 as a fraction of 255, so a texture drawn with alpha 0xFF
// still reads 0xFF: the rider shadow); the blender takes As / 128 from the second output
// (dual-source blending, SRC1_ALPHA).
struct NativeOutput { float4 color : SV_Target0; float4 blend : SV_Target1; };
NativeOutput native_pixel(NativeVaryings i) {
    float4 color = i.color;
    float q = abs(i.stqf.z) > 1e-30 ? i.stqf.z : 1e-30;
    float2 uv = i.stqf.xy / q * (float2)native_tex_size;
    float2 across = ddx(uv), down = ddy(uv);
    if (native_flags & 1) {
        float4 t;
        if (native_flags & 2048) t = native_replacement(uv, across, down);
        else {
            uint T = native_scale & 255, E = native_scale >> 16 & 255;
            // An upscaled sprite of a 1x texture samples where the GS samples its pixel, the corner
            // of the pixel's block: the HUD, text and screen passes look as at 1x, with no seams
            // between tiles and nothing read past their texels (PCSX2's "round sprite"). A sprite's
            // texture coordinates are affine, so the derivatives give them there.
            if ((native_flags & 1024) && T > 1 && E == 1) {
                float2 corner = floor(i.position.xy / T) * T - i.position.xy;
                uv += across * corner.x + down * corner.y;
            }
            uv *= E;
            if (native_flags & 32) {
                uv -= 0.5;
                int2 b = (int2)floor(uv);
                float2 f = uv - floor(uv);
                t = lerp(lerp(native_texel(b), native_texel(b + int2(1, 0)), f.x),
                         lerp(native_texel(b + int2(0, 1)), native_texel(b + int2(1, 1)), f.x), f.y);
            } else t = native_texel((int2)floor(uv));
        }
        uint tfx = (native_flags >> 2) & 3;
        float3 rgb = tfx == 1 ? t.rgb : min(floor(t.rgb * color.rgb / 128), 255);
        if (tfx >= 2) rgb = min(rgb + color.a, 255);
        float a = color.a;
        if (native_flags & 2) a = tfx == 0 ? min(floor(t.a * color.a / 128), 255) : tfx == 2 ? min(t.a + color.a, 255) : t.a;
        color = float4(rgb, a);
    }
    if (native_flags & 16) color.rgb = floor(lerp(native_fog.rgb, color.rgb, i.stqf.w));
    if (native_flags & 64) {
        uint a = (uint)color.a, r = native_aref, t = native_atst;
        bool passed = t == 1 || (t == 2 && a < r) || (t == 3 && a <= r) || (t == 4 && a == r) ||
                    (t == 5 && a >= r) || (t == 6 && a > r) || (t == 7 && a != r);
        if (passed == ((native_flags & 128) != 0)) discard;
    }
    if (native_flags & 512) color.rgb = floor(color.rgb * color.a / 128);   // premultiplied Cs * As
    NativeOutput o;
    o.color = float4(color.rgb / 255, saturate(color.a / 255));
    o.blend = float4(0, 0, 0, saturate(color.a / 128));
    return o;
}

// The display circuit from a render target, with the same field-line doubling as scanout. At scale
// S (display_state[2].x) the picture is S times larger: b.y and b.z are already scaled.
[numthreads(8,8,1)]
void native_scanout(uint3 tid : SV_DispatchThreadID) {
    uint4 a = display_state[0], b = display_state[1];
    uint S = display_state[2].x;
    if (tid.x >= b.y || tid.y >= b.z) return;
    uint2 at = uint2(tid.x, tid.y >> b.w);
    uint2 xy = ((uint2(a.w, b.x) + at / S) & 2047) * S + at % S;
    screen[tid.xy] = float4(native_source.Load(int3(xy, 0)).rgb, 1);
}

// A rectangle of one surface into another of a different scale (gpu_native.inc, native_blit): the
// viewport is the destination; each pixel reads the source pixel under its centre, so a 1x source
// is repeated and an upscaled one read at the middle of each block.
cbuffer NativeBlit : register(b5) { float4 blit_map; };   // source = destination / T + xy, times S
Texture2D<float4> blit_source : register(t7);
float4 native_blit(float4 position : SV_Position) : SV_Target {
    return blit_source.Load(int3((int2)floor((position.xy / blit_map.z + blit_map.xy) * blit_map.w), 0));
}

// R holds uploaded alpha bits, G their mask; the scaled RGB and remaining alpha
// bits come from an unchanged copy of this page of the target.
float4 native_upload_alpha(float4 position : SV_Position) : SV_Target {
    int2 local = (int2)floor(position.xy / blit_map.z - blit_map.xy);
    uint2 upload = (uint2)round(blit_source.Load(int3(local, 0)).rg * 255);
    float4 previous = native_source.Load(int3((int2)position.xy, 0));
    uint alpha = (uint)round(previous.a * 255);
    return float4(previous.rgb, ((alpha & ~upload.y) | upload.x) / 255.0);
}

// A texture over GS memory some of which native surfaces drew, in a layout the draw can't sample
// from a surface directly (gpu_native.inc, native_convert_texture): a 32-bit target read as
// 16-bit, frame buffer alpha read as a 4-bit texture, a texture page partly drawn over. Each texel
// takes its GS address; its page says where that memory is: the CPU copy (convert_base, the
// texture decoded as usual) or a surface (native_inverse: word in page to pixel in page, colour
// then Z layout), whose value the GS would read. Output as decoded textures are.
cbuffer NativeConvert : register(b4) {
    uint4 convert_tex, convert_size;
    uint4 convert_slot[4];
    uint4 convert_pages[32];
    uint4 convert_palette[64];
};
StructuredBuffer<uint> native_inverse : register(t6);
Texture2D<float4> convert_base : register(t8);
Texture2D<float4> convert_colour0 : register(t9);
Texture2D<float4> convert_colour1 : register(t10);
Texture2D<float4> convert_colour2 : register(t11);
Texture2D<float4> convert_colour3 : register(t12);
Texture2D<float> convert_depth0 : register(t13);
Texture2D<float> convert_depth1 : register(t14);
Texture2D<float> convert_depth2 : register(t15);
Texture2D<float> convert_depth3 : register(t16);
RWTexture2D<float4> native_converted : register(u2);

uint convert_word(uint slot, int3 xy, bool depth, float unit) {
    float4 c = 0; float d = 0;
    if (depth) {
        if (slot == 0) d = convert_depth0.Load(xy); else if (slot == 1) d = convert_depth1.Load(xy);
        else if (slot == 2) d = convert_depth2.Load(xy); else d = convert_depth3.Load(xy);
        return d >= 1 ? 0xFFFFFFFF : (uint)(d * unit);
    }
    if (slot == 0) c = convert_colour0.Load(xy); else if (slot == 1) c = convert_colour1.Load(xy);
    else if (slot == 2) c = convert_colour2.Load(xy); else c = convert_colour3.Load(xy);
    uint4 u = uint4(round(c * 255));
    return u.r | u.g << 8 | u.b << 16 | u.a << 24;
}

[numthreads(8,8,1)]
void native_convert(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= convert_size.x || tid.y >= convert_size.y) return;
    uint psm = convert_tex.x, a = address(psm, convert_tex.y, convert_tex.z, tid.x, tid.y);
    uint word = a >> 5, page = (word >> 11) & 511;
    uint source = (convert_pages[page >> 4][(page >> 2) & 3] >> ((page & 3) * 8)) & 255;
    if (!source) { native_converted[tid.xy] = convert_base.Load(int3(tid.xy, 0)); return; }
    uint4 s = convert_slot[source - 1];
    uint rel = (page - s.x) & 511, bw = s.y;
    uint at = native_inverse[(s.z ? 2048 : 0) + (word & 2047)];
    // An upscaled surface: the middle of the pixel's block (convert_tex.w, a byte per slot).
    uint scale = convert_tex.w >> ((source - 1) * 8) & 255;
    int3 xy = int3(((rel % bw) * 64 + (at & 255)) * scale + scale / 2, ((rel / bw) * 32 + (at >> 8)) * scale + scale / 2, 0);
    uint value = convert_word(source - 1, xy, s.z != 0, asfloat(s.w));
    uint bits = bits_per_pixel(psm), texa = convert_size.z;
    value = (value >> (a & 31)) & (bits == 32 ? 0xFFFFFFFF : (1u << bits) - 1);
    uint rgba = value;
    if (bits <= 8) rgba = convert_palette[value >> 2][value & 3];
    else if ((psm & 7) == 2) {
        uint alpha = value & 0x8000 ? convert_size.w & 255 : texa & 255;
        if ((texa & 0x8000) && value == 0) alpha = 0;
        rgba = (value & 31) << 3 | (value & 0x3E0) << 6 | (value & 0x7C00) << 9 | alpha << 24;
    } else if ((psm & 7) == 1) rgba = (value & 0xFFFFFF) | ((texa & 0x8000) && !(value & 0xFFFFFF) ? 0 : (texa & 255) << 24);
    native_converted[tid.xy] = float4(rgba & 255, rgba >> 8 & 255, rgba >> 16 & 255, rgba >> 24) / 255;
}

// Presentation (gpu_native.inc, native_present): the scanned-out picture drawn into the window's
// back buffer, filtered, in the viewport that keeps its shape. One triangle covers the viewport.
Texture2D<float4> present_source : register(t0);
SamplerState present_sampler : register(s0);
struct PresentVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
PresentVertex present_vertex(uint id : SV_VertexID) {
    PresentVertex o;
    float2 p = float2(id == 1 ? 3 : -1, id == 2 ? -3 : 1);
    o.position = float4(p, 0, 1);
    o.uv = float2(p.x + 1, 1 - p.y) * 0.5;
    return o;
}
float4 present_pixel(PresentVertex i) : SV_Target {
    return float4(present_source.Sample(present_sampler, i.uv).rgb, 1);
}
#endif
