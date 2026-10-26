/* The macOS host's dialogs (finding 091): the message boxes and the disc image's file-open dialog
   host.c shows before the game starts (host_disc_ready), as Windows' MessageBoxA and
   GetOpenFileNameA do. Cocoa wants them on the main thread, which is SDL's (sdl_host.c), so each
   runs there through sdl_host_run, after SDL's video (and with it the application) is up. */
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include "host_mac.h"

int sdl_host_run(void (*fn)(void *), void *arg);

typedef struct {
    const char *title, *text, *extension;
    int buttons, warning, answer;
    char *path;
    size_t size;
} Dialog;

static void alert(void *argument)
{
    Dialog *d = argument;
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    const SDL_MessageBoxButtonData ok_cancel[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, d->buttons == HOST_MAC_RETRY_CANCEL ? "Retry" : "OK"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    SDL_MessageBoxData box = {d->warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, NULL, d->title,
                              d->text, d->buttons == HOST_MAC_OK ? 1 : 2, ok_cancel, NULL};
    int id = 0;
    d->answer = SDL_ShowMessageBox(&box, &id) && id == 1;
}

static void open_file(void *argument)
{
    Dialog *d = argument;
    SDL_InitSubSystem(SDL_INIT_VIDEO);
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.message = @(d->title);
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        UTType *type = [UTType typeWithFilenameExtension:@(d->extension)];
        if (type) panel.allowedContentTypes = @[type];
        [NSApp activateIgnoringOtherApps:YES];
        d->answer = 0;
        if ([panel runModal] == NSModalResponseOK && panel.URL.fileSystemRepresentation) {
            snprintf(d->path, d->size, "%s", panel.URL.fileSystemRepresentation);
            d->answer = d->path[0] != 0;
        }
    }
}

int host_mac_alert(const char *title, const char *text, int buttons, int warning)
{
    Dialog d = {title, text, NULL, buttons, warning, 0, NULL, 0};
    if (!sdl_host_run(alert, &d)) {  /* no SDL: the log carries it, and nothing is chosen */
        fprintf(stderr, "%s: %s\n", title, text);
        return buttons == HOST_MAC_OK;
    }
    return d.answer;
}

int host_mac_choose_file(const char *title, const char *extension, char *path, size_t size)
{
    Dialog d = {title, NULL, extension, 0, 0, 0, path, size};
    if (!size || !sdl_host_run(open_file, &d)) return 0;
    return d.answer;
}
