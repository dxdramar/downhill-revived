/* A small Win32 companion for POSIX builds (Linux): the exact signatures
   the ported host files already use (events, joinable threads, critical
   sections, condition variables, slim locks, interlocked integers), so their
   Windows branches stay untouched and the POSIX branches only swap the
   include. Only the synchronization and thread-lifetime surface is here;
   windows, devices, COM, WinHTTP, GDI and process creation stay in each
   file's own POSIX branch. Included only when _WIN32 is not defined. */
#ifndef DHD_WIN32_COMPAT_H
#define DHD_WIN32_COMPAT_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

typedef int32_t LONG;
typedef uint32_t DWORD;
typedef int64_t LONG64;
typedef uint16_t WORD;
typedef int16_t SHORT;
typedef uint8_t BYTE;
typedef unsigned int UINT;
typedef int BOOL;
#ifndef TRUE
enum { FALSE = 0, TRUE = 1 };
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef INFINITE
enum { INFINITE = 0xFFFFFFFFu };
#endif
#ifndef WAIT_OBJECT_0
enum { WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258 };
#endif
#ifndef MAX_PATH
enum { MAX_PATH = 260 };
#endif

static inline LONG InterlockedIncrement(volatile LONG *a) { return __sync_add_and_fetch(a, 1); }
static inline LONG InterlockedDecrement(volatile LONG *a) { return __sync_add_and_fetch(a, -1); }
static inline LONG InterlockedExchange(volatile LONG *a, LONG v) { return __sync_lock_test_and_set(a, v); }
static inline LONG InterlockedExchangeAdd(volatile LONG *a, LONG v) { return __sync_fetch_and_add(a, v); }
static inline LONG InterlockedCompareExchange(volatile LONG *a, LONG e, LONG c)
{
    return __sync_val_compare_and_swap(a, e, c);
}

/* Opaque waitable: an auto-reset event, a manual-reset event or a thread. */
typedef struct WinHandle WinHandle;
typedef WinHandle *HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)0)
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER

/* The current thread's handle for GetThreadTimes (set by the trampoline). */
static _Thread_local WinHandle *win_thread_self;

struct WinHandle {
    int kind; /* 1: event, 2: thread */
    /* event */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int manual, signaled;
    /* thread */
    pthread_t thread;
    DWORD code;
    int joined;
};

static inline HANDLE WinEventCreate(int manual, int initial)
{
    WinHandle *h = calloc(1, sizeof *h);
    if (!h) return 0;
    h->kind = 1;
    h->manual = manual;
    h->signaled = initial;
    pthread_mutex_init(&h->mutex, NULL);
    pthread_cond_init(&h->cond, NULL);
    return h;
}

static inline HANDLE CreateEventA(void *attrs, BOOL manual, BOOL initial, const char *name)
{
    (void)attrs;
    (void)name;
    return WinEventCreate(manual, initial);
}

static inline BOOL SetEvent(HANDLE h)
{
    if (!h || h->kind != 1) return FALSE;
    pthread_mutex_lock(&h->mutex);
    h->signaled = 1;
    if (h->manual) pthread_cond_broadcast(&h->cond);
    else pthread_cond_signal(&h->cond);
    pthread_mutex_unlock(&h->mutex);
    return TRUE;
}

static inline BOOL ResetEvent(HANDLE h)
{
    if (!h || h->kind != 1) return FALSE;
    pthread_mutex_lock(&h->mutex);
    h->signaled = 0;
    pthread_mutex_unlock(&h->mutex);
    return TRUE;
}

/* Waits for an event (auto-reset consumes one wakeup) or joins a thread.
   Returns WAIT_OBJECT_0 or WAIT_TIMEOUT, like WaitForSingleObject. */
