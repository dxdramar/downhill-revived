/* Direct3D 11 for the renderers on the Mac (finding 075): port/mac/include/d3d11.h's functions.
   Each object holds the native host's handle of its Metal object (port/mac/native/metal.m);
   calls go there with their descriptions flattened. The immediate context keeps no state of its
   own: the host keeps the bindings, as a D3D11 context does.

   Map: a mapped resource gets a block of module memory. WRITE_DISCARD hands it out and sends it
   at Unmap (the host gives a dynamic resource new storage, so work already recorded keeps the old
   contents); READ fills it from the GPU after the work so far. */
#include <d3d11.h>
#include "mac_gpu.h"

enum {
    K_DEVICE = 1, K_CONTEXT, K_BUFFER, K_TEXTURE, K_SRV, K_UAV, K_RTV, K_DSV, K_SHADER, K_LAYOUT, K_BLEND, K_DEPTH,
    K_RASTER, K_SAMPLER, K_QUERY, K_BLOB, K_SWAPCHAIN
};

typedef struct {
    uint32_t bytes, width, height, pitch, usage;
    void *mapped;
    D3D11_MAP map;
} Resource;

typedef struct { char entry[64]; } Blob;

static void *object(uint32_t kind, uint32_t handle, size_t extra)
{
    D3DObject *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = kind;
    o->handle = handle;
    o->refs = 1;
    if (extra && !(o->data = calloc(1, extra))) { free(o); return NULL; }
    return o;
}

static uint32_t handle_of(const void *p) { return p ? ((const D3DObject *)p)->handle : 0; }

ULONG d3d_add_ref(void *p)
{
    D3DObject *o = p;
    return o ? __atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) : 0;
}

ULONG d3d_release(void *p)
{
    D3DObject *o = p;
    if (!o) return 0;
    uint32_t left = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    if (left) return left;
    if (o->handle && o->kind != K_DEVICE && o->kind != K_CONTEXT && o->kind != K_SWAPCHAIN) gpu_free(o->handle);
    if (o->kind == K_BUFFER || o->kind == K_TEXTURE) free(((Resource *)o->data)->mapped);
    free(o->data);
    free(o);
    return 0;
}

static unsigned format_bytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
    case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
    default: return 4;
    }
}

/* ---- creation ---- */

static ID3D11Device the_device = {{K_DEVICE, 1, 1, NULL}};
static ID3D11DeviceContext the_context = {{K_CONTEXT, 1, 1, NULL}};

HRESULT D3D11CreateDevice(void *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                          const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk, ID3D11Device **device,
                          D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
    (void)adapter; (void)type; (void)software; (void)flags; (void)levels; (void)count; (void)sdk;
    if (device) *device = &the_device, d3d_add_ref(&the_device);
    if (context) *context = &the_context, d3d_add_ref(&the_context);
    if (level) *level = D3D_FEATURE_LEVEL_11_0;
    return S_OK;
}

/* raster.hlsl's entry points come precompiled (port/tools/metal_shaders.py): a blob names one,
   "native:" for DHD_NATIVE's set, "exact:" for the exact renderer's. */
HRESULT D3DCompile(const void *source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO *defines, void *include,
                   LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors)
{
    (void)source; (void)size; (void)name; (void)include; (void)target; (void)flags1; (void)flags2;
    int native = 0;
    for (const D3D_SHADER_MACRO *d = defines; d && d->Name; d++)
        native |= !strcmp(d->Name, "DHD_NATIVE");
    if (errors) *errors = NULL;
    ID3DBlob *b = object(K_BLOB, 0, sizeof(Blob));
    if (!b) return E_OUTOFMEMORY;
    snprintf(((Blob *)b->object.data)->entry, sizeof ((Blob *)0)->entry, "%s:%s", native ? "native" : "exact", entry);
    *code = b;
    return S_OK;
}

HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob **blob)
{
    (void)size;
    return (*blob = object(K_BLOB, 0, sizeof(Blob))) ? S_OK : E_OUTOFMEMORY;
}
void *ID3D10Blob_GetBufferPointer(ID3DBlob *b) { return ((Blob *)b->object.data)->entry; }
SIZE_T ID3D10Blob_GetBufferSize(ID3DBlob *b) { return strlen(((Blob *)b->object.data)->entry) + 1; }

