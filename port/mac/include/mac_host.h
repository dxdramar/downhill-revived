/* Mac build (finding 075): what the wasm module asks of the native Mac host (port/mac/native).
   Plain integers and module addresses only: the host reads and writes the module's memory, which
   is the PS2 address space, at those addresses. Handles are host numbers, never pointers. */
#ifndef MAC_HOST_H
#define MAC_HOST_H

#include <stddef.h>
#include <stdint.h>

#define MAC_IMPORT(name) __attribute__((import_module("mac"), import_name(#name)))

/* Threads: entry(arg) runs on a new host thread, on the module stack whose top is stack_top. */
MAC_IMPORT(thread_create) uint32_t mac_thread_create(uint32_t (*entry)(void *), void *arg, void *stack_top);
MAC_IMPORT(thread_join) int mac_thread_join(uint32_t thread, uint32_t ms);  /* nonzero: it ended */
MAC_IMPORT(thread_id) uint32_t mac_thread_id(void);
MAC_IMPORT(thread_name) void mac_thread_name(const char *name);
/* The calling thread's native backtrace to stderr, for debugging (wasm2c names the module's functions). */
MAC_IMPORT(backtrace) void mac_backtrace(void);

/* Locks and condition variables (pthread). Recursive locks, as Windows critical sections are. */
MAC_IMPORT(lock_new) uint32_t mac_lock_new(void);
MAC_IMPORT(lock_free) void mac_lock_free(uint32_t lock);
MAC_IMPORT(lock) void mac_lock(uint32_t lock);
MAC_IMPORT(unlock) void mac_unlock(uint32_t lock);
MAC_IMPORT(cond_new) uint32_t mac_cond_new(void);
MAC_IMPORT(cond_free) void mac_cond_free(uint32_t cond);
MAC_IMPORT(cond_wait) int mac_cond_wait(uint32_t cond, uint32_t lock, uint32_t ms); /* 0: timed out */
MAC_IMPORT(cond_wake) void mac_cond_wake(uint32_t cond, int all);

/* Fibers: each has its own host stack and module stack (port/hle/host.c's EE threads). */
MAC_IMPORT(fiber_main) uint32_t mac_fiber_main(void);
MAC_IMPORT(fiber_create) uint32_t mac_fiber_create(void (*entry)(void *), void *arg, void *stack_top);
MAC_IMPORT(fiber_switch) void mac_fiber_switch(uint32_t fiber);
MAC_IMPORT(fiber_delete) void mac_fiber_delete(uint32_t fiber);
MAC_IMPORT(fiber_current) uint32_t mac_fiber_current(void);

/* Time: a monotonic clock in nanoseconds, and sleeping on it. */
MAC_IMPORT(time_ns) uint64_t mac_time_ns(void);
MAC_IMPORT(sleep_until_ns) void mac_sleep_until_ns(uint64_t when);
MAC_IMPORT(wall_time_ns) uint64_t mac_wall_time_ns(void); /* since 1970, for file times and logs */

/* The float control register in SSE's MXCSR encoding (round mode, flush to zero, the sticky
   flags), which the port's code was written for: the host keeps arm64's FPCR/FPSR in step. */
MAC_IMPORT(getcsr) uint32_t mac_getcsr(void);
MAC_IMPORT(setcsr) void mac_setcsr(uint32_t mxcsr);

/* The program's folder (where dhd.ini and saves are), with a trailing '/'; its length. */
MAC_IMPORT(app_folder) uint32_t mac_app_folder(char *out, uint32_t size);
/* The program's own file (the online lobby's build identity hashes it); its length. */
MAC_IMPORT(exe_path) uint32_t mac_exe_path(char *out, uint32_t size);
/* A modal message: flags as MessageBoxA's (MB_OK, MB_OKCANCEL ...); returns IDOK etc. */
MAC_IMPORT(message_box) int mac_message_box(const char *text, const char *caption, uint32_t flags);
/* The open-file panel; nonzero if a file was chosen (its path in out). */
MAC_IMPORT(choose_file) int mac_choose_file(const char *title, const char *extension, char *out, uint32_t size);
MAC_IMPORT(exit) void mac_exit(int code) __attribute__((noreturn));
MAC_IMPORT(utc_offset_seconds) int32_t mac_utc_offset_seconds(void);
/* The pasteboard's text as UTF-16 (at most size units); how many units. */
MAC_IMPORT(clipboard_utf16) uint32_t mac_clipboard_utf16(uint16_t *out, uint32_t size);
/* Whether a key is down while the game's window has the focus, by Windows virtual-key code
   (letters by their place on a US keyboard). */
MAC_IMPORT(key_down) int mac_key_down(int vk);

/* What the module exports for the host to call: a module function pointer (a table index),
   with one argument. Threads start in mac_call, fibers in mac_call_void (win32.c). */
uint32_t mac_call(uint32_t (*fn)(void *), void *arg);
void mac_call_void(void (*fn)(void *), void *arg);
MAC_IMPORT(log) void mac_log(const char *text, uint32_t length);

#endif
