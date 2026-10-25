/* The Mac program's user interface (finding 075): the AppKit application on the main thread,
   the game's window (a view with a CAMetalLayer, which port/mac/native/metal.m presents to), its
   keyboard, messages, the open panel, the pasteboard and the display. The game runs on a thread of
   its own (main.c); what it asks of AppKit runs on the main thread. */
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <CoreGraphics/CoreGraphics.h>
#include <stdatomic.h>
#include "native.h"

struct w2c_gpu;
id<MTLDevice> metal_device(void);

static NSWindow *window;
static CAMetalLayer *layer;
static _Atomic uint32_t client_width, client_height, closed;
static _Atomic uint8_t keys[256];  /* by Windows virtual-key code */
static int app_running;

CAMetalLayer *window_layer(void) { return layer; }

/* Mac key codes (the place on the keyboard) as Windows virtual-key codes, letters and digits as on
   a US keyboard: port/hle/keyboard.c reads keys by place. */
static uint8_t windows_key(unsigned short code)
{
    static const uint8_t map[128] = {
        [0x00] = 'A', [0x01] = 'S', [0x02] = 'D', [0x03] = 'F', [0x04] = 'H', [0x05] = 'G', [0x06] = 'Z', [0x07] = 'X',
        [0x08] = 'C', [0x09] = 'V', [0x0B] = 'B', [0x0C] = 'Q', [0x0D] = 'W', [0x0E] = 'E', [0x0F] = 'R', [0x10] = 'Y',
        [0x11] = 'T', [0x12] = '1', [0x13] = '2', [0x14] = '3', [0x15] = '4', [0x16] = '6', [0x17] = '5', [0x19] = '9',
        [0x1A] = '7', [0x1C] = '8', [0x1D] = '0', [0x1F] = 'O', [0x20] = 'U', [0x22] = 'I', [0x23] = 'P',
        [0x24] = 0x0D /* Return */, [0x25] = 'L', [0x26] = 'J', [0x28] = 'K', [0x2D] = 'N', [0x2E] = 'M',
        [0x30] = 0x09 /* Tab */, [0x31] = 0x20 /* Space */, [0x33] = 0x08 /* Delete: Backspace */,
        [0x35] = 0x1B /* Escape */, [0x38] = 0x10, [0x3C] = 0x10 /* Shift */, [0x3B] = 0x11, [0x3E] = 0x11 /* Control */,
        [0x3A] = 0x12, [0x3D] = 0x12 /* Option: Alt */, [0x37] = 0x5B, [0x36] = 0x5C /* Command: Windows */,
        [0x4C] = 0x0D /* keypad Enter */, [0x7B] = 0x25, [0x7C] = 0x27, [0x7D] = 0x28, [0x7E] = 0x26 /* arrows */,
    };
    return code < 128 ? map[code] : 0;
}

@interface GameView : NSView
@end

@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer
{
    layer = [CAMetalLayer layer];
    layer.device = metal_device();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = 2;
    const char *sync = getenv("DHD_VSYNC");
    layer.displaySyncEnabled = !sync || atoi(sync);
    return layer;
}
- (void)updateSize
{
    CGFloat scale = self.window.backingScaleFactor ? self.window.backingScaleFactor : 1;
    CGSize size = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
    layer.contentsScale = scale;
    layer.drawableSize = size;
    atomic_store(&client_width, (uint32_t)size.width);
    atomic_store(&client_height, (uint32_t)size.height);
}
- (void)setFrameSize:(NSSize)size { [super setFrameSize:size]; [self updateSize]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self updateSize]; }
- (void)keyDown:(NSEvent *)e { uint8_t k = windows_key(e.keyCode); if (k) atomic_store(&keys[k], 1); }
- (void)keyUp:(NSEvent *)e { uint8_t k = windows_key(e.keyCode); if (k) atomic_store(&keys[k], 0); }
- (void)flagsChanged:(NSEvent *)e
{
    NSEventModifierFlags f = e.modifierFlags;
    atomic_store(&keys[0x10], (f & NSEventModifierFlagShift) != 0);
    atomic_store(&keys[0x11], (f & NSEventModifierFlagControl) != 0);
    atomic_store(&keys[0x12], (f & NSEventModifierFlagOption) != 0);
    atomic_store(&keys[0x5B], (f & NSEventModifierFlagCommand) != 0);
}
@end