/* ---- the device ---- */

HRESULT ID3D11Device_QueryInterface(ID3D11Device *d, REFIID iid, void **out)
{
    (void)d; (void)iid;
    *out = NULL;
    return E_NOINTERFACE;  /* no ID3D11Device3 (conservative rasterization) and no DXGI device */
}

/* No rasterizer-ordered views or conservative rasterization: gpu.c's hardware path stays off.
   No doubles either (raster.hlsl runs DHD_NO_DOUBLE); the exact renderer doesn't need the flag. */
HRESULT ID3D11Device_CheckFeatureSupport(ID3D11Device *d, D3D11_FEATURE feature, void *data, UINT size)
{
    (void)d;
    memset(data, 0, size);
    if (feature == D3D11_FEATURE_DOUBLES) ((D3D11_FEATURE_DATA_DOUBLES *)data)->DoublePrecisionFloatShaderOps = TRUE;
    return S_OK;
}

HRESULT ID3D11Device_CreateBuffer(ID3D11Device *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init,
                                  ID3D11Buffer **out)
{
    (void)d;
    uint32_t h = gpu_buffer(desc->ByteWidth, desc->BindFlags, desc->Usage, desc->MiscFlags, desc->StructureByteStride,
                            init ? init->pSysMem : NULL);
    if (!h) return E_OUTOFMEMORY;
    ID3D11Buffer *b = object(K_BUFFER, h, sizeof(Resource));
    if (!b) { gpu_free(h); return E_OUTOFMEMORY; }
    Resource *r = b->object.data;
    r->bytes = r->pitch = desc->ByteWidth;
    r->width = desc->ByteWidth; r->height = 1; r->usage = desc->Usage;
    *out = b;
    return S_OK;
}

HRESULT ID3D11Device_CreateTexture2D(ID3D11Device *d, const D3D11_TEXTURE2D_DESC *desc,
                                     const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    (void)d;
    static int trace = -1;
    if (trace < 0) trace = getenv("DHD_D3D_TRACE") != NULL;
    if (trace)
        fprintf(stderr, "d3d11: CreateTexture2D desc %p: %u x %u, mips %u, array %u, format %u, samples %u, usage %u, "
                "bind %X, cpu %X\n", (const void *)desc, desc->Width, desc->Height, desc->MipLevels, desc->ArraySize,
                desc->Format, desc->SampleDesc.Count, desc->Usage, desc->BindFlags, desc->CPUAccessFlags);
    uint32_t h = gpu_texture(desc->Width, desc->Height, desc->Format, desc->BindFlags, desc->Usage,
                             desc->CPUAccessFlags, desc->MipLevels ? desc->MipLevels : 1, init);
    if (!h) return E_OUTOFMEMORY;
    ID3D11Texture2D *t = object(K_TEXTURE, h, sizeof(Resource));
    if (!t) { gpu_free(h); return E_OUTOFMEMORY; }
    Resource *r = t->object.data;
    r->width = desc->Width; r->height = desc->Height; r->pitch = desc->Width * format_bytes(desc->Format);
    r->bytes = r->pitch * desc->Height; r->usage = desc->Usage;
    *out = t;
    return S_OK;
}

static HRESULT view(uint32_t kind, uint32_t object_kind, ID3D11Resource *r, uint32_t format, uint32_t first,
                    uint32_t count, uint32_t raw, void **out)
{
    uint32_t h = gpu_view(kind, handle_of(r), format, first, count, raw);
    if (!h) return E_INVALIDARG;
    if (!(*out = object(object_kind, h, 0))) { gpu_free(h); return E_OUTOFMEMORY; }
    return S_OK;
}

HRESULT ID3D11Device_CreateShaderResourceView(ID3D11Device *d, ID3D11Resource *r,
                                              const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                                              ID3D11ShaderResourceView **out)
{
    (void)d;
    uint32_t first = 0, count = 0, raw = 0, format = desc ? desc->Format : 0;
    if (desc && desc->ViewDimension == D3D11_SRV_DIMENSION_BUFFER)
        first = desc->Buffer.FirstElement, count = desc->Buffer.NumElements;
    else if (desc && desc->ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX)
        first = desc->BufferEx.FirstElement, count = desc->BufferEx.NumElements,
        raw = desc->BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW;
    return view(GPU_SRV, K_SRV, r, format, first, count, raw, (void **)out);
}

