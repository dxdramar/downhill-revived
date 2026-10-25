/* Reproducible input without window focus: start:duration:button[+button],...
   Times are host seconds from boot; intervals include start, exclude end. */
#include "hle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

void gs_capture_request(const char *name);  /* port/gs/capture.c */

int autopad_read(const char *script, double seconds, uint16_t *buttons)
{
    static const struct { const char *name; uint16_t mask; } names[] = {
        {"cross",PAD_CROSS}, {"circle",PAD_CIRCLE}, {"square",PAD_SQUARE}, {"triangle",PAD_TRIANGLE},
        {"start",PAD_START}, {"select",PAD_SELECT}, {"up",PAD_UP}, {"down",PAD_DOWN},
        {"left",PAD_LEFT}, {"right",PAD_RIGHT}, {"l1",PAD_L1}, {"r1",PAD_R1}, {"l2",PAD_L2}, {"r2",PAD_R2}
    };
    *buttons = 0;
    if (!script) return 0;
    /* An opt-in file lets proof drivers advance only after the expected menu
       state, without sending later START presses into a running race. */
    char file_script[4096];
    if (*script == '@') {
#ifdef _WIN32
        /* Keep readers valid across the proof driver's atomic replacement. */
        HANDLE file = CreateFileA(script + 1, GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return 0;
        DWORD size;
        int failed = !ReadFile(file, file_script, sizeof file_script, &size, NULL);
        CloseHandle(file);
        if (failed || size == sizeof file_script || memchr(file_script, 0, size)) return 0;
#else
        FILE *file = fopen(script + 1, "rb");
        if (!file) return 0;
        size_t size = fread(file_script, 1, sizeof file_script - 1, file);
        int extra = fgetc(file), failed = ferror(file);
        fclose(file);
        if (failed || extra != EOF || memchr(file_script, 0, size)) return 0;
#endif
        while (size && (file_script[size-1] == '\r' || file_script[size-1] == '\n' ||
                        file_script[size-1] == ' ' || file_script[size-1] == '\t')) size--;
        file_script[size] = 0;
        script = file_script;
    }
    while (*script) {
        double start, duration;
        char chord[128];
        int consumed = 0;
        if (sscanf(script, "%lf:%lf:%127[a-z0-9+]%n", &start, &duration, chord, &consumed) != 3 ||
            !isfinite(start) || !isfinite(duration) || start < 0 || duration <= 0)
            return 0;
        uint16_t mask = 0;
        char *name = chord;
        for (;;) {
            char *end = strchr(name, '+');
            if (end) *end = 0;
            unsigned i;
            for (i = 0; i < sizeof names / sizeof *names && strcmp(name, names[i].name); i++) {}
            if (i == sizeof names / sizeof *names) return 0;
            mask |= names[i].mask;
            if (!end) break;
            name = end + 1;
        }
        if (seconds >= start && seconds < start + duration) *buttons |= mask;
        script += consumed;
        if (!*script) break;
        if (*script++ != ',' || !*script) return 0;
    }
    return 1;
}

/* Race-relative input and capture for PCSX2 comparisons (DHD_RACE_SYNC). The patched PCSX2
   (port/tools/prepare_pcsx2_compare.py) applies the same rule, so both runs meet at the same
   game moments whatever their boot, loading and frame times. Moments are keyed by the game's
   race clock (60 per second, one per PS2 vsync), which restarts at the countdown (menu 9, race
   phase 1) and at GO (phase 2, still running in phase 3). The port's own vsyncs don't serve:
   under its virtual clock it spends more of them per game frame than the PS2. */
#define RACE_MENU (*(volatile int32_t *)PS2_HOST(0x29DED8))
#define RACE_PHASE (*(volatile int32_t *)PS2_HOST(0x43F348))
#define RACE_CLOCK (*(volatile int32_t *)PS2_HOST(0x43EEC8))
static int race_started[2], race_clock;
static unsigned long long race_field = ~0ull;
static volatile unsigned long long race_pad_field;

/* Opt-in visual survey: NEW LEVEL retains shell menu 2. Arm the existing
   race-clock input/capture only after the built-in unlock bootstrap has
   loaded a fresh rider with its ordinary cheat flags cleared. Default
   comparisons still arm on their first menu-9 countdown, unchanged. */
static int race_sync_menu(void)
{
    return RACE_MENU == 9 || (getenv("DHD_RACE_SYNC_UNLOCKED") && RACE_MENU == 2);
}

static int race_sync_ready(void)
{
    if (!getenv("DHD_RACE_SYNC_UNLOCKED")) return 1;
    return *(volatile uint32_t *)PS2_HOST(0x4346B4) == 0x035FFFFFu &&
           *(volatile uint32_t *)PS2_HOST(0x2B6E68) == 0 &&
           *(volatile uint32_t *)PS2_HOST(0x2B6E6C) == 0 &&
           *(volatile uint32_t *)PS2_HOST(0x2B6E70) == 0;
}

#ifdef _WIN32
/* Every 30 s of wall time, where the run is: the virtual field, the game's menu, race phase
   and clock, and the field of the last pad read. The comparison drivers stop on a stalled
   heartbeat and report the screen, instead of waiting forever; it runs on its own thread so
   it reports even when the EE no longer reaches a vsync. */
static DWORD WINAPI race_heartbeat(void *thread)
{
    extern uint64_t kernel_vsync_count;
    unsigned long long last = ~0ull;
    for (;;) {
        Sleep(30000);
        unsigned long long field = kernel_vsync_count;
        unsigned long eip = 0;
        if (field == last) {  /* stalled: where the thread that reads the pad is spinning */
            CONTEXT c;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(thread) != (DWORD)-1) {
                if (GetThreadContext(thread, &c)) eip = c.Eip;
                ResumeThread(thread);
            }
        }
        last = field;
        fprintf(stderr, "compare: heartbeat field=%llu menu=%d phase=%d clock=%d pad=%llu eip=%08lX\n",
                field, RACE_MENU, RACE_PHASE, RACE_CLOCK, race_pad_field, eip);
    }
}
#endif

