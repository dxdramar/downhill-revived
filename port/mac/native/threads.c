/* Threads, locks, condition variables and fibers for the module (mac_host.h, finding 075).
   Handles are indices into tables here; 0 is never one. */
#include "native.h"

#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { MAX_LOCKS = 4096, MAX_CONDS = 4096, MAX_THREADS = 256, MAX_FIBERS = 256 };

/* ---- the instance of each thread ---- */

w2c_dhd main_instance;
static _Thread_local uint32_t thread_index;
_Thread_local w2c_dhd *thread_instance;

/* ---- locks: recursive, as Windows critical sections are ---- */

static pthread_mutex_t locks[MAX_LOCKS];
static pthread_cond_t conds[MAX_CONDS];
static volatile uint32_t lock_used[MAX_LOCKS], cond_used[MAX_CONDS];

static uint32_t take_slot(volatile uint32_t *used, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++)
        if (!used[i] && !__atomic_exchange_n(&used[i], 1, __ATOMIC_ACQ_REL)) return i;
    fprintf(stderr, "dhd: out of host locks or condition variables\n");
    abort();
}

u32 w2c_mac_lock_new(struct w2c_mac *unused)
{
    (void)unused;
    uint32_t i = take_slot(lock_used, MAX_LOCKS);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&locks[i], &a);
    pthread_mutexattr_destroy(&a);
    return i;
}
void w2c_mac_lock_free(struct w2c_mac *unused, u32 i)
{
    (void)unused;
    pthread_mutex_destroy(&locks[i]);
    __atomic_store_n(&lock_used[i], 0, __ATOMIC_RELEASE);
}
/* A thread's module stack pointer must be in the module's own memory (above PS2 RAM): a check on
   every lock, which every thread takes often (DHD_CHECK_STACK). */
#include <execinfo.h>
void check_stack(const char *where)
{
    static int on = -1, reported;
    if (on < 0) on = getenv("DHD_CHECK_STACK") != NULL;
    if (!on || !thread_instance) return;
    uint32_t sp = *instance_stack_pointer(thread_instance);
    if (sp >= 0x02000000u || reported++) return;
    fprintf(stderr, "dhd: %s with module stack pointer %08X on thread %u\n", where, sp, thread_index + 1);
    void *frames[40];
    backtrace_symbols_fd(frames, backtrace(frames, 40), 2);
}

void w2c_mac_lock(struct w2c_mac *unused, u32 i) { (void)unused; check_stack("lock"); pthread_mutex_lock(&locks[i]); }
void w2c_mac_unlock(struct w2c_mac *unused, u32 i) { (void)unused; pthread_mutex_unlock(&locks[i]); }

u32 w2c_mac_cond_new(struct w2c_mac *unused)
{
    (void)unused;
    uint32_t i = take_slot(cond_used, MAX_CONDS);
    pthread_cond_init(&conds[i], NULL);
    return i;
}
void w2c_mac_cond_free(struct w2c_mac *unused, u32 i)
{
    (void)unused;
    pthread_cond_destroy(&conds[i]);
    __atomic_store_n(&cond_used[i], 0, __ATOMIC_RELEASE);
}
u32 w2c_mac_cond_wait(struct w2c_mac *unused, u32 cond, u32 lock, u32 ms)
{
    (void)unused;
    if (ms == 0xFFFFFFFFu) return pthread_cond_wait(&conds[cond], &locks[lock]) == 0;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
    return pthread_cond_timedwait(&conds[cond], &locks[lock], &t) != ETIMEDOUT;
}
void w2c_mac_cond_wake(struct w2c_mac *unused, u32 cond, u32 all)
{
    (void)unused;
    if (all) pthread_cond_broadcast(&conds[cond]);
    else pthread_cond_signal(&conds[cond]);
}

/* ---- the module stacks: whose they are (the watch build checks stores against them) ---- */

typedef struct { uint32_t low, high; pthread_t owner; } StackRange;
static StackRange stack_ranges[512];
static _Atomic uint32_t stack_range_count;