HRESULT ID3D11Device_CreateUnorderedAccessView(ID3D11Device *d, ID3D11Resource *r,
                                               const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc,
                                               ID3D11UnorderedAccessView **out)
{
    (void)d;
    uint32_t first = 0, count = 0, raw = 0, format = desc ? desc->Format : 0;
    if (desc && desc->ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
        first = desc->Buffer.FirstElement, count = desc->Buffer.NumElements,
        raw = desc->Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW;
    return view(GPU_UAV, K_UAV, r, format, first, count, raw, (void **)out);
}

HRESULT ID3D11Device_CreateRenderTargetView(ID3D11Device *d, ID3D11Resource *r,
                                            const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out)
{
    (void)d;
    return view(GPU_RTV, K_RTV, r, desc ? desc->Format : 0, 0, 0, 0, (void **)out);
}

HRESULT ID3D11Device_CreateDepthStencilView(ID3D11Device *d, ID3D11Resource *r,
                                            const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView **out)
{
    (void)d;
    return view(GPU_DSV, K_DSV, r, desc ? desc->Format : 0, 0, 0, 0, (void **)out);
}

static HRESULT shader(const void *code, void **out)
{
    uint32_t h = gpu_shader(code);
    if (!h) { fprintf(stderr, "d3d11: no Metal shader for %s\n", (const char *)code); return E_FAIL; }
    return (*out = object(K_SHADER, h, 0)) ? S_OK : E_OUTOFMEMORY;
}
HRESULT ID3D11Device_CreateVertexShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                        ID3D11VertexShader **out)
{
    (void)d; (void)size; (void)linkage;
    return shader(code, (void **)out);
}
HRESULT ID3D11Device_CreatePixelShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                       ID3D11PixelShader **out)
{
    (void)d; (void)size; (void)linkage;
    return shader(code, (void **)out);
}
HRESULT ID3D11Device_CreateComputeShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                         ID3D11ComputeShader **out)
{
    (void)d; (void)size; (void)linkage;
    return shader(code, (void **)out);
}

/* The elements in order are the vertex shader's attributes 0, 1, ... (raster.hlsl declares its
   inputs in the order gpu_native.inc lays them out). */
HRESULT ID3D11Device_CreateInputLayout(ID3D11Device *d, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
                                       const void *code, SIZE_T size, ID3D11InputLayout **out)
{
    (void)d; (void)code; (void)size;
    uint32_t packed[32];
    if (count > 16) return E_INVALIDARG;
    for (UINT i = 0; i < count; i++) packed[i * 2] = elements[i].Format, packed[i * 2 + 1] = elements[i].AlignedByteOffset;
    uint32_t h = gpu_layout(count, packed);
    return h && (*out = object(K_LAYOUT, h, 0)) ? S_OK : E_FAIL;
}

HRESULT ID3D11Device_CreateBlendState(ID3D11Device *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out)
{
    (void)d;
    const D3D11_RENDER_TARGET_BLEND_DESC *t = &desc->RenderTarget[0];
    uint32_t packed[8] = {(uint32_t)t->BlendEnable, t->SrcBlend, t->DestBlend, t->BlendOp, t->SrcBlendAlpha,
                          t->DestBlendAlpha, t->BlendOpAlpha, t->RenderTargetWriteMask};
    uint32_t h = gpu_blend(packed);
    return h && (*out = object(K_BLEND, h, 0)) ? S_OK : E_FAIL;
}

HRESULT ID3D11Device_CreateDepthStencilState(ID3D11Device *d, const D3D11_DEPTH_STENCIL_DESC *desc,
                                             ID3D11DepthStencilState **out)
{
    (void)d;
    uint32_t h = gpu_depth((uint32_t)desc->DepthEnable, desc->DepthWriteMask, desc->DepthFunc);
    return h && (*out = object(K_DEPTH, h, 0)) ? S_OK : E_FAIL;
}