/* DHD_NAV: menu navigation by the screen shown instead of by time (menu_nav.h), the same rule as
   the PCSX2 adapter's DHD_COMPARE_NAV. Holds this field's buttons; 0 for an invalid script. */
#include "menu_nav.h"
static void nav_dump(const char *name, const char *when)
{
    const char *dir = getenv("DHD_NAV_DUMP");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/%s-%s.bin", dir, name, when);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    graphics_settle();
    fwrite((const void *)(uintptr_t)PS2_HOST(0), 32u << 20, 1, f);
    fclose(f);
    fprintf(stderr, "compare: nav dump %s\n", path);
}

static unsigned nav_read(unsigned address) { return *(volatile uint32_t *)PS2_HOST(address & 0x1FFFFFCu); }

int autopad_nav(const char *script, unsigned long long field, uint16_t *buttons)
{
    static MenuNav nav;
    static unsigned long long last = ~0ull;
    static unsigned held;
    static char *current;
    if (!current || strcmp(current, script)) {  /* a new script (online.c sets one at the race): from its first step */
        free(current);
        current = _strdup(script);
        memset(&nav, 0, sizeof nav);
        held = 0;
    }
    if (field != last) {
        int step = nav.step;
        unsigned long long pressed = nav.pressed;
        static unsigned long long post;
        static char name[64];
        last = field;
        if (!menu_nav(&nav, script, RACE_MENU, RACE_PHASE, nav_read, field, &held)) return 0;
        if (nav.step != step)
            fprintf(stderr, "compare: nav step %d field=%llu menu=%d phase=%d\n", nav.step, field, RACE_MENU, RACE_PHASE);
        /* DHD_NAV_DUMP=dir: EE RAM as each press starts and MENU_NAV_RETRY - 1 fields later, for
           finding what a screen's press changed. Game data: keep the directory under build/. */
        if (nav.pressed && nav.pressed != pressed) {
            snprintf(name, sizeof name, "nav-s%d-t%d-f%llu", nav.step, nav.tries, field);
            nav_dump(name, "pre");
            post = field + MENU_NAV_RETRY - 1;
        } else if (field == post) {
            nav_dump(name, "post");
        }
    }
    *buttons = (uint16_t)held;
    return 1;
}

static int race_in(int which)
{
    int phase = RACE_PHASE;
    return race_started[which] && race_sync_menu() && (which ? phase == 2 || phase == 3 : phase == 1);
}

void race_sync_vsync(unsigned long long field)
{
    if (!getenv("DHD_RACE_SYNC") || field == race_field) return;
    if (!race_started[0] && !race_sync_ready()) return;
#ifdef _WIN32
    static int heartbeat;
    if (!heartbeat++) {
        HANDLE self;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CloseHandle(CreateThread(NULL, 0, race_heartbeat, self, 0, NULL));
    }
#endif
    race_field = field;
    race_clock = RACE_CLOCK;
    /* DHD_RACE_FRAMELOG: the game's frame counters every field, as the PCSX2 adapter's
       DHD_COMPARE_FRAMELOG: main-loop frames (D_0029E10C) and load-bar frames (D_00663108). */
    static int frame_log = -1;
    if (frame_log < 0) frame_log = getenv("DHD_RACE_FRAMELOG") != NULL;
    if (frame_log)
        fprintf(stderr, "compare: frame field=%llu frame=%d loadbar=%d menu=%d phase=%d clock=%d\n", field,
                *(volatile int32_t *)PS2_HOST(0x29E10C), *(volatile int32_t *)PS2_HOST(0x663108), RACE_MENU, RACE_PHASE, race_clock);
    for (int i = 0; i < 2; i++)
        if (!race_started[i] && race_sync_menu() && RACE_PHASE == i + 1) {
            race_started[i] = 1;
            fprintf(stderr, "compare: race anchor=%c field=%llu clock=%d\n", "cg"[i], field, race_clock);
        }
}

