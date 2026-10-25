/* Mac build (finding 075): the part of Direct3D 11 and DXGI the port's renderers use
   (port/gs/gpu.c, gpu_native.inc, texpack.c, font_sharp.c), on Metal. Types, fields and values
   are Microsoft's; the methods, macros in the real header, are functions here
   (port/mac/d3d11_mac.c), which hand the work to the native host's Metal device
   (port/mac/native/metal.m). Shaders come precompiled from raster.hlsl
   (port/tools/metal_shaders.py): D3DCompile looks its entry point up. */
#ifndef DHD_MAC_D3D11_H
#define DHD_MAC_D3D11_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef LONG HRESULT;
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005L)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#ifndef DHD_MAC_GUID
#define DHD_MAC_GUID
typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;
typedef GUID IID;
#define REFIID const IID *
#endif
#define MAKEINTRESOURCEA(i) ((LPSTR)(uintptr_t)(i))

/* ---- objects: one header, the module's handle of the host's object ---- */

typedef struct D3DObject {
    uint32_t kind, handle, refs;
    void *data;                /* the kind's own (port/mac/d3d11_mac.c) */
} D3DObject;

#define D3D_INTERFACE(name) typedef struct name { D3DObject object; } name;
D3D_INTERFACE(ID3D11Device)
D3D_INTERFACE(ID3D11Device3)
D3D_INTERFACE(ID3D11DeviceContext)
D3D_INTERFACE(ID3D11Resource)
D3D_INTERFACE(ID3D11Buffer)
D3D_INTERFACE(ID3D11Texture2D)
D3D_INTERFACE(ID3D11ShaderResourceView)
D3D_INTERFACE(ID3D11UnorderedAccessView)
D3D_INTERFACE(ID3D11RenderTargetView)
D3D_INTERFACE(ID3D11DepthStencilView)
D3D_INTERFACE(ID3D11VertexShader)
D3D_INTERFACE(ID3D11PixelShader)
D3D_INTERFACE(ID3D11ComputeShader)
D3D_INTERFACE(ID3D11GeometryShader)
D3D_INTERFACE(ID3D11InputLayout)
D3D_INTERFACE(ID3D11BlendState)
D3D_INTERFACE(ID3D11DepthStencilState)
D3D_INTERFACE(ID3D11RasterizerState)
D3D_INTERFACE(ID3D11RasterizerState2)
D3D_INTERFACE(ID3D11SamplerState)
D3D_INTERFACE(ID3D11Query)
D3D_INTERFACE(ID3D11Asynchronous)
D3D_INTERFACE(ID3D10Blob)
D3D_INTERFACE(IDXGISwapChain)
D3D_INTERFACE(IDXGIDevice)
D3D_INTERFACE(IDXGIAdapter)
#undef D3D_INTERFACE
typedef ID3D10Blob ID3DBlob;
typedef ID3D11Resource ID3D11View;

/* ---- enumerations ---- */

typedef enum { D3D_DRIVER_TYPE_UNKNOWN, D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_REFERENCE, D3D_DRIVER_TYPE_NULL,
               D3D_DRIVER_TYPE_SOFTWARE, D3D_DRIVER_TYPE_WARP } D3D_DRIVER_TYPE;
typedef enum { D3D_FEATURE_LEVEL_11_0 = 0xb000, D3D_FEATURE_LEVEL_11_1 = 0xb100 } D3D_FEATURE_LEVEL;
#define D3D11_SDK_VERSION 7
#define D3D11_FLOAT32_MAX 3.402823466e+38f

typedef enum {
    DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R32G32B32A32_FLOAT = 2, DXGI_FORMAT_R32G32B32A32_UINT = 3,
    DXGI_FORMAT_R32G32B32_FLOAT = 6, DXGI_FORMAT_R8G8B8A8_UNORM = 28, DXGI_FORMAT_R8G8B8A8_UINT = 30,
    DXGI_FORMAT_R32_TYPELESS = 39, DXGI_FORMAT_D32_FLOAT = 40, DXGI_FORMAT_R32_FLOAT = 41, DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87
} DXGI_FORMAT;