@interface WindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation WindowDelegate
- (void)windowWillClose:(NSNotification *)n { (void)n; atomic_store(&closed, 1); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; for (int i = 0; i < 256; i++) atomic_store(&keys[i], 0); }
@end

static WindowDelegate *delegate;

/* Runs block on the main thread, where AppKit lives; at once when there is no application (the
   headless build's tools). */
static void on_main(dispatch_block_t block)
{
    if (!app_running || [NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

/* mode: 0 windowed (width x height points), 1 borderless and 2 fullscreen: the whole screen, in
   a fullscreen space of its own. */
static void apply_mode(uint32_t width, uint32_t height, uint32_t mode)
{
    BOOL full = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (mode && !full) [window toggleFullScreen:nil];
    else if (!mode) {
        if (full) [window toggleFullScreen:nil];
        [window setContentSize:NSMakeSize(width ? width : 896, height ? height : 672)];
        [window center];
    }
}

u32 w2c_gpu_window_open(struct w2c_gpu *unused, u32 width, u32 height, u32 mode)
{
    (void)unused;
    if (!app_running) return 0;
    on_main(^{
        NSRect frame = NSMakeRect(0, 0, width ? width : 896, height ? height : 672);
        window = [[NSWindow alloc] initWithContentRect:frame
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                               backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Downhill Revived";
        window.releasedWhenClosed = NO;
        window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        delegate = [WindowDelegate new];
        window.delegate = delegate;
        GameView *view = [[GameView alloc] initWithFrame:frame];
        view.wantsLayer = YES;
        window.contentView = view;
        [view updateSize];
        [window center];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:view];
        [NSApp activateIgnoringOtherApps:YES];
        apply_mode(width, height, mode);
    });
    return window != nil;
}

void w2c_gpu_window_mode(struct w2c_gpu *unused, u32 width, u32 height, u32 mode)
{
    (void)unused;
    if (window) on_main(^{ apply_mode(width, height, mode); });
}

void w2c_gpu_window_title(struct w2c_gpu *unused, u32 utf8)
{
    (void)unused;
    NSString *title = @(string_at(utf8));
    if (window && title) on_main(^{ window.title = title; });
}

u32 w2c_gpu_window_closed(struct w2c_gpu *unused) { (void)unused; return atomic_load(&closed); }

void w2c_gpu_window_close(struct w2c_gpu *unused)
{
    (void)unused;
    if (window) on_main(^{ [window close]; });
    else atomic_store(&closed, 1);
}

void w2c_mac_window_size(struct w2c_mac *unused, u32 width, u32 height)
{
    (void)unused;
    uint32_t w = atomic_load(&client_width), h = atomic_load(&client_height);
    memcpy(at(width), &w, 4);
    memcpy(at(height), &h, 4);
}

u32 w2c_mac_key_down(struct w2c_mac *unused, u32 vk)
{
    (void)unused;
    return vk < 256 && atomic_load(&keys[vk]);
}

/* ---- messages, the open panel, the pasteboard, the display ---- */

u32 w2c_mac_message_box(struct w2c_mac *unused, u32 text, u32 caption, u32 flags)
{
    (void)unused;
    fprintf(stderr, "[%s] %s\n", string_at(caption), string_at(text));
    if (!app_running) return (flags & 0xF) == 4 /* MB_YESNO */ ? 6 /* IDYES */ : 1 /* IDOK */;
    __block NSModalResponse answer = NSAlertFirstButtonReturn;
    NSString *message = @(string_at(text)), *title = @(string_at(caption));
    on_main(^{
        NSAlert *a = [NSAlert new];
        a.messageText = title;
        a.informativeText = message;
        a.alertStyle = (flags & 0xF0) == 0x10 ? NSAlertStyleCritical : (flags & 0xF0) == 0x30 ? NSAlertStyleWarning
                                                                                            : NSAlertStyleInformational;
        switch (flags & 0xF) {
        case 1: [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"]; break;
        case 4: [a addButtonWithTitle:@"Yes"]; [a addButtonWithTitle:@"No"]; break;
        case 5: [a addButtonWithTitle:@"Retry"]; [a addButtonWithTitle:@"Cancel"]; break;
        default: [a addButtonWithTitle:@"OK"];
        }
        [NSApp activateIgnoringOtherApps:YES];
        answer = [a runModal];
    });
    BOOL first = answer == NSAlertFirstButtonReturn;
    switch (flags & 0xF) {
    case 1: return first ? 1 /* IDOK */ : 2 /* IDCANCEL */;
    case 4: return first ? 6 /* IDYES */ : 7 /* IDNO */;
    case 5: return first ? 4 /* IDRETRY */ : 2;
    default: return 1;
    }
}

u32 w2c_mac_choose_file(struct w2c_mac *unused, u32 title, u32 extension, u32 out, u32 size)
{
    (void)unused;
    if (!app_running) {
        fprintf(stderr, "dhd: %s: set DHD_ISO to its path\n", string_at(title));
        return 0;
    }
    __block NSString *chosen = nil;
    NSString *prompt = @(string_at(title)), *kind = @(string_at(extension));
    on_main(^{
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.message = prompt;
        p.canChooseDirectories = NO;
        p.allowsMultipleSelection = NO;
        p.allowedFileTypes = @[kind];
        [NSApp activateIgnoringOtherApps:YES];
        if ([p runModal] == NSModalResponseOK) chosen = p.URL.path;
    });
    if (!chosen || strlen(chosen.UTF8String) + 1 > size) return 0;
    memcpy(at(out), chosen.UTF8String, strlen(chosen.UTF8String) + 1);
    return 1;
}

u32 w2c_mac_clipboard_utf16(struct w2c_mac *unused, u32 out, u32 size)
{
    (void)unused;
    __block NSString *text = nil;
    on_main(^{ text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString]; });
    NSUInteger n = MIN(text.length, (NSUInteger)size);
    if (n) [text getCharacters:(unichar *)at(out) range:NSMakeRange(0, n)];
    return (u32)n;
}

void w2c_mac_display(struct w2c_mac *unused, u32 width, u32 height, u32 refresh)
{
    (void)unused;
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(d);
    uint32_t w = mode ? (uint32_t)CGDisplayModeGetPixelWidth(mode) : 1920;
    uint32_t h = mode ? (uint32_t)CGDisplayModeGetPixelHeight(mode) : 1080;
    double hz = mode ? CGDisplayModeGetRefreshRate(mode) : 60;
    if (mode) CGDisplayModeRelease(mode);
    if (hz <= 0) hz = 60;  /* built-in panels report 0 */
    uint32_t r = (uint32_t)(hz + 0.5);
    memcpy(at(width), &w, 4); memcpy(at(height), &h, 4); memcpy(at(refresh), &r, 4);
}

/* ---- the application ---- */

/* Runs AppKit on the main thread until the game's thread ends the process (main.c); start runs
   once the application is up, so the game finds it when it opens its window. */
void window_run_application(void (*start)(void))
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        NSMenu *bar = [NSMenu new], *menu = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new];
        [menu addItemWithTitle:@"Quit Downhill Revived" action:@selector(terminate:) keyEquivalent:@"q"];
        item.submenu = menu;
        [bar addItem:item];
        NSApp.mainMenu = bar;
        app_running = 1;
        /* no App Nap or timer coalescing: the frame pacing waits to the millisecond */
        static id activity;
        activity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                              reason:@"Running the game"];
        [NSApp finishLaunching];
        start();
        [NSApp run];
    }
}
