/* Scanout and the window. GS drawing and texture transfers share local memory; presentation
   reads the framebuffer selected by the game's DISPFB/DISPLAY MMIO registers, and the renderer
   (gpu.h, the Rust renderer) presents it on its surface on the window. */
#ifdef _WIN32
#include <windows.h>
#else
#include "../host/win32_compat.h"
#endif
/* The window is SDL's (W11): created on the SDL thread (host/sdl_host.c), its events come there too.
   The renderer's surface takes the window's HWND (SDL_PROP_WINDOW_WIN32_HWND_POINTER); on macOS
   the CAMetalLayer of an SDL Metal view. */
#if defined(_WIN32) || defined(DHD_HAVE_SDL)
#include <SDL3/SDL.h>
#define PRESENT_SDL 1
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "present.h"
#include "gpu.h"
#include "capture.h"
#include "../hle/hle.h"
#include "../include/guest.h"
#include "../hle/text_input.h"
#include "../hle/splash.h"

static uint32_t pixels[1024 * 1024];
static int initialized, disabled, resize_pending;
static int surface;                 /* the renderer presents to the window (init) */
static volatile int window_closed;  /* the close button (or EXIT GAME): the game ends at the next field */
#ifdef PRESENT_SDL
static SDL_Window *sdl_window;
/* The window lives on the SDL thread (sdl_host.c): its messages and a modal drag or resize stop
   that thread only; the game thread still draws and presents to the window, and still ends the
   process when the window closes (gs_pump_messages). */
typedef struct { int x, y, w, h, borderless; } WindowSpec;
static void display_of(SDL_Window *w, SDL_Rect *full, SDL_Rect *usable);

/* The primary display's resolution: its desktop mode, the one Windows runs it at (the largest
   fullscreen mode can be bigger than the screen: NVIDIA DSR lists 2560x1440 on a 1080p monitor),
   else SDL's largest fullscreen mode. Pixels (host_dpi_aware). 0 x 0 when SDL knows none. */
static void monitor_mode(int *w, int *h)
{
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(id);
    *w = *h = 0;
    if (desktop) *w = desktop->w, *h = desktop->h;
    if (*w > 0 && *h > 0) return;
    int count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(id, &count);
    if (modes && count > 0 && modes[0]) *w = modes[0]->w, *h = modes[0]->h;
    SDL_free(modes);
}

/* DHD_MONITOR_MODE: the monitor's resolution, for PC SETTINGS > Window Size's list. A window
   size of "monitor" (dhd.ini's default, host.c player_defaults) becomes that size, in numbers, so
   everything after (gs_window_apply, the settings page) reads the size it has. SDL thread, before
   the window is made: the EE thread waits in sdl_host_run. */
static void monitor_size(WindowSpec *s)
{
    int w, h;
    char text[16];
    monitor_mode(&w, &h);
    if (w <= 0 || h <= 0) return;
    snprintf(text, sizeof text, "%dx%d", w, h);
    host_putenv_s("DHD_MONITOR_MODE", text);
    const char *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int named = (width && !strcmp(width, "monitor")) || (height && !strcmp(height, "monitor"));
    if (!named) return;
    snprintf(text, sizeof text, "%d", w);
    host_putenv_s("DHD_WINDOW_WIDTH", text);
    snprintf(text, sizeof text, "%d", h);
    host_putenv_s("DHD_WINDOW_HEIGHT", text);
    s->w = w, s->h = h;
    hle_log("gs: window size %dx%d, the monitor's resolution\n", w, h);
}

