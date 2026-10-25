/* Direct3D 11 on Metal for the module's renderers (finding 075): the host side of
   port/mac/include/mac_gpu.h. port/mac/d3d11_mac.c turns the D3D11 calls into these; here they
   become Metal commands, in order, on one queue.

   The context's bindings are kept here and applied at each draw and dispatch (Metal binds per
   encoder). Encoders follow the work: a render pass for its targets until they change, a compute
   or blit encoder in between. Buffers live in shared memory (one memory on Apple GPUs); updates
   are blits from a copy of the data, ordered with the work around them, except a dynamic
   resource's whole update, which gets new storage (D3D11's WRITE_DISCARD renaming). Reads wait
   for the work so far.

   Resource bindings follow port/tools/metal_shaders.py: b# buffer #, t# buffer or texture
   TEXTURE_SRV + #, u# buffer or texture TEXTURE_UAV + #, s# sampler #; the vertex buffer is
   buffer 30. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <execinfo.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include "native.h"
#include "shaders.h"  /* build/port-mac/shaders (port/tools/metal_shaders.py) */

struct w2c_gpu;

enum { MAX_OBJECTS = 1 << 16, SLOTS = 24, VERTEX_BUFFER_INDEX = 30 };
enum { O_FREE, O_BUFFER, O_TEXTURE, O_VIEW, O_SHADER, O_LAYOUT, O_BLEND, O_DEPTH, O_RASTER, O_SAMPLER, O_QUERY };
enum { SRV = 1, UAV, RTV, DSV };
enum { VS, PS, CS };
enum { CBV, SRV_SLOT, UAV_SLOT, SAMPLER_SLOT };
enum { S_LAYOUT = 1, S_TOPOLOGY, S_RASTER, S_BLEND, S_DEPTH, S_VERTEX_BUFFER };
/* D3D11 values (port/mac/include/d3d11.h) */
enum { USAGE_DYNAMIC = 2, USAGE_STAGING = 3, BIND_RTV = 0x20, BIND_DSV = 0x40, BIND_UAV = 0x80, CPU_READ = 0x20000 };
enum { FMT_RGBA32F = 2, FMT_RGBA32U = 3, FMT_RGB32F = 6, FMT_RGBA8 = 28, FMT_RGBA8U = 30, FMT_R32_TYPELESS = 39,
       FMT_D32F = 40, FMT_R32F = 41, FMT_R32U = 42, FMT_BGRA8 = 87 };

typedef struct {
    int kind;
    uint32_t refs;                                       /* the module's handle, and each view of it */
    __strong id<MTLBuffer> buffer;
    __strong id<MTLTexture> texture;
    uint32_t bytes, width, height, format, usage, bind, stride, cpu;
    uint32_t resource, view, first, count, raw;           /* a view */
    __strong id<MTLFunction> function;                   /* a shader */
    __strong id<MTLComputePipelineState> compute;
    uint32_t threads[3];
    char name[48];
    uint32_t elements, element_format[16], element_offset[16];  /* a layout */
    uint32_t blend[8];                                   /* a blend state */
    __strong id<MTLDepthStencilState> depth;             /* a depth state */
    uint32_t cull, scissor, clip;                        /* a rasterizer state */
    __strong id<MTLSamplerState> sampler;                /* a sampler */
    uint32_t query;                                      /* a query: its kind, the work it waits for */
    uint64_t serial, value;
} Object;

static Object objects[MAX_OBJECTS];
static uint32_t next_free = 1;
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLLibrary> library;
static id<MTLCommandBuffer> command, last_committed;
static id<MTLRenderCommandEncoder> render;
static id<MTLComputeCommandEncoder> compute;
static id<MTLBlitCommandEncoder> blit;
static uint64_t serial;                  /* of the command buffer being recorded */
static _Atomic uint64_t completed;       /* the last command buffer the GPU finished */
static NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *pipelines;
static id<MTLDepthStencilState> no_depth;

/* the context's state */
static uint32_t shader[3], bound[3][4][SLOTS], layout, topology = 4, raster, blend_state, depth_state, stencil_ref;
static uint32_t vertex_buffer, vertex_stride, vertex_offset, target, depth_target;
static uint32_t pass_target, pass_depth;  /* the render encoder's attachments */
static float blend_factor[4] = {1, 1, 1, 1};
static MTLViewport viewport;
static MTLScissorRect scissor;

/* ---- objects ---- */

static uint32_t new_object(int kind)
{
    for (uint32_t n = 1; n < MAX_OBJECTS; n++) {
        uint32_t i = next_free++;
        if (next_free == MAX_OBJECTS) next_free = 1;
        if (objects[i].kind == O_FREE) {
            objects[i] = (Object){0};
            objects[i].kind = kind;
            objects[i].refs = 1;
            return i;
        }
    }
    fprintf(stderr, "metal: out of objects\n");
    abort();
}

static Object *get(uint32_t h) { return h && h < MAX_OBJECTS && objects[h].kind ? &objects[h] : NULL; }