static void note_stack_range(uint32_t top, uint32_t size)
{
    uint32_t i = atomic_fetch_add(&stack_range_count, 1);
    if (i < 512) stack_ranges[i] = (StackRange){top - size, top, pthread_self()};
    if (getenv("DHD_STACK_LOG"))
        fprintf(stderr, "stack %08X-%08X for %s %u\n", top - size, top, size == 0x40000 ? "fiber of thread" : "thread",
                thread_index + 1);
}

/* The range of another host thread's module stack that [address, address + size) touches, or 0. */
const void *stack_of_another_thread(uint64_t address, uint64_t size)
{
    uint32_t n = atomic_load(&stack_range_count);
    for (uint32_t i = 0; i < n && i < 512; i++)
        if (address < stack_ranges[i].high && address + size > stack_ranges[i].low &&
            !pthread_equal(stack_ranges[i].owner, pthread_self()))
            return &stack_ranges[i];
    return NULL;
}

/* ---- threads ---- */

typedef struct {
    pthread_t thread;
    uint32_t index, entry, arg, stack_top;
    volatile int done;
    pthread_mutex_t lock;
    pthread_cond_t ended;
} Thread;
static Thread *threads[MAX_THREADS];
static volatile uint32_t thread_count = 1;

static void *thread_main(void *p)
{
    Thread *t = p;
    w2c_dhd instance = main_instance;
    *instance_stack_pointer(&instance) = t->stack_top;
    thread_instance = &instance;
    thread_index = t->index;
    note_stack_range(t->stack_top, 1 << 20);  /* port/mac/win32.c THREAD_STACK */
    fpcr_ee_default();
    fibers_thread_init();
    w2c_dhd_mac_call(&instance, t->entry, t->arg);
    pthread_mutex_lock(&t->lock);
    t->done = 1;
    pthread_cond_broadcast(&t->ended);
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

u32 w2c_mac_thread_create(struct w2c_mac *unused, u32 entry, u32 arg, u32 stack_top)
{
    (void)unused;
    uint32_t i = __atomic_fetch_add(&thread_count, 1, __ATOMIC_ACQ_REL);
    if (i >= MAX_THREADS) return 0;
    Thread *t = calloc(1, sizeof *t);
    t->index = i; t->entry = entry; t->arg = arg; t->stack_top = stack_top;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->ended, NULL);
    threads[i] = t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16 << 20);  /* wasm2c's C keeps the module's locals on it */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    /* the game's threads run frame-paced work: keep them on the performance cores */
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    int failed = pthread_create(&t->thread, &a, thread_main, t);
    pthread_attr_destroy(&a);
    return failed ? 0 : i;
}

u32 w2c_mac_thread_join(struct w2c_mac *unused, u32 i, u32 ms)
{
    (void)unused;
    Thread *t = i < MAX_THREADS ? threads[i] : NULL;
    if (!t) return 1;
    pthread_mutex_lock(&t->lock);
    if (ms == 0xFFFFFFFFu)
        while (!t->done) pthread_cond_wait(&t->ended, &t->lock);
    else if (!t->done) {
        struct timespec when;
        clock_gettime(CLOCK_REALTIME, &when);
        when.tv_sec += ms / 1000;
        when.tv_nsec += (long)(ms % 1000) * 1000000;
        if (when.tv_nsec >= 1000000000) when.tv_sec++, when.tv_nsec -= 1000000000;
        while (!t->done && pthread_cond_timedwait(&t->ended, &t->lock, &when) != ETIMEDOUT) {}
    }
    int done = t->done;
    pthread_mutex_unlock(&t->lock);
    return done;
}

u32 w2c_mac_thread_id(struct w2c_mac *unused) { (void)unused; return thread_index + 1; }

void w2c_mac_thread_name(struct w2c_mac *unused, u32 name)
{
    (void)unused;
    char n[64];
    snprintf(n, sizeof n, "dhd %s", string_at(name));
    pthread_setname_np(n);
}

