/* The SDL thread (W11): the process's one SDL thread. It owns the video subsystem and the window
   (SDL's window messages go to the thread that created the window, and SDL's documentation puts
   the video calls and the event queue on one thread), it pumps the events and sends them where
   they belong (the window, the keyboard, the text field, the controllers), and it runs the
   controllers' pass (gamepad.c) every 4 ms, as the controllers' own thread did before.

   The game thread never waits on SDL's queue. It reads copies: the keyboard's keys
   (keyboard.c), the controllers' states (gamepad.c), and the window's size and close (present.c).
   A modal drag or resize of the window stalls this thread only: the game keeps running.

   Keys: SDL's key events by place (scan code, for letters: the US place) and by the character the
   key types (keycode). Control characters that Windows sent as WM_CHAR (Backspace, Tab, Enter,
   Escape, Ctrl+letter) are sent from the key events here, repeats included, so the text field
   behaves as before. Text: SDL's UTF-8 text input, decoded to code points.

   Used through hle.h: sdl_host_start (the thread, once), sdl_host_run (a function run on the
   thread, waiting for it: the window's creation). On macOS the thread is the main thread
   (sdl_host_main, from launcher.c). */
#include "host_api.h"
#include "../hle/hle.h"
#include "../hle/text_input.h"
#include "../gs/present.h"

#include <SDL3/SDL.h>
#include <stdio.h>

static SDL_AtomicInt started;      /* 1 once the thread was started */
static SDL_AtomicInt up_ok;        /* 1 once SDL's events are up on the thread */
static SDL_Semaphore *up;          /* signalled once the thread's start is done */
static SDL_Mutex *slot_lock;       /* one request at a time */
static void (*request_fn)(void *); /* the pending request (under slot_lock) */
static void *request_arg;
static SDL_Semaphore *request_done;

/* Keys that are not letters, by the Windows virtual-key code the game's tables use (keyboard.c). */
static unsigned special_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    default: return 0;
    }
}

/* The control character Windows sent as WM_CHAR for the key (0: none). */
static unsigned control_char(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 13;
    case SDL_SCANCODE_ESCAPE: return 27;
    default: return 0;
    }
}

static void key_event(const SDL_KeyboardEvent *k)
{
    SDL_Scancode sc = k->scancode;
    SDL_Keycode key = k->key;
    int down = k->down;
    unsigned mods = k->mod;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        /* a letter: its place on a US keyboard, and the letter it types on this layout */
        unsigned place = 'A' + (sc - SDL_SCANCODE_A), typed = place;
        int letter = key >= 'a' && key <= 'z';
        if (letter) typed = (unsigned)(key - 'a' + 'A');
        keyboard_key(place, typed, down);
        /* Ctrl+letter is a control character to the field (Ctrl+V = 22); AltGr (Ctrl+Alt) types text */
        if (down && letter && (mods & SDL_KMOD_CTRL) && !(mods & SDL_KMOD_ALT))
            text_input_character((unsigned)(key - 'a' + 1));
        return;
    }
    unsigned vk = special_key(sc);
    if (vk) keyboard_key(vk, vk, down);
    if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL)
        keyboard_key(VKEY_CONTROL, VKEY_CONTROL, (mods & SDL_KMOD_CTRL) != 0);
    if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)
        keyboard_key(VKEY_MENU, VKEY_MENU, (mods & SDL_KMOD_ALT) != 0);
    if (sc == SDL_SCANCODE_LGUI) keyboard_key(VKEY_LWIN, VKEY_LWIN, down);
    if (sc == SDL_SCANCODE_RGUI) keyboard_key(VKEY_RWIN, VKEY_RWIN, down);
    if (down && control_char(sc)) text_input_character(control_char(sc));
}

/* One code point of UTF-8 at *s; advances *s. */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3F >> more;
    for (int i = 0; i < more && (*p & 0xC0) == 0x80; i++) c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static void text_event(const SDL_TextInputEvent *t)
{
    const char *s = t->text;
    while (s && *s) {
        unsigned c = utf8_next(&s);
        if (c) text_input_character(c);
    }
}