static inline DWORD WinHandleWait(HANDLE h, DWORD ms)
{
    struct timespec deadline;
    int infinite = ms == INFINITE;
    if (!h) return WAIT_TIMEOUT;
    if (h->kind == 2) {
        if (infinite) {
            void *ret;
            if (pthread_join(h->thread, &ret) != 0) return WAIT_TIMEOUT;
            h->joined = 1;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT; /* threads are only joined without a timeout */
    }
    if (!infinite) {
        struct timeval now;
        gettimeofday(&now, NULL);
        deadline.tv_sec = now.tv_sec + ms / 1000;
        deadline.tv_nsec = (now.tv_usec * 1000L + (ms % 1000) * 1000000L);
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    pthread_mutex_lock(&h->mutex);
    while (!h->signaled) {
        int r = infinite ? pthread_cond_wait(&h->cond, &h->mutex)
                         : pthread_cond_timedwait(&h->cond, &h->mutex, &deadline);
        if (r == ETIMEDOUT) {
            pthread_mutex_unlock(&h->mutex);
            return WAIT_TIMEOUT;
        }
    }
    if (!h->manual) h->signaled = 0;
    pthread_mutex_unlock(&h->mutex);
    return WAIT_OBJECT_0;
}

static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WinHandleWait(h, ms); }

static inline BOOL CloseHandle(HANDLE h)
{
    if (!h) return FALSE;
    if (h->kind == 1) {
        pthread_mutex_destroy(&h->mutex);
        pthread_cond_destroy(&h->cond);
    }
    free(h);
    return TRUE;
}

typedef struct {
    DWORD (WINAPI *fn)(void *);
    void *arg;
    WinHandle *self;
} WinThreadStart;

static void *win_thread_trampoline(void *p)
{
    WinThreadStart start = *(WinThreadStart *)p;
    free(p);
    win_thread_self = start.self;
    return (void *)(uintptr_t)start.fn(start.arg);
}

static inline HANDLE CreateThread(void *attrs, unsigned long stack, DWORD (WINAPI *fn)(void *), void *arg,
                                  DWORD flags, DWORD *id)
{
    WinThreadStart *start = malloc(sizeof *start);
    WinHandle *h = calloc(1, sizeof *h);
    pthread_attr_t attr;
    (void)attrs;
    (void)stack;
    (void)flags;
    if (!start || !h) {
        free(start);
        free(h);
        return 0;
    }
    h->kind = 2;
    start->fn = fn;
    start->arg = arg;
    start->self = h;
    pthread_attr_init(&attr);
    if (pthread_create(&h->thread, &attr, win_thread_trampoline, start) != 0) {
        free(start);
        free(h);
        pthread_attr_destroy(&attr);
        return 0;
    }
    pthread_attr_destroy(&attr);
    if (id) *id = (DWORD)(uintptr_t)h->thread;
    return h;
}

static inline HANDLE WinBeginThread(unsigned (*fn)(void *), void *arg)
{
    return CreateThread(NULL, 0, (DWORD(WINAPI *)(void *))fn, arg, 0, NULL);
}
#define _beginthreadex(attrs, stack, fn, arg, flags, id) \
    (WinBeginThread((unsigned (*)(void *))(fn), (arg)))

/* Critical sections, condition variables and slim locks map onto pthreads. */
typedef pthread_mutex_t CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_init(c, NULL); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_destroy(c); }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_unlock(c); }

typedef struct {
    pthread_cond_t cond;
} CONDITION_VARIABLE;

static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(&c->cond, NULL); }

static inline BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *m, DWORD ms)
{
    struct timespec deadline;
    struct timeval now;
    int r;
    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + ms / 1000;
    deadline.tv_nsec = now.tv_usec * 1000L + (ms % 1000) * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    r = pthread_cond_timedwait(&c->cond, m, &deadline);
    return r == 0 ? TRUE : FALSE;
}

static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_broadcast(&c->cond); }
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_signal(&c->cond); }

typedef pthread_rwlock_t SRWLOCK;

static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }

static inline void Sleep(DWORD ms)
{
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#ifndef _putenv_s
static inline int win_putenv_s(const char *name, const char *value)
{
    return value && *value ? setenv(name, value, 1) : unsetenv(name);
}
#define _putenv_s win_putenv_s
#endif

static inline int _fseeki64_compat(FILE *f, int64_t off, int whence) { return fseeko(f, (off_t)off, whence); }
#define _fseeki64 _fseeki64_compat

/* Performance counters and thread CPU times (graphics_timeline.h, the worker
   profiles, present.c's bench): QueryPerformanceCounter ticks in the
   monotonic clock's nanoseconds, so its frequency is 1e9. */
typedef union {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    t->QuadPart = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    return TRUE;
}

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f)
{
    f->QuadPart = 1000000000LL;
    return TRUE;
}

static inline LONG64 InterlockedExchangeAdd64(volatile LONG64 *a, LONG64 v)
{
    return __sync_fetch_and_add(a, v);
}
static inline LONG64 InterlockedCompareExchange64(volatile LONG64 *a, LONG64 e, LONG64 c)
{
    return __sync_val_compare_and_swap(a, e, c);
}

typedef struct {
    DWORD dwLowDateTime, dwHighDateTime;
} FILETIME;

static inline HANDLE GetCurrentThread(void) { return win_thread_self; }

static inline BOOL GetThreadTimes(HANDLE h, FILETIME *created, FILETIME *exited, FILETIME *kernel,
                                  FILETIME *user)
{
    uint64_t ticks;
    if (!h || h->kind != 2) return FALSE;
#if defined(__APPLE__)  /* no thread CPU clocks: Mach's thread_info */
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(h->thread), THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS)
        return FALSE;
    ticks = ((uint64_t)info.user_time.seconds + info.system_time.seconds) * 10000000ULL +
            ((uint64_t)info.user_time.microseconds + info.system_time.microseconds) * 10ULL;
#else
    clockid_t id;
    struct timespec t;
    if (pthread_getcpuclockid(h->thread, &id) != 0) return FALSE;
    if (clock_gettime(id, &t) != 0) return FALSE;
    ticks = (uint64_t)t.tv_sec * 10000000ULL + (uint64_t)t.tv_nsec / 100ULL;
#endif
    if (created) created->dwLowDateTime = created->dwHighDateTime = 0;
    if (exited) exited->dwLowDateTime = exited->dwHighDateTime = 0;
    /* No user/kernel split for a thread clock: report it all as user time. */
    if (kernel) kernel->dwLowDateTime = kernel->dwHighDateTime = 0;
    if (user) {
        user->dwLowDateTime = (DWORD)ticks;
        user->dwHighDateTime = (DWORD)(ticks >> 32);
    }
    return TRUE;
}

/* GetProcessTimes is only the wall clock here (present.c's bench divides by
   the number of threads); the per-thread split comes from GetThreadTimes. */
static inline BOOL GetProcessTimes(HANDLE h, FILETIME *created, FILETIME *exited, FILETIME *kernel,
                                   FILETIME *user)
{
    struct timespec t;
    uint64_t ticks;
    (void)h;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    ticks = (uint64_t)t.tv_sec * 10000000ULL + (uint64_t)t.tv_nsec / 100ULL;
    if (created) created->dwLowDateTime = created->dwHighDateTime = 0;
    if (exited) exited->dwLowDateTime = exited->dwHighDateTime = 0;
    if (kernel) {
        kernel->dwLowDateTime = (DWORD)ticks;
        kernel->dwHighDateTime = (DWORD)(ticks >> 32);
    }
    if (user) user->dwLowDateTime = user->dwHighDateTime = 0;
    return TRUE;
}

static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(uintptr_t)1; }

/* Milliseconds since an unspecified epoch (system start, as on Windows);
   only intervals are ever used. */
static inline unsigned long long GetTickCount64(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ULL + (unsigned long long)t.tv_nsec / 1000000ULL;
}

#define ExitProcess host_exit

#endif