/* The resource a handle names: itself, or the resource of its view. */
static Object *resource_of(uint32_t h)
{
    Object *o = get(h);
    return o && o->kind == O_VIEW ? get(o->resource) : o;
}

/* ---- the command stream ---- */

static void end_encoder(void)
{
    if (render) [render endEncoding], render = nil, pass_target = pass_depth = 0;
    if (compute) [compute endEncoding], compute = nil;
    if (blit) [blit endEncoding], blit = nil;
}

static void begin_command(void)
{
    if (command) return;
    command = [queue commandBuffer];
    uint64_t mine = ++serial;
    [command addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        uint64_t seen = atomic_load(&completed);
        while (seen < mine && !atomic_compare_exchange_weak(&completed, &seen, mine)) {}
    }];
}

static void commit(void)
{
    end_encoder();
    if (!command) return;
    [command commit];
    last_committed = command;
    command = nil;
}

static void wait_idle(void)
{
    commit();
    if (last_committed) [last_committed waitUntilCompleted];
}

static id<MTLBlitCommandEncoder> blitter(void)
{
    if (blit) return blit;
    end_encoder();
    begin_command();
    return blit = [command blitCommandEncoder];
}

/* ---- formats ---- */

static MTLPixelFormat pixel_format(uint32_t f, uint32_t bind)
{
    switch (f) {
    case FMT_RGBA8: return MTLPixelFormatRGBA8Unorm;
    case FMT_BGRA8: return MTLPixelFormatBGRA8Unorm;
    case FMT_RGBA8U: return MTLPixelFormatRGBA8Uint;
    case FMT_RGBA32F: return MTLPixelFormatRGBA32Float;
    case FMT_RGBA32U: return MTLPixelFormatRGBA32Uint;
    case FMT_R32F: return MTLPixelFormatR32Float;
    case FMT_R32U: return MTLPixelFormatR32Uint;
    case FMT_D32F: return MTLPixelFormatDepth32Float;
    case FMT_R32_TYPELESS: return bind & BIND_DSV ? MTLPixelFormatDepth32Float : MTLPixelFormatR32Float;
    default: return MTLPixelFormatRGBA8Unorm;
    }
}

static unsigned pixel_bytes(MTLPixelFormat f)
{
    return f == MTLPixelFormatRGBA32Float || f == MTLPixelFormatRGBA32Uint ? 16 : 4;
}

static MTLVertexFormat vertex_format(uint32_t f)
{
    switch (f) {
    case FMT_RGB32F: return MTLVertexFormatFloat3;
    case FMT_RGBA32F: return MTLVertexFormatFloat4;
    case FMT_RGBA8U: return MTLVertexFormatUChar4;
    case FMT_RGBA8: return MTLVertexFormatUChar4Normalized;
    case FMT_R32F: return MTLVertexFormatFloat;
    case FMT_R32U: return MTLVertexFormatUInt;
    default: return MTLVertexFormatFloat4;
    }
}

/* ---- creation ---- */

