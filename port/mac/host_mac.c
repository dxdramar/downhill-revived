/* The Mac's side of port/hle/host.c (finding 075): what is Windows-only there. Runs in the wasm
   module; threads, fibers and time come from the native host (mac_host.h). */
#include "hle.h"
#include "timer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* No crash report of its own: the native host prints the signal and the module's call stack. */
void host_init(void) { host_thread_name("game"); }
void host_backtrace(void) {}
void host_poll_dump(void) {}

/* Named threads, for the CPU time reports of the bench tools: not measured on the Mac. */
void host_thread_name(const char *name) { mac_thread_name(name); }
unsigned host_thread_cpu(const char **names, double *seconds, unsigned max)
{
    (void)names; (void)seconds; (void)max;
    return 0;
}

/* The log: stderr as the native host set it up (the terminal, or dhd.log in the app's folder). */
void host_console_init(void) { setvbuf(stderr, NULL, _IONBF, 0); }

/* Windows needs a second process to keep the PS2's addresses free; the Mac's module memory is
   laid out by the host before anything runs (memory_mac.c), so there is nothing to relaunch. */
int host_relaunch_reserving(uintptr_t at, size_t size)
{
    (void)at; (void)size;
    return -1;
}

/* ---- fibers: the EE threads (kernel.c) ----
   Each has a host stack (for the C that wasm2c made of the module) and a module stack, which
   holds the locals the game takes the address of: those are what the game keeps pointers to
   (finding 041), so host_stack_ranges reports the module stacks. */
enum { FIBER_STACK = 0x40000 };  /* as Windows' fibers */
static uintptr_t stack_range[64][2];
static int stack_ranges;

static void note_stack(uintptr_t low, uintptr_t high)
{
    if (stack_ranges < 64) stack_range[stack_ranges][0] = low, stack_range[stack_ranges++][1] = high;
}

void *host_fiber_main(void) { return (void *)(uintptr_t)mac_fiber_main(); }

void *host_fiber_create(void (*fn)(void *), void *arg)
{
    uint8_t *stack = malloc(FIBER_STACK);
    if (!stack) return NULL;
    note_stack((uintptr_t)stack, (uintptr_t)stack + FIBER_STACK);
    return (void *)(uintptr_t)mac_fiber_create(fn, arg, stack + FIBER_STACK);
}

int host_stack_ranges(uintptr_t (**ranges)[2])
{
    *ranges = stack_range;
    return stack_ranges;
}

/* The host keeps each fiber's float control (FPCR) across the switch, as FIBER_FLAG_FLOAT_SWITCH
   and the fxsave in host.c do. */
void host_fiber_switch(void *fiber) { mac_fiber_switch((uint32_t)(uintptr_t)fiber); }
void host_fiber_delete(void *fiber) { mac_fiber_delete((uint32_t)(uintptr_t)fiber); }
void *host_fiber_current(void) { return (void *)(uintptr_t)mac_fiber_current(); }

/* ---- time ---- */

double host_wall_seconds(void)
{
    static uint64_t start;
    if (!start) start = mac_time_ns();
    return (double)(mac_time_ns() - start) * 1e-9;
}

double host_seconds(void)
{
    return ee_frame_clock_enabled() ? (double)ee_frame_clock_cycles() / 147456000.0 : host_wall_seconds();
}

void host_sleep_ms(unsigned ms) { Sleep(ms); }

/* Until a host_wall_seconds() time: as on Windows, the host sleeps to a millisecond before it
   (mach_wait_until can wake a few milliseconds late, more when the system coalesces timers),
   then spins. */
void host_wait_wall(double seconds)
{
    double left = seconds - host_wall_seconds();
    if (left > 0.001) mac_sleep_until_ns(mac_time_ns() + (uint64_t)((left - 0.001) * 1e9));
    while (host_wall_seconds() < seconds)
        YieldProcessor();
}