/* ---- fibers ----
   Each has its own host stack (for wasm2c's C) and module stack pointer; a switch saves the
   thread's instance stack pointer with the host context (switch_arm64.S keeps FPCR too). */
void fiber_switch(void **save_sp, void *load_sp);
void fiber_start(void);

typedef struct {
    void *sp;                 /* the saved host stack pointer, while not running */
    uint32_t module_sp;       /* the saved module stack pointer, while not running */
    uint8_t *stack;           /* host stack (NULL: a thread's own) */
    size_t stack_size;
    uint32_t entry, arg;
} Fiber;
static Fiber *fibers[MAX_FIBERS];
static pthread_mutex_t fiber_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t current_fiber;
enum { FIBER_HOST_STACK = 8 << 20 };

static uint32_t new_fiber(Fiber *f)
{
    pthread_mutex_lock(&fiber_lock);
    uint32_t i = 1;
    while (i < MAX_FIBERS && fibers[i]) i++;
    if (i < MAX_FIBERS) fibers[i] = f;
    pthread_mutex_unlock(&fiber_lock);
    if (i == MAX_FIBERS) { fprintf(stderr, "dhd: out of fibers\n"); abort(); }
    return i;
}

void fibers_thread_init(void) { current_fiber = 0; }

u32 w2c_mac_fiber_main(struct w2c_mac *unused)
{
    (void)unused;
    if (!current_fiber) current_fiber = new_fiber(calloc(1, sizeof(Fiber)));
    return current_fiber;
}

u32 w2c_mac_fiber_current(struct w2c_mac *unused) { (void)unused; return current_fiber; }

static void fiber_entry(void *p)
{
    Fiber *f = p;
    w2c_dhd_mac_call_void(thread_instance, f->entry, f->arg);
    fprintf(stderr, "dhd: a fiber's function returned\n");
    abort();
}

u32 w2c_mac_fiber_create(struct w2c_mac *unused, u32 entry, u32 arg, u32 module_stack_top)
{
    (void)unused;
    Fiber *f = calloc(1, sizeof *f);
    f->stack_size = FIBER_HOST_STACK;
    f->stack = mmap(NULL, f->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->stack == MAP_FAILED) { perror("dhd: fiber stack"); abort(); }
    mprotect(f->stack, 16384, PROT_NONE);  /* a guard page at the bottom */
    f->entry = entry; f->arg = arg;
    f->module_sp = module_stack_top;
    note_stack_range(module_stack_top, 0x40000);  /* port/mac/host_mac.c FIBER_STACK */
    /* The first switch to it pops a frame from the top of its stack: x19 = fiber_entry, x20 = f,
       x30 = fiber_start, FPCR as the creator's. */
    uint64_t *frame = (uint64_t *)(f->stack + f->stack_size - 176 - 16);
    memset(frame, 0, 176);
    frame[0] = (uint64_t)(uintptr_t)fiber_entry;
    frame[1] = (uint64_t)(uintptr_t)f;
    frame[11] = (uint64_t)(uintptr_t)fiber_start;
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    frame[20] = fpcr;
    f->sp = frame;
    return new_fiber(f);
}

void w2c_mac_fiber_switch(struct w2c_mac *unused, u32 to)
{
    (void)unused;
    Fiber *from = fibers[current_fiber], *next = fibers[to];
    if (!from || !next || from == next) return;
    uint32_t *sp = instance_stack_pointer(thread_instance);
    from->module_sp = *sp;
    *sp = next->module_sp;
    current_fiber = to;
    fiber_switch(&from->sp, next->sp);
}

void w2c_mac_fiber_delete(struct w2c_mac *unused, u32 i)
{
    (void)unused;
    Fiber *f = i < MAX_FIBERS ? fibers[i] : NULL;
    if (!f || i == current_fiber) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
    pthread_mutex_lock(&fiber_lock);
    fibers[i] = NULL;
    pthread_mutex_unlock(&fiber_lock);
}
