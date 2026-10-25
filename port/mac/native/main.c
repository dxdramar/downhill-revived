/* The Mac program (finding 075): sets up the module's memory, instantiates the module that
   wasm2c made of the port and runs it (its _start runs port/hle/boot.c's main). */
#include "native.h"

#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int main_argc;
extern char **main_argv;

/* The import namespaces: the module's imports get these, and use none of them (state is here). */
struct w2c_env { int unused; };
struct w2c_mac { int unused; };
struct w2c_wasi__snapshot__preview1 { int unused; };
struct w2c_gpu { int unused; };
struct w2c_audio { int unused; };
struct w2c_pad { int unused; };
struct w2c_net { int unused; };
static struct w2c_env env;
static struct w2c_mac mac;
static struct w2c_wasi__snapshot__preview1 wasi;
static struct w2c_gpu gpu;
static struct w2c_audio audio;
static struct w2c_pad pad;
static struct w2c_net net;

static wasm_rt_memory_t memory;
wasm_rt_memory_t *w2c_env_memory(struct w2c_env *unused) { (void)unused; return &memory; }

uint32_t *instance_stack_pointer(w2c_dhd *instance) { return w2c_dhd_0x5F_stack_pointer(instance); }

/* dhd.log in the app's folder when not started from a terminal, as on Windows (host.c). */
void log_init(void)
{
    if (isatty(2) || getenv("DHD_LOG_STDERR")) return;
    char path[1200];
    snprintf(path, sizeof path, "%sdhd.log", app_folder());
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
}

/* The native call stack: wasm2c keeps the module's function names (w2c_dhd_func_...). */
static void backtrace_now(void)
{
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
}

void w2c_mac_backtrace(struct w2c_mac *unused) { (void)unused; backtrace_now(); }

static void crashed(int sig)
{
    fprintf(stderr, "dhd: %s\n", strsignal(sig));
    backtrace_now();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* A call through a function pointer that is no function: the value, for the log. */
void dhd_bad_call(uint32_t index, uint32_t size)
{
    fprintf(stderr, "dhd: call through 0x%08X, which is no function (the table has %u)\n", index, size);
}

/* A wasm trap (unreachable code, a call through a table entry of another type, an integer
   division by zero): where, then the end (build_mac.py sets WASM_RT_TRAP_HANDLER). */
void dhd_trap(wasm_rt_trap_t code)
{
    fprintf(stderr, "dhd: wasm trap: %s\n", wasm_rt_strerror(code));
    backtrace_now();
    fflush(NULL);
    _exit(70);
}

static void run_game(void)
{
    wasm2c_dhd_instantiate(&main_instance, &audio, &env, &gpu, &mac, &net, &pad, &wasi);
    thread_instance = &main_instance;
    fpcr_ee_default();
    fibers_thread_init();
    w2c_dhd_0x5Fstart(&main_instance);
    pads_stop();
    fflush(NULL);
    _exit(0);
}

static void *game_thread(void *unused)
{
    (void)unused;
    pthread_setname_np("dhd game");
    run_game();
    return NULL;
}

/* The game on a thread of its own, with a large stack (wasm2c's C keeps the module's locals on
   it), while AppKit has the main thread. */
static void start_game(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    /* the game's threads run frame-paced work: keep them on the performance cores */
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    if (pthread_create(&t, &a, game_thread, NULL)) {
        perror("dhd: game thread");
        exit(1);
    }
    pthread_attr_destroy(&a);
}

void window_run_application(void (*start)(void));

int main(int argc, char **argv)
{
    main_argc = argc;
    main_argv = argv;
    app_folder_init();
    log_init();
    signal(SIGSEGV, crashed);
    signal(SIGBUS, crashed);
    signal(SIGILL, crashed);
    signal(SIGABRT, crashed);
    signal(SIGTRAP, crashed);
    wasm_rt_init();
    space_init();
    memory.data = space;
    /* pages: the size the module was linked with (link_mac.py MODULE_END), which memory.size
       reports, so the C library's heap ends there. size: what wasm2c checks bulk copies and
       fills against, all 4 GB, as the PS2's hardware and RAM mirrors above the heap are mapped
       too (space.c) and the game copies to and from them. */
    memory.size = 0x100000000ull;
    memory.pages = memory.max_pages = 0x10000000ull / 65536;
    memory.page_size = 65536;
    memory.is64 = false;
    if (getenv("DHD_HEADLESS")) run_game();  /* no window: the tools' runs */
    window_run_application(start_game);
    return 0;
}