/* From the countdown the pad is neutral, then DHD_RACE_PAD (player 0; DHD_RACE_PAD<n> for
   player n, else DHD_RACE_PAD) keyed by race-clock seconds after GO. Returns 0 while the boot
   input still applies. */
int race_sync_pad(int player, uint16_t *buttons)
{
    char own[32];
    snprintf(own, sizeof own, "DHD_RACE_PAD%d", player);
    const char *script = player && getenv(own) ? getenv(own) : getenv("DHD_RACE_PAD");
    race_pad_field = race_field;
    if (!race_started[0]) return 0;
    *buttons = 0;
    if (race_in(1) && script && *script && !autopad_read(script, race_clock / 60.0, buttons)) {
        fprintf(stderr, "Invalid DHD_RACE_PAD\n");
        exit(1);
    }
    return 1;
}

double race_sync_seconds(void) { return race_in(1) ? race_clock / 60.0 : -1; }

/* A race seeds its RNGs (start grid, AI) from the EE time since a timer reset (0016B8C8:
   func_00238F10), which follows load timing. DHD_RACE_SEED pins it, so the port starts the race
   PCSX2 compared against (the seed is stored at 0x0043FA10). */
int hle_race_seed(int seed)
{
    const char *pinned = getenv("DHD_RACE_SEED");
    int value = pinned && *pinned ? (int)strtol(pinned, NULL, 0) : seed;
    if (getenv("DHD_RACE_SYNC"))
        fprintf(stderr, "compare: race seed %d (timer %d)\n", value, seed);
    return value;
}

/* DHD_RACE_DUMP=dir: at each moment, EE RAM (32 MB) then the scratchpad (16 KB) to
   dir/ee-c+N.bin, as the patched PCSX2 does with DHD_COMPARE_RACE_DUMP, for comparing game
   state. Game data: keep the directory under build/. */
static void race_sync_dump(char anchor, long value)
{
    const char *dir = getenv("DHD_RACE_DUMP");
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/ee-%c%+ld.bin", dir, anchor, value);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    graphics_settle();
    fwrite((const void *)(uintptr_t)PS2_HOST(0), 32u << 20, 1, f);
    fwrite((const void *)0x70000000, 16u << 10, 1, f);
    fclose(f);
    fprintf(stderr, "compare: race dump %s clock=%d\n", path, race_clock);
}

/* Online tests: h+N moments count from the first hit this PC dealt or took (puppet.c). */
static int hit_clock = -1;
void race_sync_hit(void)
{
    if (hit_clock < 0) hit_clock = RACE_CLOCK;
}

/* DHD_RACE_CAPTURE lists c+N / g+N / h+N race-clock values. Each moment is captured on the vsync
   after its phase shows a clock of at least N (h: N after the first hit), and on the
   2 * DHD_RACE_CAPTURE_WINDOW after it. Names this field's capture, or returns 0. */
int race_sync_capture(const char *prefix, char *name, size_t size)
{
    static unsigned long long crossed[16];
    const char *spec = getenv("DHD_RACE_CAPTURE"), *window = getenv("DHD_RACE_CAPTURE_WINDOW");
    long w = window ? atol(window) : 0;
    for (int i = 0; spec && *spec && i < 16; i++) {
        char *end;
        int which = *spec == 'g' || *spec == 'h';
        long value = strtol(spec + 1, &end, 10);
        if (!crossed[i] && race_in(which) && race_clock >= value + (*spec == 'h' ? hit_clock : 0) &&
            (*spec != 'h' || hit_clock >= 0)) {
            char name[32];
            crossed[i] = race_field + 2;  /* from the next vsync, like PCSX2 */
            race_sync_dump(*spec, value);
            snprintf(name, sizeof name, "%c%+ld", *spec, value);
            gs_capture_request(name);  /* DHD_RACE_GSDUMP */
        }
        if (crossed[i] && race_field + 1 - crossed[i] <= (unsigned long long)(2 * w)) {
            snprintf(name, size, "%s-%c%+ld+%llu.bmp", prefix, *spec, value, race_field + 1 - crossed[i]);
            fprintf(stderr, "compare: race moment=%c%+ld delta=%llu field=%llu clock=%d phase=%d\n", *spec, value,
                    race_field + 1 - crossed[i], race_field, race_clock, RACE_PHASE);
            return 1;
        }
        spec = *end ? end + 1 : end;
    }
    return 0;
}