HRESULT ID3D11Device_CreateRasterizerState(ID3D11Device *d, const D3D11_RASTERIZER_DESC *desc,
                                           ID3D11RasterizerState **out)
{
    (void)d;
    uint32_t h = gpu_raster(desc->CullMode, (uint32_t)desc->ScissorEnable, (uint32_t)desc->DepthClipEnable);
    return h && (*out = object(K_RASTER, h, 0)) ? S_OK : E_FAIL;
}

HRESULT ID3D11Device3_CreateRasterizerState2(ID3D11Device3 *d, const D3D11_RASTERIZER_DESC2 *desc,
                                             ID3D11RasterizerState2 **out)
{
    (void)d; (void)desc;
    *out = NULL;
    return E_FAIL;
}

HRESULT ID3D11Device_CreateSamplerState(ID3D11Device *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out)
{
    (void)d;
    uint32_t h = gpu_sampler(desc->Filter, desc->AddressU, desc->AddressV, desc->AddressW, desc->MaxAnisotropy);
    return h && (*out = object(K_SAMPLER, h, 0)) ? S_OK : E_FAIL;
}

HRESULT ID3D11Device_CreateQuery(ID3D11Device *d, const D3D11_QUERY_DESC *desc, ID3D11Query **out)
{
    (void)d;
    uint32_t h = gpu_query(desc->Query);
    return h && (*out = object(K_QUERY, h, 0)) ? S_OK : E_FAIL;
}

/* ---- the immediate context ---- */

void ID3D11DeviceContext_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s, void *i, UINT n)
{
    (void)c; (void)i; (void)n;
    gpu_set_shader(GPU_VS, handle_of(s));
}
void ID3D11DeviceContext_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s, void *i, UINT n)
{
    (void)c; (void)i; (void)n;
    gpu_set_shader(GPU_PS, handle_of(s));
}
void ID3D11DeviceContext_GSSetShader(ID3D11DeviceContext *c, ID3D11GeometryShader *s, void *i, UINT n)
{
    (void)c; (void)s; (void)i; (void)n;  /* no geometry shaders: the port only clears this stage */
}
void ID3D11DeviceContext_CSSetShader(ID3D11DeviceContext *c, ID3D11ComputeShader *s, void *i, UINT n)
{
    (void)c; (void)i; (void)n;
    gpu_set_shader(GPU_CS, handle_of(s));
}

static void bind(uint32_t stage, uint32_t kind, UINT slot, UINT count, const void *const *objects)
{
    for (UINT i = 0; i < count; i++) gpu_bind(stage, kind, slot + i, objects ? handle_of(objects[i]) : 0);
}
void ID3D11DeviceContext_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT n, ID3D11Buffer *const *b)
{
    (void)c; bind(GPU_VS, GPU_CBV, slot, n, (const void *const *)b);
}
void ID3D11DeviceContext_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT n, ID3D11Buffer *const *b)
{
    (void)c; bind(GPU_PS, GPU_CBV, slot, n, (const void *const *)b);
}
void ID3D11DeviceContext_CSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT n, ID3D11Buffer *const *b)
{
    (void)c; bind(GPU_CS, GPU_CBV, slot, n, (const void *const *)b);
}
void ID3D11DeviceContext_VSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT n,
                                              ID3D11ShaderResourceView *const *v)
{
    (void)c; bind(GPU_VS, GPU_SRV_SLOT, slot, n, (const void *const *)v);
}
void ID3D11DeviceContext_PSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT n,
                                              ID3D11ShaderResourceView *const *v)
{
    (void)c; bind(GPU_PS, GPU_SRV_SLOT, slot, n, (const void *const *)v);
}
void ID3D11DeviceContext_CSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT n,
                                              ID3D11ShaderResourceView *const *v)
{
    (void)c; bind(GPU_CS, GPU_SRV_SLOT, slot, n, (const void *const *)v);
}
void ID3D11DeviceContext_CSSetUnorderedAccessViews(ID3D11DeviceContext *c, UINT slot, UINT n,
                                                   ID3D11UnorderedAccessView *const *v, const UINT *initial)
{
    (void)c; (void)initial;
    bind(GPU_CS, GPU_UAV_SLOT, slot, n, (const void *const *)v);
}
void ID3D11DeviceContext_PSSetSamplers(ID3D11DeviceContext *c, UINT slot, UINT n, ID3D11SamplerState *const *s)
{
    (void)c; bind(GPU_PS, GPU_SAMPLER_SLOT, slot, n, (const void *const *)s);
}