typedef enum { D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE, D3D11_USAGE_DYNAMIC, D3D11_USAGE_STAGING } D3D11_USAGE;
enum {
    D3D11_BIND_VERTEX_BUFFER = 0x1, D3D11_BIND_INDEX_BUFFER = 0x2, D3D11_BIND_CONSTANT_BUFFER = 0x4,
    D3D11_BIND_SHADER_RESOURCE = 0x8, D3D11_BIND_RENDER_TARGET = 0x20, D3D11_BIND_DEPTH_STENCIL = 0x40,
    D3D11_BIND_UNORDERED_ACCESS = 0x80
};
enum { D3D11_CPU_ACCESS_WRITE = 0x10000, D3D11_CPU_ACCESS_READ = 0x20000 };
enum { D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40 };
typedef enum { D3D11_MAP_READ = 1, D3D11_MAP_WRITE = 2, D3D11_MAP_READ_WRITE = 3, D3D11_MAP_WRITE_DISCARD = 4,
               D3D11_MAP_WRITE_NO_OVERWRITE = 5 } D3D11_MAP;
enum { D3D11_ASYNC_GETDATA_DONOTFLUSH = 0x1 };
enum { D3D11_CLEAR_DEPTH = 0x1, D3D11_CLEAR_STENCIL = 0x2 };

typedef enum { D3D11_SRV_DIMENSION_BUFFER = 1, D3D11_SRV_DIMENSION_TEXTURE2D = 4,
               D3D11_SRV_DIMENSION_BUFFEREX = 11 } D3D11_SRV_DIMENSION;
typedef enum { D3D11_UAV_DIMENSION_BUFFER = 1, D3D11_UAV_DIMENSION_TEXTURE2D = 4 } D3D11_UAV_DIMENSION;
typedef enum { D3D11_RTV_DIMENSION_TEXTURE2D = 4 } D3D11_RTV_DIMENSION;
typedef enum { D3D11_DSV_DIMENSION_TEXTURE2D = 3 } D3D11_DSV_DIMENSION;
enum { D3D11_BUFFEREX_SRV_FLAG_RAW = 0x1 };
enum { D3D11_BUFFER_UAV_FLAG_RAW = 0x1 };

typedef enum { D3D11_COMPARISON_NEVER = 1, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
               D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL,
               D3D11_COMPARISON_ALWAYS } D3D11_COMPARISON_FUNC;
typedef enum { D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_DEPTH_WRITE_MASK_ALL } D3D11_DEPTH_WRITE_MASK;
typedef enum { D3D11_STENCIL_OP_KEEP = 1 } D3D11_STENCIL_OP;
typedef enum { D3D11_BLEND_ZERO = 1, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
               D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA,
               D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_SRC_ALPHA_SAT,
               D3D11_BLEND_BLEND_FACTOR = 14, D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_SRC1_COLOR,
               D3D11_BLEND_INV_SRC1_COLOR, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA } D3D11_BLEND;
typedef enum { D3D11_BLEND_OP_ADD = 1, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_OP_MIN,
               D3D11_BLEND_OP_MAX } D3D11_BLEND_OP;
enum { D3D11_COLOR_WRITE_ENABLE_ALL = 15 };
typedef enum { D3D11_FILL_WIREFRAME = 2, D3D11_FILL_SOLID = 3 } D3D11_FILL_MODE;
typedef enum { D3D11_CULL_NONE = 1, D3D11_CULL_FRONT, D3D11_CULL_BACK } D3D11_CULL_MODE;
typedef enum { D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF, D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON }
    D3D11_CONSERVATIVE_RASTERIZATION_MODE;
typedef enum { D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED, D3D11_CONSERVATIVE_RASTERIZATION_TIER_1,
               D3D11_CONSERVATIVE_RASTERIZATION_TIER_2, D3D11_CONSERVATIVE_RASTERIZATION_TIER_3 }
    D3D11_CONSERVATIVE_RASTERIZATION_TIER;
typedef enum { D3D11_FILTER_MIN_MAG_MIP_POINT = 0, D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
               D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15, D3D11_FILTER_ANISOTROPIC = 0x55 } D3D11_FILTER;
typedef enum { D3D11_TEXTURE_ADDRESS_WRAP = 1, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
               D3D11_TEXTURE_ADDRESS_BORDER } D3D11_TEXTURE_ADDRESS_MODE;
typedef enum { D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,
               D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
               D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP }
    D3D11_PRIMITIVE_TOPOLOGY;
