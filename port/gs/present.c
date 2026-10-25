/* D3D11 scanout. GS drawing and texture transfers share local memory; presentation
   reads the framebuffer selected by the game's DISPFB/DISPLAY MMIO registers. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "present.h"
#include "gpu_win32.h"
#include "capture.h"
#include "../runtime/memory.h"
#include "../hle/hle.h"
#include "../hle/text_input.h"
#include "../hle/splash.h"
#ifndef _WIN32
#include <time.h>
#include "mac_gpu.h"
#endif

static HWND window;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static IDXGISwapChain *swapchain;
static ID3D11Texture2D *backbuffer;
static unsigned buffer_width, buffer_height;
static uint32_t pixels[1024 * 1024];
static int initialized, disabled, resize_pending;
static volatile LONG window_closed;
#ifdef _WIN32
/* Laptops with two graphics chips run a program they don't know on the integrated one, which may
   lack what the renderers need (double-precision shaders, gpu.c); these exports ask the NVIDIA and
   AMD drivers for the dedicated chip, as most PC games do. */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif
/* Presentation for low latency and variable refresh (round 38, finding 052): a flip-model chain
   that may tear (G-SYNC/FreeSync show each picture when presented; the port's own limiter paces
   them, kernel.c) and holds at most one frame (the frame-latency waitable object, else the
   device's maximum frame latency of 1). DHD_TEARING=0 leaves tearing off. */
static UINT swap_flags;
static HANDLE latency_waitable;
static volatile LONG latency_armed;

#ifdef _WIN32
static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Flip chains need ResizeBuffers after window/fullscreen transitions,
       even when the game's scanout dimensions have not changed. */
    if (msg == WM_CHAR && text_input_active()) { text_input_character((unsigned)wp); return 0; }
    if (msg == WM_KILLFOCUS) text_input_clear();
    if (msg == WM_SIZE) resize_pending = 1;
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* The window has a thread of its own that only handles its messages. While the window is dragged
   or resized (or its system menu is open) Windows runs a modal loop inside DispatchMessage, which
   on the game's thread stopped the game until the mouse was let go; online that left the player
   behind everyone. The game thread still draws and presents to the window, and still ends the
   process when the window closes (gs_pump_messages). */
typedef struct { const char *class_name; DWORD style; int x, y, w, h; HANDLE ready; } WindowSpec;