static void setup(void)
{
    if (device) return;
    device = MTLCreateSystemDefaultDevice();
    queue = [device newCommandQueue];
    NSError *error = nil;
    dispatch_data_t data = dispatch_data_create(shader_library, sizeof shader_library, NULL,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    library = [device newLibraryWithData:data error:&error];
    if (!library) {
        fprintf(stderr, "metal: the shader library didn't load: %s\n", error.localizedDescription.UTF8String);
        abort();
    }
    pipelines = [NSMutableDictionary dictionary];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionAlways;
    d.depthWriteEnabled = NO;
    no_depth = [device newDepthStencilStateWithDescriptor:d];
    fprintf(stderr, "gs: Metal device %s\n", device.name.UTF8String);
}

id<MTLDevice> metal_device(void)
{
    pthread_mutex_lock(&lock);
    setup();
    pthread_mutex_unlock(&lock);
    return device;
}

/* Every call under the one lock (the module's threads share the context, as D3D11's callers do);
   a break in body leaves body, not the lock. */
void check_stack(const char *where);
#define LOCKED(...) do { check_stack(__func__); pthread_mutex_lock(&lock); setup(); do { __VA_ARGS__; } while (0); pthread_mutex_unlock(&lock); } while (0)

u32 w2c_gpu_buffer(struct w2c_gpu *unused, u32 bytes, u32 bind, u32 usage, u32 misc, u32 stride, u32 init)
{
    (void)unused; (void)misc;
    uint32_t h;
    LOCKED({
        h = new_object(O_BUFFER);
        Object *o = &objects[h];
        NSUInteger size = bytes ? bytes : 16;
        o->buffer = init ? [device newBufferWithBytes:at(init) length:size options:MTLResourceStorageModeShared]
                         : [device newBufferWithLength:size options:MTLResourceStorageModeShared];
        o->bytes = bytes; o->bind = bind; o->usage = usage; o->stride = stride;
    });
    return h;
}

u32 w2c_gpu_texture(struct w2c_gpu *unused, u32 width, u32 height, u32 format, u32 bind, u32 usage, u32 cpu, u32 mips,
                    u32 init)
{
    (void)unused;
    uint32_t h = 0;
    if (!width || !height || width > 16384 || height > 16384) {
        fprintf(stderr, "metal: no %ux%u texture (format %u)\n", width, height, format);
        static int shown;
        if (!shown++) { void *f[24]; backtrace_symbols_fd(f, backtrace(f, 24), 2); }
        return 0;
    }
    LOCKED({
        h = new_object(O_TEXTURE);
        Object *o = &objects[h];
        MTLPixelFormat pf = pixel_format(format, bind);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:width
                                                                                    height:height mipmapped:mips > 1];
        d.mipmapLevelCount = mips ? mips : 1;
        d.usage = MTLTextureUsageShaderRead;
        if (bind & (BIND_RTV | BIND_DSV)) d.usage |= MTLTextureUsageRenderTarget;
        if (bind & BIND_UAV) d.usage |= MTLTextureUsageShaderWrite;
        d.storageMode = (cpu & CPU_READ) || usage == USAGE_STAGING ? MTLStorageModeShared : MTLStorageModePrivate;
        if (pf == MTLPixelFormatDepth32Float) d.storageMode = MTLStorageModePrivate;
        o->texture = [device newTextureWithDescriptor:d];
        o->width = width; o->height = height; o->format = format; o->bind = bind; o->usage = usage; o->cpu = cpu;
        o->bytes = width * height * pixel_bytes(pf);
        for (uint32_t level = 0; init && level < (mips ? mips : 1); level++) {
            uint32_t data, pitch;  /* D3D11_SUBRESOURCE_DATA on wasm32: {pSysMem, SysMemPitch, SysMemSlicePitch} */
            memcpy(&data, at(init + level * 12), 4);
            memcpy(&pitch, at(init + level * 12 + 4), 4);
            uint32_t w = width >> level ? width >> level : 1, ht = height >> level ? height >> level : 1;
            id<MTLBuffer> staging = [device newBufferWithBytes:at(data) length:(NSUInteger)pitch * ht
                                                       options:MTLResourceStorageModeShared];
            [blitter() copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch * ht
                           sourceSize:MTLSizeMake(w, ht, 1) toTexture:o->texture destinationSlice:0
                     destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    });
    return h;
}

u32 w2c_gpu_view(struct w2c_gpu *unused, u32 kind, u32 resource, u32 format, u32 first, u32 count, u32 raw)
{
    (void)unused;
    uint32_t h;
    LOCKED({
        if (!get(resource)) { h = 0; break; }
        h = new_object(O_VIEW);
        Object *o = &objects[h];
        o->view = kind; o->resource = resource; o->format = format; o->first = first; o->count = count; o->raw = raw;
        objects[resource].refs++;  /* a view keeps its resource, as in D3D11 */
    });
    return h;
}

u32 w2c_gpu_shader(struct w2c_gpu *unused, u32 entry)
{
    (void)unused;
    uint32_t h = 0;
    LOCKED({
        for (unsigned i = 0; i < sizeof shader_entries / sizeof *shader_entries; i++) {
            if (strcmp(shader_entries[i].entry, string_at(entry))) continue;
            id<MTLFunction> f = [library newFunctionWithName:@(shader_entries[i].name)];
            if (!f) break;
            h = new_object(O_SHADER);
            objects[h].function = f;
            memcpy(objects[h].threads, shader_entries[i].threads, sizeof objects[h].threads);
            snprintf(objects[h].name, sizeof objects[h].name, "%s", shader_entries[i].name);
            break;
        }
    });
    return h;
}

u32 w2c_gpu_layout(struct w2c_gpu *unused, u32 count, u32 elements)
{
    (void)unused;
    uint32_t h;
    LOCKED({
        h = new_object(O_LAYOUT);
        Object *o = &objects[h];
        o->elements = count < 16 ? count : 16;
        for (uint32_t i = 0; i < o->elements; i++) {
            memcpy(&o->element_format[i], at(elements + i * 8), 4);
            memcpy(&o->element_offset[i], at(elements + i * 8 + 4), 4);
        }
    });
    return h;
}

u32 w2c_gpu_blend(struct w2c_gpu *unused, u32 desc)
{
    (void)unused;
    uint32_t h;
    LOCKED({ h = new_object(O_BLEND); memcpy(objects[h].blend, at(desc), sizeof objects[h].blend); });
    return h;
}

static MTLCompareFunction compare(uint32_t f)
{
    static const MTLCompareFunction map[9] = {MTLCompareFunctionAlways, MTLCompareFunctionNever,
        MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,
        MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return map[f < 9 ? f : 0];
}

u32 w2c_gpu_depth(struct w2c_gpu *unused, u32 enable, u32 write, u32 func)
{
    (void)unused;
    uint32_t h;
    LOCKED({
        h = new_object(O_DEPTH);
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = enable ? compare(func) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = enable && write;
        objects[h].depth = [device newDepthStencilStateWithDescriptor:d];
    });
    return h;
}

u32 w2c_gpu_raster(struct w2c_gpu *unused, u32 cull, u32 scissor_enable, u32 clip)
{
    (void)unused;
    uint32_t h;
    LOCKED({ h = new_object(O_RASTER); objects[h].cull = cull; objects[h].scissor = scissor_enable; objects[h].clip = clip; });
    return h;
}

static MTLSamplerAddressMode address(uint32_t m)
{
    return m == 1 ? MTLSamplerAddressModeRepeat : m == 2 ? MTLSamplerAddressModeMirrorRepeat
         : MTLSamplerAddressModeClampToEdge;
}

u32 w2c_gpu_sampler(struct w2c_gpu *unused, u32 filter, u32 u, u32 v, u32 w, u32 anisotropy)
{
    (void)unused;
    uint32_t h;
    LOCKED({
        h = new_object(O_SAMPLER);
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        int linear_min = (filter & 0x10) != 0, linear_mag = (filter & 0x4) != 0, linear_mip = (filter & 0x1) != 0;
        d.minFilter = linear_min ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.magFilter = linear_mag ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = linear_mip ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        if (filter == 0x55) d.maxAnisotropy = anisotropy ? anisotropy : 1,
            d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear, d.mipFilter = MTLSamplerMipFilterLinear;
        d.sAddressMode = address(u); d.tAddressMode = address(v); d.rAddressMode = address(w);
        objects[h].sampler = [device newSamplerStateWithDescriptor:d];
    });
    return h;
}

u32 w2c_gpu_query(struct w2c_gpu *unused, u32 kind)
{
    (void)unused;
    uint32_t h;
    LOCKED({ h = new_object(O_QUERY); objects[h].query = kind; });
    return h;
}

static void release(uint32_t h)
{
    Object *o = get(h);
    if (!o || --o->refs) return;
    uint32_t resource = o->kind == O_VIEW ? o->resource : 0;
    *o = (Object){0};
    if (resource) release(resource);
}

void w2c_gpu_free(struct w2c_gpu *unused, u32 h)
{
    (void)unused;
    LOCKED(release(h));
}

/* ---- the context's state ---- */

void w2c_gpu_set_shader(struct w2c_gpu *unused, u32 stage, u32 h)
{
    (void)unused;
    LOCKED({ if (stage < 3) shader[stage] = h; });
}

void w2c_gpu_bind(struct w2c_gpu *unused, u32 stage, u32 kind, u32 slot, u32 h)
{
    (void)unused;
    LOCKED({ if (stage < 3 && kind < 4 && slot < SLOTS) bound[stage][kind][slot] = h; });
}

void w2c_gpu_state(struct w2c_gpu *unused, u32 what, u32 h, u32 a, u32 b)
{
    (void)unused;
    LOCKED({
        switch (what) {
        case S_LAYOUT: layout = h; break;
        case S_TOPOLOGY: topology = a; break;
        case S_RASTER: raster = h; break;
        case S_BLEND: blend_state = h; break;
        case S_DEPTH: depth_state = h; stencil_ref = a; break;
        case S_VERTEX_BUFFER: vertex_buffer = h; vertex_stride = a; vertex_offset = b; break;
        }
    });
}

void w2c_gpu_blend_factor(struct w2c_gpu *unused, f32 r, f32 g, f32 b, f32 a)
{
    (void)unused;
    LOCKED({ blend_factor[0] = r; blend_factor[1] = g; blend_factor[2] = b; blend_factor[3] = a; });
}

void w2c_gpu_viewport(struct w2c_gpu *unused, f32 x, f32 y, f32 w, f32 h, f32 znear, f32 zfar)
{
    (void)unused;
    LOCKED({ viewport = (MTLViewport){x, y, w, h, znear, zfar}; });
}

void w2c_gpu_scissor(struct w2c_gpu *unused, u32 left, u32 top, u32 right, u32 bottom)
{
    (void)unused;
    LOCKED({
        int32_t l = (int32_t)left, t = (int32_t)top, r = (int32_t)right, b = (int32_t)bottom;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        scissor = (MTLScissorRect){(NSUInteger)l, (NSUInteger)t, r > l ? (NSUInteger)(r - l) : 0, b > t ? (NSUInteger)(b - t) : 0};
    });
}

void w2c_gpu_targets(struct w2c_gpu *unused, u32 rtv, u32 dsv)
{
    (void)unused;
    LOCKED({ target = rtv; depth_target = dsv; });
}

/* ---- binding resources to an encoder ---- */

typedef void (^BindBuffer)(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger index);
typedef void (^BindTexture)(id<MTLTexture> texture, NSUInteger index);
typedef void (^BindSampler)(id<MTLSamplerState> sampler, NSUInteger index);

static void bind_stage(int stage, BindBuffer buffer, BindTexture texture, BindSampler sampler)
{
    for (unsigned slot = 0; slot < SLOTS; slot++) {
        Object *c = get(bound[stage][CBV][slot]);
        if (c && c->buffer && slot < TEXTURE_SRV) buffer(c->buffer, 0, slot);
        for (int kind = SRV_SLOT; kind <= UAV_SLOT; kind++) {
            Object *v = get(bound[stage][kind][slot]);
            Object *r = v ? resource_of(bound[stage][kind][slot]) : NULL;
            NSUInteger index = (kind == SRV_SLOT ? TEXTURE_SRV : TEXTURE_UAV) + slot;
            if (!r) continue;
            if (r->texture) texture(r->texture, index);
            else if (r->buffer && index < VERTEX_BUFFER_INDEX) {
                NSUInteger element = v->kind == O_VIEW && !v->raw && r->stride ? r->stride : 4;
                buffer(r->buffer, v->kind == O_VIEW ? v->first * element : 0, index);
            }
        }
        Object *s = get(bound[stage][SAMPLER_SLOT][slot]);
        if (s && s->sampler && slot < 16) sampler(s->sampler, slot);
    }
}

/* ---- drawing ---- */

static MTLBlendFactor blend_factor_of(uint32_t b)
{
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    case 16: return MTLBlendFactorSource1Color;
    case 17: return MTLBlendFactorOneMinusSource1Color;
    case 18: return MTLBlendFactorSource1Alpha;
    case 19: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    static const MTLBlendOperation map[6] = {MTLBlendOperationAdd, MTLBlendOperationAdd, MTLBlendOperationSubtract,
        MTLBlendOperationReverseSubtract, MTLBlendOperationMin, MTLBlendOperationMax};
    return map[op < 6 ? op : 0];
}

static id<MTLRenderPipelineState> render_pipeline(MTLPixelFormat color, MTLPixelFormat depth)
{
    Object *vs = get(shader[VS]), *ps = get(shader[PS]), *l = get(layout), *b = get(blend_state);
    NSString *key = [NSString stringWithFormat:@"%u/%u/%u/%u/%lu/%lu/%u", shader[VS], shader[PS], layout,
                     blend_state, (unsigned long)color, (unsigned long)depth, l ? vertex_stride : 0];
    id<MTLRenderPipelineState> p = pipelines[key];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs ? vs->function : nil;
    d.fragmentFunction = ps ? ps->function : nil;
    if (l) {
        MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < l->elements; i++) {
            v.attributes[i].format = vertex_format(l->element_format[i]);
            v.attributes[i].offset = l->element_offset[i];
            v.attributes[i].bufferIndex = VERTEX_BUFFER_INDEX;
        }
        v.layouts[VERTEX_BUFFER_INDEX].stride = vertex_stride ? vertex_stride : 16;
        d.vertexDescriptor = v;
    }
    d.colorAttachments[0].pixelFormat = color;
    d.depthAttachmentPixelFormat = depth;
    if (b && color != MTLPixelFormatInvalid) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
        c.blendingEnabled = b->blend[0] != 0;
        c.sourceRGBBlendFactor = blend_factor_of(b->blend[1]);
        c.destinationRGBBlendFactor = blend_factor_of(b->blend[2]);
        c.rgbBlendOperation = blend_op(b->blend[3]);
        c.sourceAlphaBlendFactor = blend_factor_of(b->blend[4]);
        c.destinationAlphaBlendFactor = blend_factor_of(b->blend[5]);
        c.alphaBlendOperation = blend_op(b->blend[6]);
        c.writeMask = (b->blend[7] & 1 ? MTLColorWriteMaskRed : 0) | (b->blend[7] & 2 ? MTLColorWriteMaskGreen : 0) |
                      (b->blend[7] & 4 ? MTLColorWriteMaskBlue : 0) | (b->blend[7] & 8 ? MTLColorWriteMaskAlpha : 0);
    }
    NSError *error = nil;
    p = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!p) {
        fprintf(stderr, "metal: render pipeline %s/%s: %s\n", vs ? vs->name : "-", ps ? ps->name : "-",
                error.localizedDescription.UTF8String);
        return nil;
    }
    pipelines[key] = p;
    return p;
}