void ID3D11DeviceContext_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *l)
{
    (void)c; gpu_state(GPU_LAYOUT, handle_of(l), 0, 0);
}
void ID3D11DeviceContext_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
    (void)c; gpu_state(GPU_TOPOLOGY, 0, t, 0);
}
void ID3D11DeviceContext_IASetVertexBuffers(ID3D11DeviceContext *c, UINT slot, UINT n, ID3D11Buffer *const *b,
                                            const UINT *strides, const UINT *offsets)
{
    (void)c; (void)slot;
    if (n) gpu_state(GPU_VERTEX_BUFFER, handle_of(b ? b[0] : NULL), strides ? strides[0] : 0, offsets ? offsets[0] : 0);
}
void ID3D11DeviceContext_RSSetState(ID3D11DeviceContext *c, void *s)
{
    (void)c; gpu_state(GPU_RASTER, handle_of(s), 0, 0);
}
void ID3D11DeviceContext_RSSetViewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v)
{
    (void)c;
    if (n) gpu_viewport(v->TopLeftX, v->TopLeftY, v->Width, v->Height, v->MinDepth, v->MaxDepth);
}
void ID3D11DeviceContext_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{
    (void)c;
    if (n) gpu_scissor(r->left, r->top, r->right, r->bottom);
}
void ID3D11DeviceContext_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *b, const FLOAT f[4], UINT mask)
{
    (void)c; (void)mask;
    if (f) gpu_blend_factor(f[0], f[1], f[2], f[3]);
    gpu_state(GPU_BLEND, handle_of(b), 0, 0);
}
void ID3D11DeviceContext_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref)
{
    (void)c; gpu_state(GPU_DEPTH, handle_of(s), ref, 0);
}
void ID3D11DeviceContext_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                            ID3D11DepthStencilView *dsv)
{
    (void)c;
    gpu_targets(n && rtv ? handle_of(rtv[0]) : 0, handle_of(dsv));
}
void ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *c, UINT n,
    ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv, UINT uav_slot, UINT uav_count,
    ID3D11UnorderedAccessView *const *uav, const UINT *initial)
{
    (void)uav_slot; (void)uav_count; (void)uav; (void)initial;  /* gpu.c's hardware path only, off here */
    if (n != (UINT)-1) ID3D11DeviceContext_OMSetRenderTargets(c, n, rtv, dsv);
}
void ID3D11DeviceContext_ClearRenderTargetView(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT rgba[4])
{
    (void)c; gpu_clear_color(handle_of(v), rgba[0], rgba[1], rgba[2], rgba[3]);
}
void ID3D11DeviceContext_ClearDepthStencilView(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT flags,
                                               FLOAT depth, BYTE stencil)
{
    (void)c; (void)stencil;
    if (flags & D3D11_CLEAR_DEPTH) gpu_clear_depth(handle_of(v), depth);
}
void ID3D11DeviceContext_Draw(ID3D11DeviceContext *c, UINT count, UINT start) { (void)c; gpu_draw(count, start); }
void ID3D11DeviceContext_Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z) { (void)c; gpu_dispatch(x, y, z); }

void ID3D11DeviceContext_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    (void)c; gpu_copy(handle_of(dst), 0, 0, handle_of(src), NULL);
}

void ID3D11DeviceContext_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dst_sub, UINT x,
                                               UINT y, UINT z, ID3D11Resource *src, UINT src_sub, const D3D11_BOX *box)
{
    (void)c; (void)dst_sub; (void)z; (void)src_sub;
    gpu_copy(handle_of(dst), x, y, handle_of(src), (const uint32_t *)box);
}

void ID3D11DeviceContext_UpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                                           const void *data, UINT row_pitch, UINT depth_pitch)
{
    (void)c; (void)sub; (void)depth_pitch;
    Resource *r = dst->object.data;
    gpu_update(handle_of(dst), (const uint32_t *)box, data, row_pitch ? row_pitch : r->pitch);
}