static DWORD WINAPI window_thread(void *arg)
{
    WindowSpec *s = arg;
    MSG msg;
    window = CreateWindowA(s->class_name, "Downhill Revived", s->style, s->x, s->y, s->w, s->h, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    SetEvent(s->ready);  /* s is gone after this */
    if (!window) return 0;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    InterlockedExchange(&window_closed, 1);
    return 0;
}

#endif

static void release_presentation_device(void)
{
    if (context) { ID3D11DeviceContext_Release(context); context = NULL; }
    if (swapchain) { IDXGISwapChain_Release(swapchain); swapchain = NULL; }
    if (device) { ID3D11Device_Release(device); device = NULL; }
}

#ifdef _WIN32
/* DXGI wants a swap chain out of exclusive fullscreen before it goes. */
static void leave_fullscreen(void)
{
    if (swapchain) IDXGISwapChain_SetFullscreenState(swapchain, FALSE, NULL);
}

static int init(void)
{
    WNDCLASSA wc = {0};
    DXGI_SWAP_CHAIN_DESC desc = {0};
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL reference[] = {D3D_FEATURE_LEVEL_11_0};
    const D3D_FEATURE_LEVEL hardware[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const char *hardware_option = getenv("DHD_GS_HARDWARE");
    int hardware_enabled = !hardware_option || atoi(hardware_option);
    const D3D_FEATURE_LEVEL *required = hardware_enabled ? hardware : reference;
    unsigned required_count = hardware_enabled ? 2 : 1;
    HRESULT hr;
    host_display_aware();  /* real pixels: a borderless window at the monitor's own resolution */
    wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "DHDNativeGS";
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));  /* port/res/dhd.rc */
    if (!RegisterClassA(&wc)) return 0;
    /* DHD_WINDOW: windowed (default; DHD_WINDOW_WIDTH x DHD_WINDOW_HEIGHT inside, 896x672),
       borderless (the whole monitor) or fullscreen (exclusive, the monitor's current mode). A
       fullscreen window starts borderless: when Windows refuses exclusive mode at the start, or
       drops it as the start window goes, the game still fills the monitor, as gs_window_apply's does. */
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int borderless = mode && !strcmp(mode, "borderless"), fullscreen = mode && !strcmp(mode, "fullscreen");
    MONITORINFO monitor = {sizeof monitor};
    POINT origin = {0, 0};
    GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor);
    RECT rect = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672};
    DWORD style = borderless || fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (borderless || fullscreen) {
        rect = monitor.rcMonitor;
        x = rect.left; y = rect.top;
    } else AdjustWindowRect(&rect, style, FALSE);
    WindowSpec spec = {wc.lpszClassName, style, x, y, rect.right - rect.left, rect.bottom - rect.top,
                       CreateEventA(NULL, TRUE, FALSE, NULL)};
    HANDLE thread = spec.ready ? CreateThread(NULL, 0, window_thread, &spec, 0, NULL) : NULL;
    if (thread) WaitForSingleObject(spec.ready, INFINITE), CloseHandle(thread);
    if (spec.ready) CloseHandle(spec.ready);
    if (!window) return 0;
    desc.BufferDesc.Width = 640; desc.BufferDesc.Height = 448;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
    const char *flip_option = getenv("DHD_FLIP_MODEL");
    int flip_model = !flip_option || atoi(flip_option);
    desc.SwapEffect = flip_model ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    const char *tearing_option = getenv("DHD_TEARING");
    BOOL tearing = FALSE;
    if (flip_model && (!tearing_option || atoi(tearing_option))) {
        static const GUID factory1_iid = {0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
        static const GUID factory5_iid = {0x7632e1f5,0xee65,0x4dca,{0x87,0xfd,0x84,0xcd,0x75,0xf8,0x83,0x8d}};
        IDXGIFactory1 *factory1 = NULL;
        IDXGIFactory5 *factory5 = NULL;
        if (SUCCEEDED(CreateDXGIFactory1(&factory1_iid, (void **)&factory1))) {
            if (SUCCEEDED(IDXGIFactory1_QueryInterface(factory1, &factory5_iid, (void **)&factory5))) {
                if (FAILED(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                             sizeof tearing))) tearing = FALSE;
                IDXGIFactory5_Release(factory5);
            }
            IDXGIFactory1_Release(factory1);
        }
    }
    if (flip_model)
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, required, required_count,
                                     D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
    if (FAILED(hr) && desc.Flags) {  /* a runtime without those flags: the chain as before */
        release_presentation_device();
        desc.Flags = 0;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, required, required_count,
                                         D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
    }
    /* Older runtimes reject a feature-level 11_1 request outright. Preserve
       the reference device and original presentation fallback in that case. */
    if (FAILED(hr) && hardware_enabled) {
        release_presentation_device();
        required = reference; required_count = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, required, required_count,
            D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
    }
    if (FAILED(hr) && flip_model) {
        release_presentation_device();
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Flags = 0;
        flip_model = 0;
        hle_log("gs: flip presentation unavailable, using discard\n");
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, required, required_count,
                                         D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
    }
    if (FAILED(hr)) {
        release_presentation_device();
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, required, required_count,
                                         D3D11_SDK_VERSION, &desc, &swapchain, &device, &level, &context);
    }
    if (FAILED(hr)) {
        release_presentation_device();
        fprintf(stderr, "gs: D3D11 initialization failed (%08lX)\n", hr); return 0;
    }
    hle_log("gs: D3D11 presentation, feature level %X\n", level);
    hle_log("gs: swap effect %u (flip model %d)\n", (unsigned)desc.SwapEffect, flip_model);
    swap_flags = desc.Flags;
    if (swap_flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        static const GUID chain2_iid = {0xa8be2ac4,0x199f,0x4946,{0xb3,0x31,0x79,0x59,0x9f,0xb9,0x8d,0xe7}};
        IDXGISwapChain2 *chain2 = NULL;
        if (SUCCEEDED(IDXGISwapChain_QueryInterface(swapchain, &chain2_iid, (void **)&chain2))) {
            IDXGISwapChain2_SetMaximumFrameLatency(chain2, 1);
            latency_waitable = IDXGISwapChain2_GetFrameLatencyWaitableObject(chain2);
            IDXGISwapChain2_Release(chain2);
        }
    }
    if (!latency_waitable) {
        static const GUID device1_iid = {0x77db970f,0x6276,0x48ba,{0xba,0x28,0x07,0x01,0x43,0xb4,0x39,0x2c}};
        IDXGIDevice1 *device1 = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &device1_iid, (void **)&device1))) {
            IDXGIDevice1_SetMaximumFrameLatency(device1, 1);
            IDXGIDevice1_Release(device1);
        }
    }
    latency_armed = latency_waitable != NULL;  /* the first wait takes the chain's initial frame */
    hle_log("gs: tearing %s, frame latency 1 (%s)\n", swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING ? "allowed" : "off",
            latency_waitable ? "waitable object" : "device");
    {  /* the graphics chip, in every log: the first thing a player's report needs */
        static const GUID dxgi_iid = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};
        IDXGIDevice *dx = NULL; IDXGIAdapter *adapter = NULL; DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &dxgi_iid, (void **)&dx))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &adapter))) {
                if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad))) {
                    hle_log("gs: adapter %ls\n", ad.Description);
                    if (getenv("DHD_GPU_TRACE"))
                        fprintf(stderr, "gpu trace adapter: %ls vendor=%04X device=%04X dedicated=%llu\n", ad.Description, ad.VendorId, ad.DeviceId, (uint64_t)ad.DedicatedVideoMemory);
                }
                IDXGIAdapter_Release(adapter);
            }
            IDXGIDevice_Release(dx);
        }
    }
    splash_step(75, "Preparing the graphics...");
    if (!gs_gpu_init(&gs, device, context)) return 0;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    splash_close();
    if (fullscreen) {
        DXGI_MODE_DESC target = {0};
        target.Width = (UINT)(monitor.rcMonitor.right - monitor.rcMonitor.left);
        target.Height = (UINT)(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        IDXGISwapChain_ResizeTarget(swapchain, &target);
        hr = IDXGISwapChain_SetFullscreenState(swapchain, TRUE, NULL);
        if (FAILED(hr)) hle_log("gs: exclusive fullscreen unavailable (%08lX), borderless\n", hr);
        else atexit(leave_fullscreen);
        resize_pending = 1;
    }
    return 1;
}