/* The render pass for the bound targets, continued while they stay. */
static id<MTLRenderCommandEncoder> render_pass(void)
{
    if (render && pass_target == target && pass_depth == depth_target) return render;
    end_encoder();
    begin_command();
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    Object *t = resource_of(target), *z = resource_of(depth_target);
    if (t) {
        d.colorAttachments[0].texture = t->texture;
        d.colorAttachments[0].loadAction = MTLLoadActionLoad;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (z) {
        d.depthAttachment.texture = z->texture;
        d.depthAttachment.loadAction = MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
    }
    if (!t && !z) return nil;
    render = [command renderCommandEncoderWithDescriptor:d];
    pass_target = target; pass_depth = depth_target;
    return render;
}

void w2c_gpu_draw(struct w2c_gpu *unused, u32 count, u32 start)
{
    (void)unused;
    LOCKED({
        id<MTLRenderCommandEncoder> e = render_pass();
        Object *t = resource_of(target), *z = resource_of(depth_target);
        if (!e) break;
        id<MTLRenderPipelineState> p = render_pipeline(t ? t->texture.pixelFormat : MTLPixelFormatInvalid,
                                                       z ? z->texture.pixelFormat : MTLPixelFormatInvalid);
        if (!p) break;
        [e setRenderPipelineState:p];
        Object *ds = get(depth_state), *r = get(raster);
        [e setDepthStencilState:ds && z ? ds->depth : no_depth];
        [e setBlendColorRed:blend_factor[0] green:blend_factor[1] blue:blend_factor[2] alpha:blend_factor[3]];
        [e setViewport:viewport];
        NSUInteger tw = t ? t->texture.width : z->texture.width, th = t ? t->texture.height : z->texture.height;
        MTLScissorRect s = r && r->scissor ? scissor : (MTLScissorRect){0, 0, tw, th};
        if (s.x >= tw || s.y >= th) break;
        if (s.x + s.width > tw) s.width = tw - s.x;
        if (s.y + s.height > th) s.height = th - s.y;
        if (!s.width || !s.height) break;
        [e setScissorRect:s];
        [e setCullMode:MTLCullModeNone];
        if (r && !r->clip) [e setDepthClipMode:MTLDepthClipModeClamp];
        bind_stage(VS, ^(id<MTLBuffer> b, NSUInteger o, NSUInteger i) { [e setVertexBuffer:b offset:o atIndex:i]; },
                   ^(id<MTLTexture> x, NSUInteger i) { [e setVertexTexture:x atIndex:i]; },
                   ^(id<MTLSamplerState> x, NSUInteger i) { [e setVertexSamplerState:x atIndex:i]; });
        bind_stage(PS, ^(id<MTLBuffer> b, NSUInteger o, NSUInteger i) { [e setFragmentBuffer:b offset:o atIndex:i]; },
                   ^(id<MTLTexture> x, NSUInteger i) { [e setFragmentTexture:x atIndex:i]; },
                   ^(id<MTLSamplerState> x, NSUInteger i) { [e setFragmentSamplerState:x atIndex:i]; });
        Object *vb = get(vertex_buffer);
        if (vb && vb->buffer && layout) [e setVertexBuffer:vb->buffer offset:vertex_offset atIndex:VERTEX_BUFFER_INDEX];
        MTLPrimitiveType type = topology == 1 ? MTLPrimitiveTypePoint : topology == 2 ? MTLPrimitiveTypeLine
                              : topology == 3 ? MTLPrimitiveTypeLineStrip : topology == 5 ? MTLPrimitiveTypeTriangleStrip
                              : MTLPrimitiveTypeTriangle;
        [e drawPrimitives:type vertexStart:start vertexCount:count];
    });
}

void w2c_gpu_dispatch(struct w2c_gpu *unused, u32 x, u32 y, u32 z)
{
    (void)unused;
    LOCKED({
        Object *cs = get(shader[CS]);
        if (!cs || !x || !y || !z) break;
        if (!cs->compute) {
            NSError *error = nil;
            cs->compute = [device newComputePipelineStateWithFunction:cs->function error:&error];
            if (!cs->compute) {
                fprintf(stderr, "metal: compute pipeline %s: %s\n", cs->name, error.localizedDescription.UTF8String);
                break;
            }
        }
        if (!compute) {
            end_encoder();
            begin_command();
            compute = [command computeCommandEncoder];
        }
        id<MTLComputeCommandEncoder> e = compute;
        [e setComputePipelineState:cs->compute];
        bind_stage(CS, ^(id<MTLBuffer> b, NSUInteger o, NSUInteger i) { [e setBuffer:b offset:o atIndex:i]; },
                   ^(id<MTLTexture> t, NSUInteger i) { [e setTexture:t atIndex:i]; },
                   ^(id<MTLSamplerState> s, NSUInteger i) { [e setSamplerState:s atIndex:i]; });
        [e dispatchThreadgroups:MTLSizeMake(x, y, z)
          threadsPerThreadgroup:MTLSizeMake(cs->threads[0], cs->threads[1], cs->threads[2])];
    });
}

/* A clear is a render pass of its own that only clears. */
static void clear(Object *t, Object *z, const float *rgba, float depth)
{
    end_encoder();
    begin_command();
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    if (t) {
        d.colorAttachments[0].texture = t->texture;
        d.colorAttachments[0].loadAction = MTLLoadActionClear;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    if (z) {
        d.depthAttachment.texture = z->texture;
        d.depthAttachment.loadAction = MTLLoadActionClear;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.clearDepth = depth;
    }
    [[command renderCommandEncoderWithDescriptor:d] endEncoding];
}

void w2c_gpu_clear_color(struct w2c_gpu *unused, u32 rtv, f32 r, f32 g, f32 b, f32 a)
{
    (void)unused;
    LOCKED({ Object *t = resource_of(rtv); float c[4] = {r, g, b, a}; if (t && t->texture) clear(t, NULL, c, 0); });
}

void w2c_gpu_clear_depth(struct w2c_gpu *unused, u32 dsv, f32 depth)
{
    (void)unused;
    LOCKED({ Object *z = resource_of(dsv); if (z && z->texture) clear(NULL, z, NULL, depth); });
}

/* ---- copies, updates and reads ---- */

void w2c_gpu_copy(struct w2c_gpu *unused, u32 dst, u32 x, u32 y, u32 src, u32 box)
{
    (void)unused;
    LOCKED({
        Object *d = resource_of(dst), *s = resource_of(src);
        if (!d || !s) break;
        uint32_t b[6] = {0, 0, 0, 0, 0, 1};
        if (box) memcpy(b, at(box), sizeof b);
        if (d->buffer && s->buffer) {
            NSUInteger from = box ? b[0] : 0, size = box ? b[3] - b[0] : MIN(s->buffer.length, d->buffer.length);
            [blitter() copyFromBuffer:s->buffer sourceOffset:from toBuffer:d->buffer destinationOffset:box ? x : 0 size:size];
        } else if (d->texture && s->texture) {
            if (!box) b[3] = (uint32_t)MIN(s->texture.width, d->texture.width), b[4] = (uint32_t)MIN(s->texture.height, d->texture.height);
            if (b[3] <= b[0] || b[4] <= b[1]) break;
            [blitter() copyFromTexture:s->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(b[0], b[1], 0)
                            sourceSize:MTLSizeMake(b[3] - b[0], b[4] - b[1], 1) toTexture:d->texture destinationSlice:0
                      destinationLevel:0 destinationOrigin:MTLOriginMake(box ? x : 0, box ? y : 0, 0)];
        } else if (d->buffer && s->texture) {
            NSUInteger row = s->texture.width * pixel_bytes(s->texture.pixelFormat);
            [blitter() copyFromTexture:s->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                            sourceSize:MTLSizeMake(s->texture.width, s->texture.height, 1) toBuffer:d->buffer
                     destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * s->texture.height];
        }
    });
}

void w2c_gpu_update(struct w2c_gpu *unused, u32 dst, u32 box, u32 data, u32 pitch)
{
    (void)unused;
    LOCKED({
        Object *d = resource_of(dst);
        if (!d) break;
        uint32_t b[6] = {0, 0, 0, 0, 0, 1};
        if (box) memcpy(b, at(box), sizeof b);
        if (d->buffer) {
            NSUInteger offset = box ? b[0] : 0, size = box ? b[3] - b[0] : d->buffer.length;
            if (!size) break;
            if (!box && d->usage == USAGE_DYNAMIC) {  /* WRITE_DISCARD: new storage, recorded work keeps the old */
                d->buffer = [device newBufferWithBytes:at(data) length:size options:MTLResourceStorageModeShared];
                break;
            }
            id<MTLBuffer> staging = [device newBufferWithBytes:at(data) length:size options:MTLResourceStorageModeShared];
            [blitter() copyFromBuffer:staging sourceOffset:0 toBuffer:d->buffer destinationOffset:offset size:size];
        } else if (d->texture) {
            if (!box) b[3] = (uint32_t)d->texture.width, b[4] = (uint32_t)d->texture.height;
            uint32_t w = b[3] - b[0], h = b[4] - b[1];
            if (!w || !h) break;
            id<MTLBuffer> staging = [device newBufferWithBytes:at(data) length:(NSUInteger)pitch * h
                                                       options:MTLResourceStorageModeShared];
            [blitter() copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:pitch * h
                           sourceSize:MTLSizeMake(w, h, 1) toTexture:d->texture destinationSlice:0 destinationLevel:0
                    destinationOrigin:MTLOriginMake(b[0], b[1], 0)];
        }
    });
}

void w2c_gpu_read(struct w2c_gpu *unused, u32 src, u32 out, u32 pitch)
{
    (void)unused;
    LOCKED({
        Object *s = resource_of(src);
        if (!s) break;
        wait_idle();
        if (s->buffer) memcpy(at(out), s->buffer.contents, s->bytes ? s->bytes : s->buffer.length);
        else if (s->texture && s->texture.storageMode == MTLStorageModeShared)
            [s->texture getBytes:at(out) bytesPerRow:pitch fromRegion:MTLRegionMake2D(0, 0, s->texture.width, s->texture.height)
                     mipmapLevel:0];
    });
}

/* ---- queries ---- */

void w2c_gpu_query_end(struct w2c_gpu *unused, u32 q)
{
    (void)unused;
    LOCKED({ Object *o = get(q); if (o) begin_command(), o->serial = serial, o->value = 0; });
}

u32 w2c_gpu_query_get(struct w2c_gpu *unused, u32 q, u32 out, u32 size, u32 flush)
{
    (void)unused;
    uint32_t done = 1;
    LOCKED({
        Object *o = get(q);
        if (!o) break;
        if (o->serial > atomic_load(&completed)) {
            if (flush && o->serial == serial) commit();
            done = 0;
            break;
        }
        if (!o->value) o->value = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (o->query == 3 && size >= 12) {  /* TIMESTAMP_DISJOINT: {frequency, disjoint} */
            uint64_t frequency = 1000000000;
            uint32_t disjoint = 0;
            memcpy(at(out), &frequency, 8); memcpy(at(out + 8), &disjoint, 4);
        } else if (size >= 8) memcpy(at(out), &o->value, 8);
        else if (size >= 4) { uint32_t yes = 1; memcpy(at(out), &yes, 4); }
    });
    return done;
}

void w2c_gpu_flush(struct w2c_gpu *unused)
{
    (void)unused;
    LOCKED(commit());
}

/* ---- the window's picture (port/mac/native/window.m) ---- */

static __strong id<MTLTexture> back;
static id<MTLRenderPipelineState> show_pipeline;
CAMetalLayer *window_layer(void);

u32 w2c_gpu_back_buffer(struct w2c_gpu *unused, u32 width, u32 height)
{
    (void)unused;
    uint32_t h;
    LOCKED({
        if (!back || back.width != width || back.height != height) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:width ? width : 1
                                                                                        height:height ? height : 1
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            back = [device newTextureWithDescriptor:d];
        }
        h = new_object(O_TEXTURE);
        objects[h].texture = back;
        objects[h].width = (uint32_t)back.width; objects[h].height = (uint32_t)back.height;
        objects[h].format = FMT_RGBA8; objects[h].bind = BIND_RTV;
    });
    return h;
}

static id<MTLRenderPipelineState> show(MTLPixelFormat format)
{
    if (show_pipeline) return show_pipeline;
    static const char source[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 position [[position]]; float2 uv; };\n"
        "vertex V show_vertex(uint i [[vertex_id]]) {\n"
        "    float2 p = float2(i == 1 ? 3.0 : -1.0, i == 2 ? -3.0 : 1.0);\n"
        "    V v; v.position = float4(p, 0, 1); v.uv = float2((p.x + 1) / 2, (1 - p.y) / 2); return v; }\n"
        "fragment float4 show_pixel(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
        "    constexpr sampler s(filter::nearest); return t.sample(s, v.uv); }\n";
    NSError *error = nil;
    id<MTLLibrary> l = [device newLibraryWithSource:@(source) options:nil error:&error];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [l newFunctionWithName:@"show_vertex"];
    d.fragmentFunction = [l newFunctionWithName:@"show_pixel"];
    d.colorAttachments[0].pixelFormat = format;
    show_pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!show_pipeline) fprintf(stderr, "metal: present pipeline: %s\n", error.localizedDescription.UTF8String);
    return show_pipeline;
}