typedef enum { D3D11_INPUT_PER_VERTEX_DATA, D3D11_INPUT_PER_INSTANCE_DATA } D3D11_INPUT_CLASSIFICATION;
typedef enum { D3D11_QUERY_EVENT, D3D11_QUERY_OCCLUSION, D3D11_QUERY_TIMESTAMP, D3D11_QUERY_TIMESTAMP_DISJOINT } D3D11_QUERY;
typedef enum { D3D11_FEATURE_THREADING, D3D11_FEATURE_DOUBLES, D3D11_FEATURE_D3D11_OPTIONS2 = 12 } D3D11_FEATURE;

/* ---- descriptions ---- */

typedef struct { UINT Count, Quality; } DXGI_SAMPLE_DESC;
typedef struct { UINT ByteWidth; D3D11_USAGE Usage; UINT BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride; }
    D3D11_BUFFER_DESC;
typedef struct {
    UINT Width, Height, MipLevels, ArraySize; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage; UINT BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEXTURE2D_DESC;
typedef struct { const void *pSysMem; UINT SysMemPitch, SysMemSlicePitch; } D3D11_SUBRESOURCE_DATA;
typedef struct { void *pData; UINT RowPitch, DepthPitch; } D3D11_MAPPED_SUBRESOURCE;
typedef struct { UINT left, top, front, right, bottom, back; } D3D11_BOX;
typedef struct { FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth; } D3D11_VIEWPORT;
typedef RECT D3D11_RECT;

typedef struct {
    DXGI_FORMAT Format; D3D11_SRV_DIMENSION ViewDimension;
    union {
        struct { union { UINT FirstElement, ElementOffset; }; union { UINT NumElements, ElementWidth; }; } Buffer;
        struct { UINT MostDetailedMip, MipLevels; } Texture2D;
        struct { UINT FirstElement, NumElements, Flags; } BufferEx;
    };
} D3D11_SHADER_RESOURCE_VIEW_DESC;
typedef struct {
    DXGI_FORMAT Format; D3D11_UAV_DIMENSION ViewDimension;
    union { struct { UINT FirstElement, NumElements, Flags; } Buffer; struct { UINT MipSlice; } Texture2D; };
} D3D11_UNORDERED_ACCESS_VIEW_DESC;
typedef struct { DXGI_FORMAT Format; D3D11_RTV_DIMENSION ViewDimension; union { struct { UINT MipSlice; } Texture2D; }; }
    D3D11_RENDER_TARGET_VIEW_DESC;
typedef struct {
    DXGI_FORMAT Format; D3D11_DSV_DIMENSION ViewDimension; UINT Flags;
    union { struct { UINT MipSlice; } Texture2D; };
} D3D11_DEPTH_STENCIL_VIEW_DESC;

typedef struct { D3D11_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp; D3D11_COMPARISON_FUNC StencilFunc; }
    D3D11_DEPTH_STENCILOP_DESC;
typedef struct {
    BOOL DepthEnable; D3D11_DEPTH_WRITE_MASK DepthWriteMask; D3D11_COMPARISON_FUNC DepthFunc; BOOL StencilEnable;
    BYTE StencilReadMask, StencilWriteMask; D3D11_DEPTH_STENCILOP_DESC FrontFace, BackFace;
} D3D11_DEPTH_STENCIL_DESC;
typedef struct {
    BOOL BlendEnable; D3D11_BLEND SrcBlend, DestBlend; D3D11_BLEND_OP BlendOp;
    D3D11_BLEND SrcBlendAlpha, DestBlendAlpha; D3D11_BLEND_OP BlendOpAlpha; BYTE RenderTargetWriteMask;
} D3D11_RENDER_TARGET_BLEND_DESC;
typedef struct { BOOL AlphaToCoverageEnable, IndependentBlendEnable; D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8]; }
    D3D11_BLEND_DESC;