/* The PC settings page changed DHD_WINDOW, DHD_WINDOW_WIDTH or DHD_WINDOW_HEIGHT: the window
   mode and size now, as init sets them up. EE thread (it owns the window, so the messages these
   calls send are handled at once); the native render thread's context lock is held around the
   swap chain's fullscreen change. The back buffer follows at the next present (WM_SIZE). */
void gs_window_apply(void)
{
    if (!window || !swapchain) return;
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int borderless = mode && !strcmp(mode, "borderless"), fullscreen = mode && !strcmp(mode, "fullscreen");
    int native = gs_gpu_native(&gs);
    BOOL exclusive = FALSE;
    MONITORINFO monitor = {sizeof monitor};
    GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
    if (native) gs_native_lock(&gs, 1);
    IDXGISwapChain_GetFullscreenState(swapchain, &exclusive, NULL);
    if (exclusive && !fullscreen) IDXGISwapChain_SetFullscreenState(swapchain, FALSE, NULL);
    if (native) gs_native_lock(&gs, 0);
    if (borderless || fullscreen) {
        RECT r = monitor.rcMonitor;
        SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        RECT r = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top;
        RECT work = monitor.rcWork;
        int x = work.left + ((work.right - work.left) - w) / 2, y = work.top + ((work.bottom - work.top) - h) / 2;
        SetWindowLongA(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(window, HWND_NOTOPMOST, x > work.left ? x : work.left, y > work.top ? y : work.top, w, h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    if (fullscreen && !exclusive) {
        DXGI_MODE_DESC target = {0};
        target.Width = (UINT)(monitor.rcMonitor.right - monitor.rcMonitor.left);
        target.Height = (UINT)(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (native) gs_native_lock(&gs, 1);
        IDXGISwapChain_ResizeTarget(swapchain, &target);
        HRESULT hr = IDXGISwapChain_SetFullscreenState(swapchain, TRUE, NULL);
        if (native) gs_native_lock(&gs, 0);
        if (FAILED(hr)) hle_log("gs: exclusive fullscreen unavailable (%08lX), borderless\n", hr);
        else {
            static int registered;
            if (!registered++) atexit(leave_fullscreen);
        }
    }
    resize_pending = 1;
}

#else
/* DHD_WINDOW on the Mac (finding 075): windowed (DHD_WINDOW_WIDTH x DHD_WINDOW_HEIGHT points,
   896x672) or, borderless and fullscreen alike, the whole screen in a fullscreen space. */
static uint32_t window_mode(uint32_t *width, uint32_t *height)
{
    const char *mode = getenv("DHD_WINDOW"), *w = getenv("DHD_WINDOW_WIDTH"), *h = getenv("DHD_WINDOW_HEIGHT");
    *width = w && atoi(w) > 0 ? (uint32_t)atoi(w) : 896;
    *height = h && atoi(h) > 0 ? (uint32_t)atoi(h) : 672;
    return mode && (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen")) ? 1 : 0;
}

IDXGISwapChain *dhd_mac_swap_chain(uint32_t width, uint32_t height);  /* port/mac/d3d11_mac.c */

/* The window (port/mac/native/window.m) and Direct3D 11 on Metal (port/mac/d3d11_mac.c). */
static int init(void)
{
    uint32_t width, height, mode = window_mode(&width, &height);
    char adapter[128];
    if (!gpu_window_open(width, height, mode)) return 0;
    window = (HWND)1;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level,
                                 &context)) || !(swapchain = dhd_mac_swap_chain(640, 448)))
        return 0;
    gpu_adapter_name(adapter, sizeof adapter);
    hle_log("gs: Direct3D 11 on Metal, %s\n", adapter);
    if (!gs_gpu_init(&gs, device, context)) return 0;
    resize_pending = 1;
    return 1;
}

void gs_window_apply(void)
{
    uint32_t width, height, mode = window_mode(&width, &height);
    if (!window) return;
    gpu_window_mode(width, height, mode);
    resize_pending = 1;
}
#endif

/* The picture's filter and shape changed (DHD_PRESENT_FILTER, DHD_ASPECT): native renderer only. */
void gs_present_options_apply(void)
{
    if (window && gs_gpu_native(&gs)) gs_native_present_options(&gs);
}

static int resize(unsigned width, unsigned height)
{
    static const GUID texture_iid = {0x6F15AAF2,0xD208,0x4E89,{0x9A,0xB4,0x48,0x95,0x35,0xD3,0x4F,0x9C}};
    HRESULT hr;
    if (backbuffer && width == buffer_width && height == buffer_height && !resize_pending) return 1;
    if (backbuffer) { ID3D11Texture2D_Release(backbuffer); backbuffer = NULL; }
    hr = IDXGISwapChain_ResizeBuffers(swapchain, 2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, swap_flags);
    if (SUCCEEDED(hr)) hr = IDXGISwapChain_GetBuffer(swapchain, 0, &texture_iid, (void **)&backbuffer);
    if (FAILED(hr)) { fprintf(stderr, "gs: resize failed (%08lX)\n", hr); return 0; }
    buffer_width = width; buffer_height = height; resize_pending = 0;
    hle_log("gs: scanout %ux%u DISPFB=%llX DISPLAY=%llX\n", width, height,
            gs_priv_read(&gs, GS_DISPFB1), gs_priv_read(&gs, GS_DISPLAY1));
    return 1;
}

/* The picture to save: the native renderer's own (upscaled with DHD_SCALE, so its size), else
   pixels[] (read back from the exact renderer when gpu_frame). */
static int grab(int gpu_frame, const uint32_t **image, unsigned *width, unsigned *height)
{
    if (gs_gpu_native(&gs)) return (*image = gs_native_picture(&gs, width, height)) != NULL;
    *image = pixels;
    return !gpu_frame || gs_gpu_capture(&gs, pixels);
}

static void capture(unsigned width, unsigned height, int gpu_frame)
{
    const uint32_t *image = pixels;
    static unsigned saved;
    const char *path = getenv("DHD_SCREENSHOT"), *at = getenv("DHD_CAPTURE_AT");
    const char *every = getenv("DHD_CAPTURE_INTERVAL");
    const char *frame_at = getenv("DHD_CAPTURE_FRAME");
    extern uint64_t kernel_vsync_count;
    double capture_time = frame_at ? (double)kernel_vsync_count : host_seconds();
    if (frame_at) {
        at = frame_at;
        every = getenv("DHD_CAPTURE_FRAME_INTERVAL");
    }
    double interval = every ? atof(every) : 0;
    char numbered[1024];
    race_sync_vsync(kernel_vsync_count);
    /* DHD_CAPTURE_FIELDS=first-last[,first-last...]: every field of those windows, PATH-fNNNNNN.bmp. */
    const char *fields = getenv("DHD_CAPTURE_FIELDS");
    if (path && fields) {
        unsigned long long first, last;
        int hit = 0;
        for (const char *s = fields; s && sscanf(s, "%llu-%llu", &first, &last) == 2; s = strchr(s, ','), s = s ? s + 1 : s)
            hit |= kernel_vsync_count >= first && kernel_vsync_count <= last;
        if (!hit || !grab(gpu_frame, &image, &width, &height)) return;
        snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        path = numbered;
    } else if (getenv("DHD_RACE_SYNC")) {
        if (!path) return;
        if (!race_sync_capture(path, numbered, sizeof numbered)) {
            /* Survey-only periodic pictures alongside the named race windows.
               DHD_CAPTURE_FRAME is otherwise intentionally ignored in race
               mode. Named consecutive captures always have priority. */
            const char *period = getenv("DHD_RACE_CAPTURE_INTERVAL");
            if (!period || strtoul(period, NULL, 10) == 0) return;
            static unsigned long long next;
            unsigned long long step = strtoul(period, NULL, 10);
            const char *first = getenv("DHD_RACE_CAPTURE_FROM");
            if (!next) next = first ? strtoull(first, NULL, 10) : step;
            if (kernel_vsync_count < next) return;
            next = (kernel_vsync_count / step + 1) * step;
            snprintf(numbered, sizeof numbered, "%s-f%06llu.bmp", path, (unsigned long long)kernel_vsync_count);
        }
        if (!grab(gpu_frame, &image, &width, &height)) return;
        path = numbered;
    } else {
        if (!path || (saved && interval <= 0) ||
            capture_time < (at ? atof(at) : 10.0) + saved * interval) return;
        if (!grab(gpu_frame, &image, &width, &height)) return;
        if (interval > 0) {
            snprintf(numbered, sizeof numbered, "%s-%03u.bmp", path, saved);
            path = numbered;
        }
        saved++;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "gs: cannot save screenshot %s\n", path); return; }
    BITMAPFILEHEADER file = {0};
    BITMAPINFOHEADER info = {0};
    file.bfType = 0x4D42; file.bfOffBits = sizeof file + sizeof info;
    file.bfSize = file.bfOffBits + width * height * 4;
    info.biSize = sizeof info; info.biWidth = width; info.biHeight = -(LONG)height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    fwrite(&file, sizeof file, 1, f); fwrite(&info, sizeof info, 1, f);
    for (unsigned i = 0; i < width * height; i++) {
        uint32_t v = image[i], bgra = (v & 0xFF00FF00) | (v & 255) << 16 | (v >> 16 & 255);
        fwrite(&bgra, 4, 1, f);
    }
    fclose(f);
    fprintf(stderr, "compare: capture %s field=%llu host=%.6f game=%.6f\n", path,
            (unsigned long long)kernel_vsync_count, host_wall_seconds(), host_seconds());
    hle_log("gs: saved %s (%llu PATH1 / %llu PATH2 / %llu PATH3 packets, %llu kicks, %llu uploads)\n",
            path, gs.packets[0], gs.packets[1], gs.packets[2], gs.kicks, gs.uploads);
    hle_log("gs: PMODE=%llX SMODE2=%llX DISPFB1=%llX DISPLAY1=%llX DISPFB2=%llX DISPLAY2=%llX\n",
            gs_priv_read(&gs, GS_PMODE), gs_priv_read(&gs, GS_SMODE2),
            gs_priv_read(&gs, GS_DISPFB1), gs_priv_read(&gs, GS_DISPLAY1),
            gs_priv_read(&gs, GS_DISPFB2), gs_priv_read(&gs, GS_DISPLAY2));
}

volatile uint32_t gs_presented_frames;

#ifdef _WIN32
/* The swap chain's creation flags, which ResizeBuffers must repeat. */
UINT gs_swap_chain_flags(void) { return swap_flags; }

/* Before drawing a picture: until the chain holds no frame (after a present, else at once). */
void gs_frame_latency_wait(void)
{
    if (latency_waitable && InterlockedExchange(&latency_armed, 0))
        WaitForSingleObjectEx(latency_waitable, 100, TRUE);
}

/* Present at once (sync interval 0), tearing allowed unless in exclusive fullscreen. */
HRESULT gs_present(IDXGISwapChain *chain)
{
    UINT flags = 0;
    if (swap_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) {
        BOOL exclusive = FALSE;
        IDXGISwapChain_GetFullscreenState(chain, &exclusive, NULL);
        if (!exclusive) flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT hr = IDXGISwapChain_Present(chain, 0, flags);
    if (latency_waitable) latency_armed = 1;
    return hr;
}
volatile int gs_native_view;

void gs_window_title(const char *utf8)
{
    wchar_t title[256];
    if (!window || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, title, 256)) return;
    SetWindowTextW(window, title);
}

/* EXIT GAME (port/hle/exit_menu.c): the window closes as when its close button is clicked, and
   the game ends at the next gs_pump_messages. */
void gs_window_close(void)
{
    if (window) PostMessageA(window, WM_CLOSE, 0, 0);
    else exit(0);
}

/* The window's own thread handles its messages (window_thread); here the game ends once it closed. */
void gs_pump_messages(void)
{
    if (window_closed) exit(0);
}

#else
UINT gs_swap_chain_flags(void) { return 0; }
void gs_frame_latency_wait(void) {}
HRESULT gs_present(IDXGISwapChain *chain) { return IDXGISwapChain_Present(chain, 0, 0); }
volatile int gs_native_view;
void gs_window_title(const char *utf8)
{
    if (window) gpu_window_title(utf8);
}
void gs_window_close(void)
{
    if (window) gpu_window_close();
    else exit(0);
}
void gs_pump_messages(void)
{
    if (window && gpu_window_closed()) exit(0);
}
#endif

/* DHD_BENCH=first-last times the wall clock between consecutive fields of that virtual-field
   window (run with DHD_FRAME_CLOCK=1 for an unpaced measurement) and prints one summary line
   at the last field; DHD_BENCH_EXIT=1 then ends the process. */
static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

void snd_host_bench(int end);  /* port/hle/sound.c: audio output counters */
void graphics_timeline(int phase, unsigned field, double wall_seconds);  /* DHD_TIMELINE (graphics_queue.c) */

/* CPU time of each named port thread over the window, in ms per field, and of the whole process. */
static void bench_threads(int end, unsigned fields, double wall)
{
    enum { MAX = 16 };
    static const char *names[MAX];
    static double before[MAX], process_before;
    static unsigned count;
    double now[MAX];
    const char *now_names[MAX];
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    double process = (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                      ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
#else
    double process = (double)clock() / CLOCKS_PER_SEC;
#endif
    unsigned n = host_thread_cpu(now_names, now, MAX);
    if (!end) {
        count = n; process_before = process;
        for (unsigned i = 0; i < n; i++) { names[i] = now_names[i]; before[i] = now[i]; }
        return;
    }
    fprintf(stderr, "bench threads: cpu_ms_per_field");
    for (unsigned i = 0; i < n; i++) {
        double start = 0;
        for (unsigned j = 0; j < count; j++) if (names[j] == now_names[i]) start = before[j];
        fprintf(stderr, " %s=%.3f", now_names[i], (now[i] - start) * 1e3 / fields);
    }
    fprintf(stderr, " process=%.3f busy_cores=%.2f\n", (process - process_before) * 1e3 / fields,
            (process - process_before) / wall);
}

static void bench_vsync(void)
{
    static int state;  /* 0 unread, 1 armed, 2 done */
    static unsigned long long first, last;
    static double start, previous, *frames;
    extern uint64_t kernel_vsync_count;
    if (state == 2) return;
    if (!state) {
        const char *window = getenv("DHD_BENCH");
        if (!window || sscanf(window, "%llu-%llu", &first, &last) != 2 || last <= first) { state = 2; return; }
        frames = calloc(last - first, sizeof *frames);
        if (!frames) { state = 2; return; }
        state = 1;
    }
    unsigned long long field = kernel_vsync_count;
    if (field < first) return;
    double now = host_wall_seconds();
    if (field == first) {
        start = previous = now; gs_native_bench(&gs, 0); snd_host_bench(0); bench_threads(0, 0, 0);
        graphics_timeline(0, (unsigned)field, 0);
        return;
    }
    if (field > last) { state = 2; return; }  /* started inside the window: no clean measurement */
    frames[field - first - 1] = now - previous;
    graphics_timeline(field < last ? 1 : 2, (unsigned)field, now - previous);
    previous = now;
    if (field < last) return;
    gs_native_bench(&gs, 1);
    snd_host_bench(1);
    unsigned n = (unsigned)(last - first), worst = n / 100 ? n / 100 : 1;
    bench_threads(1, n, now - start);
    qsort(frames, n, sizeof *frames, compare_double);
    double tail = 0;
    for (unsigned i = n - worst; i < n; i++) tail += frames[i];
    fprintf(stderr, "bench: fields=%u-%llu fields_per_s=%.3f p50_ms=%.3f p99_ms=%.3f worst1pct_ms=%.3f max_ms=%.3f\n",
            (unsigned)first, last, n / (now - start), frames[n / 2] * 1e3, frames[n * 99 / 100] * 1e3,
            tail / worst * 1e3, frames[n - 1] * 1e3);
    fflush(stderr);
    free(frames);
    state = 2;
    const char *quit = getenv("DHD_BENCH_EXIT");
    if (quit && !strcmp(quit, "1")) ExitProcess(0);
}

const uint32_t *movie_picture(unsigned *width, unsigned *height);  /* port/hle/movie.c */

/* The graphics didn't start. The game would go on with no window (heard, never seen) behind a
   start window stuck on its graphics step: instead the start window closes, a message says why
   and the game ends. Tools that want no window set DHD_HEADLESS and never get here. */
static void graphics_failed(void)
{
    D3D11_FEATURE_DATA_DOUBLES doubles = {0};
    int no_doubles = device && (FAILED(ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_DOUBLES, &doubles, sizeof doubles)) ||
                                !doubles.DoublePrecisionFloatShaderOps);
    hle_log("gs: the graphics didn't start%s\n", no_doubles ? " (no double-precision shaders)" : "");
    splash_close();
    char message[1024];
    snprintf(message, sizeof message, "Downhill Revived couldn't start its graphics on this PC.\n\n%s"
             "On a laptop with two graphics chips, open Windows Settings > System > Display > Graphics, "
             "add dhd.exe and choose High performance.\n\nThe details are in dhd.log, next to dhd.exe.",
             no_doubles ? "Its renderer needs DirectX 11 with double-precision shaders, and this graphics "
                          "chip doesn't offer them.\n\n" : "");
    MessageBoxA(NULL, message, "Downhill Revived", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    exit(1);
}

void gs_vsync(void)
{
    static int profile, gpu_trace;
    static int cpu_scanout;
    static double profile_start;
    static unsigned fields, previous_frames, previous_presented, previous_bp = ~0u;
    if (!initialized) {
        initialized = 1;
        disabled = getenv("DHD_HEADLESS") != NULL;
        if (!disabled && !init()) graphics_failed();
        profile = getenv("DHD_PROFILE") != NULL;
        gpu_trace = getenv("DHD_GPU_TRACE") != NULL;
        cpu_scanout = getenv("DHD_GS_CPU_SCANOUT") != NULL;
        profile_start = host_wall_seconds();
    }
    bench_vsync();
    if (disabled) return;
    gs_pump_messages();
    /* Plain MMIO pages are the authoritative privileged register storage. */
    for (unsigned addr = 0; addr <= 0xE0; addr += 16)
        gs_priv_write(&gs, addr, *(volatile uint64_t *)(uintptr_t)(0x12000000 + addr));
    /* Count changed display buffers, excluding repeated interlaced fields.
       The game's menu counter does not cover every track-intro render path. */
    uint64_t pmode = gs_priv_read(&gs, GS_PMODE);
    unsigned bp = (unsigned)gs_priv_read(&gs, pmode & 1 ? GS_DISPFB1 : GS_DISPFB2) & 511;
    if ((pmode & 3) && bp != previous_bp) { gs_presented_frames++; previous_bp = bp; }
    unsigned width = 640, height = 448;
    int gpu_frame = 1;
    /* While a movie plays (port/hle/movie.c, finding 055) its picture is shown instead. */
    unsigned movie_width, movie_height;
    const uint32_t *movie = movie_picture(&movie_width, &movie_height);
    if (gs_gpu_native(&gs)) {
        /* The native renderer's thread draws and presents; capture waits for its picture. */
        gs_native_movie(&gs, movie, movie_width, movie_height);
        gs_native_vsync(&gs, swapchain, &width, &height, resize_pending);
        resize_pending = 0;
    } else {
        gpu_frame = !cpu_scanout && gs_gpu_display(&gs, &width, &height);
        if (movie) {
            gpu_frame = 0;
            width = movie_width, height = movie_height;
            memcpy(pixels, movie, (size_t)width * height * 4);
        } else if (!gpu_frame) {
            if (!gs_display(&gs, pixels, &width, &height)) memset(pixels, 0, width * height * 4);
        }
        if (!resize(width, height)) return;
        gs_frame_latency_wait();
        if (gpu_frame) gs_gpu_copy_display(&gs, backbuffer);
        else ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)backbuffer, 0, NULL, pixels, width * 4, 0);
        if (gpu_trace) gs_gpu_trace_present(&gs, 0, 0);
        double due, began;
        pacing_handed(&due, &began);
        pacing_present(due, began);
        HRESULT hr = gs_present(swapchain);
        if (gpu_trace) gs_gpu_trace_present(&gs, 1, host_wall_seconds());
        if (FAILED(hr)) { fprintf(stderr, "gs: presentation failed (%08lX)\n", hr); disabled = 1; }
    }
    capture(width, height, gpu_frame);
    gs_capture_vsync(&gs);
    if (profile) {
        fields++;
        double now = host_wall_seconds(), elapsed = now - profile_start;
        if (elapsed >= 5) {
            unsigned frames = *(volatile unsigned *)PS2_HOST(0x29E10C);
            if (frames >= previous_frames)
                fprintf(stderr, "profile: %.2f fields/s, %.2f menu frames/s, %.2f display-buffer changes/s\n", fields / elapsed,
                        (frames - previous_frames) / elapsed, (gs_presented_frames - previous_presented) / elapsed);
            else fprintf(stderr, "profile: %.2f fields/s, frame counter reset\n", fields / elapsed);
            gs_gpu_profile(&gs, elapsed);
            previous_frames = frames; fields = 0; profile_start = now;
            previous_presented = gs_presented_frames;
        }
    }
}