static void dispatch(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        key_event(&e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        text_event(&e->text);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        keyboard_focus(1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:  /* WM_KILLFOCUS: the field's text is cleared, the keys let go */
        keyboard_focus(0);
        text_input_clear();
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        gs_window_event(e->type);
        break;
    case SDL_EVENT_JOYSTICK_ADDED:
        gamepad_device(PADDEV_ADDED, (unsigned)e->jdevice.which);
        break;
    case SDL_EVENT_JOYSTICK_REMOVED:
        gamepad_device(PADDEV_REMOVED, (unsigned)e->jdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        gamepad_device(PADDEV_REMAPPED, (unsigned)e->gdevice.which);
        break;
    default:
        break;
    }
}

/* A request from sdl_host_run, if one is waiting: run it, then let its caller go. */
static void run_request(void)
{
    SDL_LockMutex(slot_lock);
    void (*fn)(void *) = request_fn;
    void *arg = request_arg;
    SDL_Semaphore *done = request_done;
    request_fn = NULL;
    SDL_UnlockMutex(slot_lock);
    if (fn) {
        fn(arg);
        SDL_SignalSemaphore(done);
    }
}

static int sdl_thread(void *unused)
{
    (void)unused;
    host_thread_name("sdl");
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        fprintf(stderr, "sdl: no event queue (%s)\n", SDL_GetError());
        SDL_SignalSemaphore(up);
        return 0;
    }
    SDL_SetAtomicInt(&up_ok, 1);
    gamepad_thread_start();  /* the controllers: their own messages, logs and failures */
    SDL_SignalSemaphore(up);
    for (;;) {
        run_request();
        SDL_Event e;
        /* wakes on any event; 4 ms at most, so the controllers' states and vibration are never late */
        for (int got = SDL_WaitEventTimeout(&e, 4); got; got = SDL_PollEvent(&e)) dispatch(&e);
        gamepad_thread_update();
    }
    return 0;
}

#if defined(__APPLE__)
/* macOS: Cocoa's windows and their events belong to the main thread, so the main thread is the
   SDL thread (launcher.c starts the game on another and calls this). It starts at once; the game
   thread's sdl_host_start only waits for it. */
void sdl_host_main(void)
{
    up = SDL_CreateSemaphore(0);
    slot_lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&started, 1);
    sdl_thread(NULL);
}
#endif

/* Starts the thread once; returns 1 when SDL's events are up there (waits up to 3 s the first time). */
int sdl_host_start(void)
{
    static int waited;
#if defined(__APPLE__)
    while (!SDL_GetAtomicInt(&started)) SDL_Delay(1);  /* sdl_host_main runs from the start */
#endif
    if (SDL_CompareAndSwapAtomicInt(&started, 0, 1)) {
        up = SDL_CreateSemaphore(0);
        slot_lock = SDL_CreateMutex();
        SDL_Thread *t = SDL_CreateThread(sdl_thread, "sdl", NULL);
        if (t) SDL_DetachThread(t);
        else {
            fprintf(stderr, "sdl: no thread (%s)\n", SDL_GetError());
            SDL_SignalSemaphore(up);
        }
    }
    if (!waited) {  /* the game thread makes these calls; only the first one waits */
        waited = 1;
        SDL_WaitSemaphoreTimeout(up, 3000);
    }
    return SDL_GetAtomicInt(&up_ok);
}

/* Runs fn(arg) on the SDL thread and returns when it has. 0 if the thread isn't up. */
int sdl_host_run(void (*fn)(void *), void *arg)
{
    if (!sdl_host_start()) return 0;
    SDL_Semaphore *done = SDL_CreateSemaphore(0);
    for (;;) {  /* one request at a time */
        SDL_LockMutex(slot_lock);
        if (!request_fn) break;
        SDL_UnlockMutex(slot_lock);
        SDL_Delay(1);
    }
    request_fn = fn;
    request_arg = arg;
    request_done = done;
    SDL_UnlockMutex(slot_lock);
    SDL_WaitSemaphore(done);
    SDL_DestroySemaphore(done);
    return 1;
}