typedef struct {
    D3D11_FILL_MODE FillMode; D3D11_CULL_MODE CullMode; BOOL FrontCounterClockwise; INT DepthBias;
    FLOAT DepthBiasClamp, SlopeScaledDepthBias; BOOL DepthClipEnable, ScissorEnable, MultisampleEnable,
    AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;
typedef struct {
    D3D11_FILL_MODE FillMode; D3D11_CULL_MODE CullMode; BOOL FrontCounterClockwise; INT DepthBias;
    FLOAT DepthBiasClamp, SlopeScaledDepthBias; BOOL DepthClipEnable, ScissorEnable, MultisampleEnable,
    AntialiasedLineEnable; UINT ForcedSampleCount; D3D11_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
} D3D11_RASTERIZER_DESC2;
typedef struct {
    D3D11_FILTER Filter; D3D11_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW; FLOAT MipLODBias;
    UINT MaxAnisotropy; D3D11_COMPARISON_FUNC ComparisonFunc; FLOAT BorderColor[4], MinLOD, MaxLOD;
} D3D11_SAMPLER_DESC;
typedef struct {
    LPCSTR SemanticName; UINT SemanticIndex; DXGI_FORMAT Format; UINT InputSlot, AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass; UINT InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;
typedef struct { D3D11_QUERY Query; UINT MiscFlags; } D3D11_QUERY_DESC;
typedef struct { UINT64 Frequency; BOOL Disjoint; } D3D11_QUERY_DATA_TIMESTAMP_DISJOINT;
typedef struct { BOOL DoublePrecisionFloatShaderOps; } D3D11_FEATURE_DATA_DOUBLES;
typedef struct {
    BOOL PSSpecifiedStencilRefSupported, TypedUAVLoadAdditionalFormats, ROVsSupported;
    D3D11_CONSERVATIVE_RASTERIZATION_TIER ConservativeRasterizationTier; UINT TiledResourcesTier;
    BOOL MapOnDefaultTextures, StandardSwizzle, UnifiedMemoryArchitecture;
} D3D11_FEATURE_DATA_D3D11_OPTIONS2;

typedef struct { UINT Width, Height; struct { UINT Numerator, Denominator; } RefreshRate; DXGI_FORMAT Format;
                 UINT ScanlineOrdering, Scaling; } DXGI_MODE_DESC;
typedef struct {
    DXGI_MODE_DESC BufferDesc; DXGI_SAMPLE_DESC SampleDesc; UINT BufferUsage, BufferCount; HWND OutputWindow;
    BOOL Windowed; UINT SwapEffect, Flags;
} DXGI_SWAP_CHAIN_DESC;
typedef struct {
    WCHAR Description[128]; UINT VendorId, DeviceId, SubSysId, Revision;
    SIZE_T DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory; LARGE_INTEGER AdapterLuid;
} DXGI_ADAPTER_DESC;

typedef struct { LPCSTR Name, Definition; } D3D_SHADER_MACRO;
#define D3DCOMPILE_ENABLE_STRICTNESS (1u << 11)
#define D3DCOMPILE_IEEE_STRICTNESS (1u << 13)

/* ---- creation ---- */

HRESULT D3D11CreateDevice(void *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                          const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk, ID3D11Device **device,
                          D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context);
HRESULT D3DCompile(const void *source, SIZE_T size, LPCSTR name, const D3D_SHADER_MACRO *defines, void *include,
                   LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);
HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob **blob);
void *ID3D10Blob_GetBufferPointer(ID3DBlob *blob);
SIZE_T ID3D10Blob_GetBufferSize(ID3DBlob *blob);

/* ---- reference counts: one pair for every kind ---- */

ULONG d3d_add_ref(void *object);
ULONG d3d_release(void *object);
#define D3D_REFCOUNT(name) \
    static inline ULONG name##_AddRef(name *o) { return d3d_add_ref(o); } \
    static inline ULONG name##_Release(name *o) { return d3d_release(o); }
D3D_REFCOUNT(ID3D11Device) D3D_REFCOUNT(ID3D11Device3) D3D_REFCOUNT(ID3D11DeviceContext)
D3D_REFCOUNT(ID3D11Resource) D3D_REFCOUNT(ID3D11Buffer) D3D_REFCOUNT(ID3D11Texture2D)
D3D_REFCOUNT(ID3D11ShaderResourceView) D3D_REFCOUNT(ID3D11UnorderedAccessView) D3D_REFCOUNT(ID3D11RenderTargetView)
D3D_REFCOUNT(ID3D11DepthStencilView) D3D_REFCOUNT(ID3D11VertexShader) D3D_REFCOUNT(ID3D11PixelShader)
D3D_REFCOUNT(ID3D11ComputeShader) D3D_REFCOUNT(ID3D11GeometryShader) D3D_REFCOUNT(ID3D11InputLayout)
D3D_REFCOUNT(ID3D11BlendState) D3D_REFCOUNT(ID3D11DepthStencilState) D3D_REFCOUNT(ID3D11RasterizerState)
D3D_REFCOUNT(ID3D11RasterizerState2) D3D_REFCOUNT(ID3D11SamplerState) D3D_REFCOUNT(ID3D11Query)
D3D_REFCOUNT(ID3D10Blob) D3D_REFCOUNT(IDXGISwapChain) D3D_REFCOUNT(IDXGIDevice) D3D_REFCOUNT(IDXGIAdapter)
#undef D3D_REFCOUNT

/* ---- the device ---- */

HRESULT ID3D11Device_QueryInterface(ID3D11Device *d, REFIID iid, void **out);
HRESULT ID3D11Device_CheckFeatureSupport(ID3D11Device *d, D3D11_FEATURE feature, void *data, UINT size);
HRESULT ID3D11Device_CreateBuffer(ID3D11Device *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init,
                                  ID3D11Buffer **out);
HRESULT ID3D11Device_CreateTexture2D(ID3D11Device *d, const D3D11_TEXTURE2D_DESC *desc,
                                     const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out);
HRESULT ID3D11Device_CreateShaderResourceView(ID3D11Device *d, ID3D11Resource *r,
                                              const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                                              ID3D11ShaderResourceView **out);
HRESULT ID3D11Device_CreateUnorderedAccessView(ID3D11Device *d, ID3D11Resource *r,
                                               const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc,
                                               ID3D11UnorderedAccessView **out);
HRESULT ID3D11Device_CreateRenderTargetView(ID3D11Device *d, ID3D11Resource *r,
                                            const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out);
HRESULT ID3D11Device_CreateDepthStencilView(ID3D11Device *d, ID3D11Resource *r,
                                            const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView **out);
HRESULT ID3D11Device_CreateVertexShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                        ID3D11VertexShader **out);
HRESULT ID3D11Device_CreatePixelShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                       ID3D11PixelShader **out);
HRESULT ID3D11Device_CreateComputeShader(ID3D11Device *d, const void *code, SIZE_T size, void *linkage,
                                         ID3D11ComputeShader **out);