HRESULT ID3D11DeviceContext_Map(ID3D11DeviceContext *c, ID3D11Resource *res, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE *out)
{
    (void)c; (void)sub; (void)flags;
    Resource *r = res->object.data;
    if (!r->mapped && !(r->mapped = malloc(r->bytes))) return E_OUTOFMEMORY;
    r->map = type;
    if (type == D3D11_MAP_READ || type == D3D11_MAP_READ_WRITE) gpu_read(handle_of(res), r->mapped, r->pitch);
    out->pData = r->mapped;
    out->RowPitch = r->pitch;
    out->DepthPitch = r->bytes;
    return S_OK;
}

void ID3D11DeviceContext_Unmap(ID3D11DeviceContext *c, ID3D11Resource *res, UINT sub)
{
    (void)c; (void)sub;
    Resource *r = res->object.data;
    if (r->mapped && r->map != D3D11_MAP_READ) gpu_update(handle_of(res), NULL, r->mapped, r->pitch);
}

void ID3D11DeviceContext_Begin(ID3D11DeviceContext *c, void *q) { (void)c; (void)q; }
void ID3D11DeviceContext_End(ID3D11DeviceContext *c, void *q) { (void)c; gpu_query_end(handle_of(q)); }
HRESULT ID3D11DeviceContext_GetData(ID3D11DeviceContext *c, void *q, void *data, UINT size, UINT flags)
{
    (void)c;
    return gpu_query_get(handle_of(q), data, size, !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)) ? S_OK : S_FALSE;
}
void ID3D11DeviceContext_Flush(ID3D11DeviceContext *c) { (void)c; gpu_flush(); }

/* ---- DXGI: one swap chain, the game's window's ---- */

HRESULT IDXGIDevice_GetAdapter(IDXGIDevice *d, IDXGIAdapter **out)
{
    (void)d;
    *out = NULL;
    return E_FAIL;
}
HRESULT IDXGIAdapter_GetDesc(IDXGIAdapter *a, DXGI_ADAPTER_DESC *out)
{
    (void)a;
    memset(out, 0, sizeof *out);
    return E_FAIL;
}

typedef struct { uint32_t width, height; ID3D11Texture2D *buffer; } Chain;

IDXGISwapChain *dhd_mac_swap_chain(uint32_t width, uint32_t height);
IDXGISwapChain *dhd_mac_swap_chain(uint32_t width, uint32_t height)
{
    IDXGISwapChain *s = object(K_SWAPCHAIN, 1, sizeof(Chain));
    if (!s) return NULL;
    Chain *c = s->object.data;
    c->width = width; c->height = height;
    return s;
}

HRESULT IDXGISwapChain_GetDesc(IDXGISwapChain *s, DXGI_SWAP_CHAIN_DESC *out)
{
    Chain *c = s->object.data;
    memset(out, 0, sizeof *out);
    out->BufferDesc.Width = c->width; out->BufferDesc.Height = c->height;
    out->BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    out->SampleDesc.Count = 1; out->BufferCount = 2; out->Windowed = TRUE;
    out->OutputWindow = (HWND)1;  /* the game's window (GetClientRect asks the host) */
    return S_OK;
}

HRESULT IDXGISwapChain_ResizeBuffers(IDXGISwapChain *s, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                     UINT flags)
{
    (void)count; (void)format; (void)flags;
    Chain *c = s->object.data;
    c->width = width; c->height = height;
    return S_OK;
}

/* The back buffer: a texture of the chain's size, which gpu_present shows in the window. */
HRESULT IDXGISwapChain_GetBuffer(IDXGISwapChain *s, UINT index, REFIID iid, void **out)
{
    (void)index; (void)iid;
    Chain *c = s->object.data;
    uint32_t h = gpu_back_buffer(c->width, c->height);
    if (!h) return E_FAIL;
    ID3D11Texture2D *t = object(K_TEXTURE, h, sizeof(Resource));
    if (!t) return E_OUTOFMEMORY;
    Resource *r = t->object.data;
    r->width = c->width; r->height = c->height; r->pitch = c->width * 4; r->bytes = r->pitch * c->height;
    t->object.kind = K_TEXTURE;
    *out = t;
    return S_OK;
}

HRESULT IDXGISwapChain_Present(IDXGISwapChain *s, UINT interval, UINT flags)
{
    (void)s; (void)interval; (void)flags;
    gpu_present();
    return S_OK;
}