/* On the SDL thread: the window, hidden until init has its swap chain (sdl_host_run). */
static void make_window(void *arg)
{
    WindowSpec *s = arg;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "gs: SDL video did not start (%s)\n", SDL_GetError());
        return;
    }
    monitor_size(s);
    if (s->borderless) {  /* the whole primary monitor: SDL knows its bounds only once its video runs */
        SDL_Rect full, usable;
        display_of(NULL, &full, &usable);
        s->x = full.x, s->y = full.y, s->w = full.w, s->h = full.h;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (s->borderless ? SDL_WINDOW_BORDERLESS : SDL_WINDOW_RESIZABLE);
    /* A minimised start: SDL applies its window state when the window shows, so it must know
       (show_window); otherwise it restores the window then. */
    if (start_minimised()) flags |= SDL_WINDOW_MINIMIZED;
#ifdef __APPLE__
    flags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;  /* the renderer's Metal layer, in real pixels */
#endif
    sdl_window = SDL_CreateWindow("Downhill Revived", s->w, s->h, flags);  /* s->w x s->h: the client area */
    if (!sdl_window) {
        fprintf(stderr, "gs: SDL window (%s)\n", SDL_GetError());
        return;
    }
    if (s->borderless) SDL_SetWindowPosition(sdl_window, s->x, s->y);
    hle_log("gs: SDL window %dx%d (video %s)\n", s->w, s->h, SDL_GetCurrentVideoDriver());
}