HRESULT ID3D11Device_CreateInputLayout(ID3D11Device *d, const D3D11_INPUT_ELEMENT_DESC *elements, UINT count,
                                       const void *code, SIZE_T size, ID3D11InputLayout **out);
HRESULT ID3D11Device_CreateBlendState(ID3D11Device *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out);
HRESULT ID3D11Device_CreateDepthStencilState(ID3D11Device *d, const D3D11_DEPTH_STENCIL_DESC *desc,
                                             ID3D11DepthStencilState **out);
HRESULT ID3D11Device_CreateRasterizerState(ID3D11Device *d, const D3D11_RASTERIZER_DESC *desc,
                                           ID3D11RasterizerState **out);
HRESULT ID3D11Device3_CreateRasterizerState2(ID3D11Device3 *d, const D3D11_RASTERIZER_DESC2 *desc,
                                             ID3D11RasterizerState2 **out);
HRESULT ID3D11Device_CreateSamplerState(ID3D11Device *d, const D3D11_SAMPLER_DESC *desc, ID3D11SamplerState **out);
HRESULT ID3D11Device_CreateQuery(ID3D11Device *d, const D3D11_QUERY_DESC *desc, ID3D11Query **out);

/* ---- the immediate context ---- */

void ID3D11DeviceContext_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s, void *instances, UINT count);
void ID3D11DeviceContext_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s, void *instances, UINT count);
void ID3D11DeviceContext_GSSetShader(ID3D11DeviceContext *c, ID3D11GeometryShader *s, void *instances, UINT count);
void ID3D11DeviceContext_CSSetShader(ID3D11DeviceContext *c, ID3D11ComputeShader *s, void *instances, UINT count);
void ID3D11DeviceContext_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT count, ID3D11Buffer *const *b);
void ID3D11DeviceContext_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT count, ID3D11Buffer *const *b);
void ID3D11DeviceContext_CSSetConstantBuffers(ID3D11DeviceContext *c, UINT slot, UINT count, ID3D11Buffer *const *b);
void ID3D11DeviceContext_VSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT count,
                                              ID3D11ShaderResourceView *const *v);