/* DHD_SNAP=path.png: the window's picture DHD_SNAP_AT seconds (default 10) after the first one;
   for checking the renderer without looking at the screen. DHD_SNAP_AT=20,40,60 takes one at each,
   as path-20.png and so on. */
static void snapshot(void)
{
    static double first;
    static unsigned taken;
    const char *path = getenv("DHD_SNAP"), *list = getenv("DHD_SNAP_AT");
    if (!path || !back) return;
    double times[16];
    unsigned count = 0;
    for (const char *t = list; t && *t && count < 16; t = strchr(t, ',') ? strchr(t, ',') + 1 : "")
        times[count++] = atof(t);
    if (!count) times[count++] = 10;
    double now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9;
    if (!first) first = now;
    if (taken >= count || now - first < times[taken]) return;
    char name[1024];
    if (count == 1) snprintf(name, sizeof name, "%s", path);
    else {
        const char *dot = strrchr(path, '.');
        int stem = dot ? (int)(dot - path) : (int)strlen(path);
        snprintf(name, sizeof name, "%.*s-%g%s", stem, path, times[taken], dot ? dot : ".png");
    }
    taken++;
    path = name;
    NSUInteger w = back.width, h = back.height, row = w * 4;
    id<MTLBuffer> pixels = [device newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    [blitter() copyFromTexture:back sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(w, h, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:row
      destinationBytesPerImage:row * h];
    wait_idle();
    CGColorSpaceRef space_rgb = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(pixels.contents, w, h, 8, row, space_rgb,
                                           (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault);
    CGImageRef image = CGBitmapContextCreateImage(c);
    CGImageDestinationRef out = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (out) CGImageDestinationAddImage(out, image, NULL), CGImageDestinationFinalize(out), CFRelease(out);
    CGImageRelease(image);
    CGContextRelease(c);
    CGColorSpaceRelease(space_rgb);
    fprintf(stderr, "gs: snapshot %s (%lux%lu)\n", path, (unsigned long)w, (unsigned long)h);
}

/* The back buffer to the window: the layer's next drawable, drawn over by the picture. */
void w2c_gpu_present(struct w2c_gpu *unused)
{
    (void)unused;
    LOCKED({
        snapshot();
        CAMetalLayer *layer = window_layer();
        if (!layer || !back) { commit(); break; }
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) { commit(); break; }
        end_encoder();
        begin_command();
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = drawable.texture;
        d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [command renderCommandEncoderWithDescriptor:d];
        id<MTLRenderPipelineState> p = show(layer.pixelFormat);
        if (p) {
            [e setRenderPipelineState:p];
            [e setFragmentTexture:back atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
        [command presentDrawable:drawable];
        commit();
    });
}

u32 w2c_gpu_adapter_name(struct w2c_gpu *unused, u32 out, u32 size)
{
    (void)unused;
    id<MTLDevice> d = metal_device();
    snprintf(at(out), size, "%s", d.name.UTF8String);
    return (u32)strlen(string_at(out));
}