/* A display's whole area and its usable part (without the taskbar), in pixels (host_dpi_aware). */
static void display_of(SDL_Window *w, SDL_Rect *full, SDL_Rect *usable)
{
    SDL_DisplayID id = w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay();
    if (!SDL_GetDisplayBounds(id, full)) {
        full->x = full->y = 0; full->w = 896; full->h = 672;
    }
    if (!SDL_GetDisplayUsableBounds(id, usable)) *usable = *full;
}
#endif
#ifdef _WIN32
static HWND window;                 /* sdl_window's HWND, for the renderer's surface */
/* Laptops with two graphics chips run a program they don't know on the integrated one; these
   exports ask the NVIDIA and AMD drivers for the dedicated chip, as most PC games do. In the exe
   (this file is host code). */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* The game's window appears, in front, or minimised and without the focus when the game was
   started minimised (splash.c's start_minimised: test and recording runs on a PC in use). */
static void show_window(void)
{
    int minimised = start_minimised();
    hle_log("gs: window shown%s\n", minimised ? " minimised (a minimised start)" : "");
    if (minimised) {  /* created minimised (make_window); shown without taking the focus */
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_ShowWindow(sdl_window);
        ShowWindow(window, SW_SHOWMINNOACTIVE);
    } else { SDL_ShowWindow(sdl_window); SDL_RaiseWindow(sdl_window); }
}

/* The window (SDL's), then the renderer (gpu_rust.c: wgpu on Direct3D 12, or Vulkan; WARP when
   there is no graphics driver) and its surface on the window, which is shown once both run. */
static int init(void)
{
    host_dpi_aware();  /* real pixels: a borderless window at the monitor's own resolution (before SDL's video) */
    /* DHD_WINDOW: windowed (default; DHD_WINDOW_WIDTH x DHD_WINDOW_HEIGHT inside, 896x672),
       borderless or fullscreen (both the whole monitor, borderless: gs_window_apply). */
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int borderless = mode && !strcmp(mode, "borderless"), fullscreen = mode && !strcmp(mode, "fullscreen");
    WindowSpec spec = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672,
                       borderless || fullscreen};
    sdl_host_run(make_window, &spec);  /* on the SDL thread: the window's messages are its queue's */
    if (!sdl_window) return 0;
    window = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!window) return 0;
    /* the icon (port/res/dhd.rc), as the window class had it: SDL sets none */
    HICON icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    SendMessageA(window, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessageA(window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    splash_step(75, "Preparing the graphics...");
    if (!gs_gpu_init(&gs)) return 0;
    if (!gs_gpu_surface(&gs, window)) {
        hle_log("gs: the renderer has no surface on the window\n");
        gs_gpu_destroy(&gs);
        return 0;
    }
    surface = 1;
    hle_log("gs: WebGPU presentation (%s)\n", gs_gpu_backend_name(&gs));
    show_window();
    splash_close();
    if (fullscreen) gs_window_apply();  /* the whole monitor */
    return 1;
}
#elif defined(__APPLE__) && defined(PRESENT_SDL)
/* macOS (finding 091): the window is SDL's, on the main thread (sdl_host.c's sdl_host_main); the
   renderer (gpu_rust.c: wgpu on Metal) draws into the CAMetalLayer of an SDL Metal view on it.
   Every window call goes to that thread (sdl_host_run). Borderless and fullscreen are macOS's
   own fullscreen (a Space of its own, the menu bar hidden). */
static SDL_MetalView metal_view;
static void *metal_layer;

static void make_metal_view(void *unused)
{
    (void)unused;
    metal_view = SDL_Metal_CreateView(sdl_window);
    metal_layer = metal_view ? SDL_Metal_GetLayer(metal_view) : NULL;
}

static void show_window(void *unused)
{
    (void)unused;
    SDL_ShowWindow(sdl_window);
    SDL_RaiseWindow(sdl_window);
}

/* The window's drawable size in pixels (gpu_rust.c's GetClientRect): 0 x 0 while minimised. */
void gs_window_pixels(unsigned *width, unsigned *height)
{
    int w = 0, h = 0;
    if (sdl_window && !(SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MINIMIZED))
        SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
    *width = w > 0 ? (unsigned)w : 0;
    *height = h > 0 ? (unsigned)h : 0;
}

static int init(void)
{
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int fullscreen = mode && (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen"));
    WindowSpec spec = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672, 0};
    if (getenv("DHD_HEADLESS")) return 0;  /* headless runs never open a window */
    sdl_host_run(make_window, &spec);  /* on the main thread: Cocoa's windows live there */
    if (!sdl_window) return 0;
    sdl_host_run(make_metal_view, NULL);
    if (!metal_layer) {
        fprintf(stderr, "gs: no Metal view on the window (%s)\n", SDL_GetError());
        return 0;
    }
    splash_step(75, "Preparing the graphics...");
    if (!gs_gpu_init(&gs)) return 0;
    if (!gs_gpu_surface(&gs, metal_layer)) {
        hle_log("gs: the renderer has no surface on the window\n");
        gs_gpu_destroy(&gs);
        return 0;
    }
    surface = 1;
    hle_log("gs: WebGPU presentation (%s)\n", gs_gpu_backend_name(&gs));
    sdl_host_run(show_window, NULL);
    splash_close();
    if (fullscreen) gs_window_apply();
    return 1;
}
#elif defined(PRESENT_SDL)
/* No presentation device on this host (Linux: the renderer has no surface here yet). The SDL
   window still opens for input, and audio still plays; rendering stays headless, so init reports
   no device and gs_vsync keeps its bench path. */
static int init(void)
{
    const char *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    WindowSpec spec = {0, 0, width && atoi(width) > 0 ? atoi(width) : 896,
                       height && atoi(height) > 0 ? atoi(height) : 672, 0};
    if (getenv("DHD_HEADLESS")) return 0;  /* headless runs never open a window */
    sdl_host_run(make_window, &spec);  /* on the SDL thread: the window's messages are its queue's */
    if (!sdl_window) return 0;
    SDL_ShowWindow(sdl_window);
    hle_log("gs: no renderer on this host yet (headless scanout)\n");
    return 0;  /* no renderer: gs_vsync stays disabled */
}
#else
/* No presentation device: init fails, so gs_vsync stays disabled (bench only). */
static int init(void) { return 0; }
#endif

/* The PC settings page changed DHD_WINDOW, DHD_WINDOW_WIDTH or DHD_WINDOW_HEIGHT: the window
   mode and size now, as init sets them up (fullscreen is the whole monitor, borderless). EE thread
   (it owns the window, so the messages these calls send are handled at once). The renderer's
   surface follows at the next present (WM_SIZE). */
#ifdef _WIN32
void gs_window_apply(void)
{
    if (!sdl_window || !surface) return;
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    int borderless = mode && !strcmp(mode, "borderless"), fullscreen = mode && !strcmp(mode, "fullscreen");
    SDL_Rect monitor, usable;
    display_of(sdl_window, &monitor, &usable);
    /* The window's own calls, from this thread as before (the SDL thread gets its messages). */
    if (borderless || fullscreen) {
        SDL_SetWindowBordered(sdl_window, false);
        SDL_SetWindowSize(sdl_window, monitor.w, monitor.h);
        SDL_SetWindowPosition(sdl_window, monitor.x, monitor.y);
        if (!IsIconic(window)) SDL_ShowWindow(sdl_window);  /* a minimised start stays so */
    } else {
        int w = width && atoi(width) > 0 ? atoi(width) : 896, h = height && atoi(height) > 0 ? atoi(height) : 672;
        SDL_SetWindowBordered(sdl_window, true);
        SDL_SetWindowSize(sdl_window, w, h);
        int top = 0, left = 0, bottom = 0, right = 0;
        SDL_GetWindowBordersSize(sdl_window, &top, &left, &bottom, &right);
        int outer_w = w + left + right, outer_h = h + top + bottom;  /* centred on the usable area */
        int x = usable.x + (usable.w - outer_w) / 2, y = usable.y + (usable.h - outer_h) / 2;
        SDL_SetWindowPosition(sdl_window, x > usable.x ? x : usable.x, y > usable.y ? y : usable.y);
        if (!IsIconic(window)) SDL_ShowWindow(sdl_window);
    }
    resize_pending = 1;
}
#elif defined(__APPLE__) && defined(PRESENT_SDL)
static void window_apply(void *unused)
{
    (void)unused;
    const char *mode = getenv("DHD_WINDOW"), *width = getenv("DHD_WINDOW_WIDTH"), *height = getenv("DHD_WINDOW_HEIGHT");
    if (mode && (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen"))) {
        SDL_SetWindowFullscreen(sdl_window, true);  /* the desktop's mode: no mode change */
    } else {
        SDL_SetWindowFullscreen(sdl_window, false);
        SDL_SetWindowSize(sdl_window, width && atoi(width) > 0 ? atoi(width) : 896, height && atoi(height) > 0 ? atoi(height) : 672);
        SDL_SetWindowPosition(sdl_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void gs_window_apply(void)
{
    if (!sdl_window || !surface) return;
    sdl_host_run(window_apply, NULL);  /* Cocoa: on the main thread */
    resize_pending = 1;
}
#else
void gs_window_apply(void) {}
#endif

/* The picture's filter and shape changed (DHD_PRESENT_FILTER, DHD_ASPECT): native renderer only. */
void gs_present_options_apply(void)
{
#if defined(_WIN32) || (defined(__APPLE__) && defined(PRESENT_SDL))
    if (sdl_window && gs_gpu_native(&gs)) gs_native_present_options(&gs);
#endif
}

/* The picture to save: the native renderer's own (upscaled with DHD_SCALE, so its size), else
   pixels[] (read back from the exact renderer when gpu_frame). */
static int grab(int gpu_frame, const uint32_t **image, unsigned *width, unsigned *height)
{
    if (gs_gpu_native(&gs)) return (*image = gs_native_picture(&gs, width, height)) != NULL;
    *image = pixels;
    return !gpu_frame || gs_gpu_capture(&gs, pixels);
}

static char published_capture_name[1024];
static int published_capture_ready;
void host_capture_name(uint32_t address, int ready) {
    published_capture_ready=ready;
    if (ready) snprintf(published_capture_name,sizeof published_capture_name,"%s",(const char *)GUEST(address));
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
        if (published_capture_ready) snprintf(numbered,sizeof numbered,"%s",published_capture_name);
        if (!published_capture_ready) {
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
#ifdef _WIN32
    BITMAPFILEHEADER file = {0};
    BITMAPINFOHEADER info = {0};
#else
    /* The BMP headers, packed as the format requires. */
    struct __attribute__((packed)) {
        uint16_t bfType;
        uint32_t bfSize;
        uint16_t bfReserved1, bfReserved2;
        uint32_t bfOffBits;
    } file = {0};
    struct __attribute__((packed)) {
        uint32_t biSize;
        int32_t biWidth, biHeight;
        uint16_t biPlanes, biBitCount;
        uint32_t biCompression, biSizeImage;
        int32_t biXPelsPerMeter, biYPelsPerMeter;
        uint32_t biClrUsed, biClrImportant;
    } info = {0};
    _Static_assert(sizeof file == 14, "BMP file header is 14 bytes");
    _Static_assert(sizeof info == 40, "BMP info header is 40 bytes");
#define BI_RGB 0
#endif
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

volatile int gs_native_view;
void host_service_gs_native_view(int view) { gs_native_view = view; }

void gs_window_title(const char *utf8)
{
#ifdef PRESENT_SDL
    if (sdl_window) SDL_SetWindowTitle(sdl_window, utf8);
#else
    (void)utf8;
#endif
}

/* The window's events, from the SDL thread (sdl_host.c): the size changed (WM_SIZE: the back buffer
   follows at the next present), or the window was asked to close (WM_CLOSE: the game ends at the
   next gs_pump_messages). */
void gs_window_event(unsigned type)
{
#ifdef PRESENT_SDL
    if (type == SDL_EVENT_WINDOW_RESIZED) resize_pending = 1;
    else if (type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || type == SDL_EVENT_QUIT) {
        if (sdl_window) SDL_HideWindow(sdl_window);
        window_closed = 1;
    }
#else
    (void)type;
#endif
}

/* EXIT GAME (port/hle/exit_menu.c): the window closes as when its close button is clicked, and
   the game ends at the next gs_pump_messages. */
void gs_window_close(void)
{
#ifdef PRESENT_SDL
    if (sdl_window) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        e.window.windowID = SDL_GetWindowID(sdl_window);
        SDL_PushEvent(&e);  /* the SDL thread sees it: gs_window_event */
        return;
    }
#endif
    exit(0);
}

/* The window's events are the SDL thread's (sdl_host.c); here the game ends once the window closed. */
void gs_pump_messages(void)
{
    if (window_closed) exit(0);
}

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
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    double process = (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                      ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
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
    hle_log("gs: the graphics didn't start\n");
    splash_close();
    const char *message = "Downhill Revived couldn't start its graphics on this PC.\n\n"
                          "Its renderer needs Direct3D 12 or Vulkan (Windows 10 or 11 with a current graphics driver). "
                          "On a laptop with two graphics chips, open Windows Settings > System > Display > Graphics, "
                          "add the game and choose High performance.\n\nThe details are in dhd.log, next to the game.";
    host_error_box("Downhill Revived", message);
    exit(1);
}

void gs_vsync(void)
{
    static int profile;
    static int cpu_scanout;
    static double profile_start;
    static unsigned fields, previous_frames, previous_presented, previous_bp = ~0u;
    if (!initialized) {
        initialized = 1;
        disabled = getenv("DHD_HEADLESS") != NULL;
        if (!disabled && !init()) graphics_failed();
        profile = getenv("DHD_PROFILE") != NULL;
        cpu_scanout = getenv("DHD_GS_CPU_SCANOUT") != NULL;
        profile_start = host_wall_seconds();
    }
    bench_vsync();
    gs_pump_messages();  /* first: the close button ends the game even with no renderer */
    if (disabled) return;
    /* Plain MMIO pages are the authoritative privileged register storage. */
    for (unsigned addr = 0; addr <= 0xE0; addr += 16)
        gs_priv_write(&gs, addr, *(volatile uint64_t *)GUEST(0x12000000 + addr));
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
        gs_native_vsync(&gs, &width, &height, resize_pending);
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
        /* the renderer shows the picture itself, stretched over the client area */
        unsigned client_width = 0, client_height = 0;
#ifdef _WIN32
        RECT client = {0};
        GetClientRect(window, &client);
        client_width = (unsigned)client.right; client_height = (unsigned)client.bottom;
#elif defined(__APPLE__) && defined(PRESENT_SDL)
        gs_window_pixels(&client_width, &client_height);
#endif
        double due, began;
        pacing_handed(&due, &began);
        pacing_present(due, began);
        if (!gs_gpu_present(&gs, gpu_frame ? NULL : pixels, width, height, client_width, client_height)) {
            fprintf(stderr, "gs: WebGPU presentation failed\n");
            disabled = 1;
        }
    }
    capture(width, height, gpu_frame);
    gs_capture_vsync(&gs);
    if (profile) {
        fields++;
        double now = host_wall_seconds(), elapsed = now - profile_start;
        if (elapsed >= 5) {
            unsigned frames = *(volatile unsigned *)GUEST(0x0829E10C);
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

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_gs_vsync(void) { gs_vsync(); }
void host_service_gs_pump_messages(void) { gs_pump_messages(); }
void host_service_gs_window_title(uint32_t a0) { gs_window_title((const char *)GUEST(a0)); }
void host_service_gs_window_apply(void) { gs_window_apply(); }
void host_service_gs_present_options_apply(void) { gs_present_options_apply(); }
void host_service_gs_window_close(void) { gs_window_close(); }