void ID3D11DeviceContext_PSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT count,
                                              ID3D11ShaderResourceView *const *v);
void ID3D11DeviceContext_CSSetShaderResources(ID3D11DeviceContext *c, UINT slot, UINT count,
                                              ID3D11ShaderResourceView *const *v);
void ID3D11DeviceContext_CSSetUnorderedAccessViews(ID3D11DeviceContext *c, UINT slot, UINT count,
                                                   ID3D11UnorderedAccessView *const *v, const UINT *initial);
void ID3D11DeviceContext_PSSetSamplers(ID3D11DeviceContext *c, UINT slot, UINT count, ID3D11SamplerState *const *s);
void ID3D11DeviceContext_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *layout);
void ID3D11DeviceContext_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY topology);
void ID3D11DeviceContext_IASetVertexBuffers(ID3D11DeviceContext *c, UINT slot, UINT count, ID3D11Buffer *const *b,
                                            const UINT *strides, const UINT *offsets);
void ID3D11DeviceContext_RSSetState(ID3D11DeviceContext *c, void *state);
void ID3D11DeviceContext_RSSetViewports(ID3D11DeviceContext *c, UINT count, const D3D11_VIEWPORT *v);
void ID3D11DeviceContext_RSSetScissorRects(ID3D11DeviceContext *c, UINT count, const D3D11_RECT *r);
void ID3D11DeviceContext_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *b, const FLOAT factor[4], UINT mask);
void ID3D11DeviceContext_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref);
void ID3D11DeviceContext_OMSetRenderTargets(ID3D11DeviceContext *c, UINT count, ID3D11RenderTargetView *const *rtv,
                                            ID3D11DepthStencilView *dsv);
void ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *c, UINT count,
    ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv, UINT uav_slot, UINT uav_count,
    ID3D11UnorderedAccessView *const *uav, const UINT *initial);
void ID3D11DeviceContext_ClearRenderTargetView(ID3D11DeviceContext *c, ID3D11RenderTargetView *v, const FLOAT rgba[4]);
void ID3D11DeviceContext_ClearDepthStencilView(ID3D11DeviceContext *c, ID3D11DepthStencilView *v, UINT flags,
                                               FLOAT depth, BYTE stencil);
void ID3D11DeviceContext_Draw(ID3D11DeviceContext *c, UINT count, UINT start);
void ID3D11DeviceContext_Dispatch(ID3D11DeviceContext *c, UINT x, UINT y, UINT z);
void ID3D11DeviceContext_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src);
void ID3D11DeviceContext_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dst_sub, UINT x,
                                               UINT y, UINT z, ID3D11Resource *src, UINT src_sub, const D3D11_BOX *box);
void ID3D11DeviceContext_UpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                                           const void *data, UINT row_pitch, UINT depth_pitch);
HRESULT ID3D11DeviceContext_Map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT flags,
                                D3D11_MAPPED_SUBRESOURCE *out);
void ID3D11DeviceContext_Unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub);
void ID3D11DeviceContext_Begin(ID3D11DeviceContext *c, void *query);
void ID3D11DeviceContext_End(ID3D11DeviceContext *c, void *query);
HRESULT ID3D11DeviceContext_GetData(ID3D11DeviceContext *c, void *query, void *data, UINT size, UINT flags);
void ID3D11DeviceContext_Flush(ID3D11DeviceContext *c);

/* ---- DXGI ---- */

HRESULT IDXGIDevice_GetAdapter(IDXGIDevice *d, IDXGIAdapter **out);
HRESULT IDXGIAdapter_GetDesc(IDXGIAdapter *a, DXGI_ADAPTER_DESC *out);
HRESULT IDXGISwapChain_GetBuffer(IDXGISwapChain *s, UINT index, REFIID iid, void **out);
HRESULT IDXGISwapChain_GetDesc(IDXGISwapChain *s, DXGI_SWAP_CHAIN_DESC *out);
HRESULT IDXGISwapChain_ResizeBuffers(IDXGISwapChain *s, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                     UINT flags);
HRESULT IDXGISwapChain_Present(IDXGISwapChain *s, UINT interval, UINT flags);

#ifdef __cplusplus
}
#endif

#endif
