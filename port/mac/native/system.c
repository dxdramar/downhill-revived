/* Time, the app's folder, messages, the log and other small host services for the module
   (mac_host.h, finding 075). */
#include "native.h"

#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- time ---- */

static mach_timebase_info_data_t timebase;

static uint64_t ticks_to_ns(uint64_t t) { return t * timebase.numer / timebase.denom; }

u64 w2c_mac_time_ns(struct w2c_mac *unused)
{
    (void)unused;
    if (!timebase.denom) mach_timebase_info(&timebase);
    return ticks_to_ns(mach_absolute_time());
}

void w2c_mac_sleep_until_ns(struct w2c_mac *unused, u64 when)
{
    (void)unused;
    if (!timebase.denom) mach_timebase_info(&timebase);
    uint64_t now_ns = ticks_to_ns(mach_absolute_time());
    if (when <= now_ns) return;
    uint64_t wait = when - now_ns;
    /* The frame pacing's waits want the millisecond, and a process that isn't in front (or was
       started from a background shell) has its timers coalesced: they woke ~6 ms late in testing.
       The thread is a time-constraint one for the wait alone (16 us late), so none of the game's
       work runs at that priority. */
    thread_port_t self = pthread_mach_thread_np(pthread_self());
    int precise = wait < 50000000u;
    if (precise) {
        uint32_t ms = (uint32_t)(1000000ull * timebase.denom / timebase.numer);
        thread_time_constraint_policy_data_t rt = {16 * ms, ms / 2, 16 * ms, 1};
        precise = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&rt,
                                    THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    }
    mach_wait_until(mach_absolute_time() + wait * timebase.denom / timebase.numer);
    if (precise) {
        thread_standard_policy_data_t standard = {0};
        thread_policy_set(self, THREAD_STANDARD_POLICY, (thread_policy_t)&standard, THREAD_STANDARD_POLICY_COUNT);
    }
}

u64 w2c_mac_wall_time_ns(struct w2c_mac *unused)
{
    (void)unused;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

u32 w2c_mac_utc_offset_seconds(struct w2c_mac *unused)
{
    (void)unused;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    return (u32)(int32_t)local.tm_gmtoff;
}

/* ---- the app's folder ----
   dhd.ini, saves/ and dhd.log: beside the program when it is a plain executable (as on Windows,
   and what the tools expect), else ~/Library/Application Support/Downhill Revived for the .app
   (an app bundle is no place to write). DHD_HOME names another. */
static char folder[1024];

void app_folder_init(void)
{
    const char *home = getenv("DHD_HOME");
    char exe[1024];
    uint32_t size = sizeof exe;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (home && *home) snprintf(folder, sizeof folder, "%s", home);
    else if (!_NSGetExecutablePath(exe, &size) && realpath(exe, folder) && !strstr(folder, ".app/Contents/MacOS/")) {
        char *slash = strrchr(folder, '/');
        if (slash) slash[1] = 0;
    } else {
        snprintf(folder, sizeof folder, "%s/Library/Application Support/Downhill Revived", getenv("HOME"));
        mkdir(folder, 0755);
        /* the first start: the bundle's dhd.ini template (port/tools/package_mac.py) */
        char ini[1100], template[1100];
        snprintf(ini, sizeof ini, "%s/dhd.ini", folder);
        char *contents = strstr(exe, ".app/Contents/MacOS/");
        if (contents && access(ini, F_OK)) {
            snprintf(template, sizeof template, "%.*s.app/Contents/Resources/dhd.ini", (int)(contents - exe), exe);
            FILE *in = fopen(template, "rb"), *out = in ? fopen(ini, "wb") : NULL;
            char buffer[4096];
            size_t n;
            while (out && (n = fread(buffer, 1, sizeof buffer, in)) > 0) fwrite(buffer, 1, n, out);
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    size_t n = strlen(folder);
    if (n && folder[n - 1] != '/' && n + 1 < sizeof folder) folder[n] = '/', folder[n + 1] = 0;
}

const char *app_folder(void) { return folder; }

u32 w2c_mac_exe_path(struct w2c_mac *unused, u32 out, u32 size)
{
    (void)unused;
    char path[PATH_MAX], real[PATH_MAX];
    uint32_t n = sizeof path;
    if (!size || _NSGetExecutablePath(path, &n) || !realpath(path, real)) return 0;
    int k = snprintf(at(out), size, "%s", real);
    return k > 0 && (u32)k < size ? (u32)k : 0;
}

u32 w2c_mac_app_folder(struct w2c_mac *unused, u32 out, u32 size)
{
    (void)unused;
    size_t n = strlen(folder);
    if (n >= size) n = size ? size - 1 : 0;
    memcpy(at(out), folder, n);
    if (size) ((char *)at(out))[n] = 0;
    return (u32)n;
}

/* ---- the log, messages, the end ---- */

void w2c_mac_log(struct w2c_mac *unused, u32 text, u32 length)
{
    (void)unused;
    fwrite(at(text), 1, length, stderr);
}

void w2c_mac_exit(struct w2c_mac *unused, u32 code)
{
    (void)unused;
    pads_stop();
    fflush(NULL);
    _exit((int)code);
}

/* ---- SHA-256 (bcrypt.h) ---- */

enum { MAX_HASHES = 16 };
static CC_SHA256_CTX hashes[MAX_HASHES];
static volatile uint32_t hash_used[MAX_HASHES];

u32 w2c_mac_sha256_new(struct w2c_mac *unused)
{
    (void)unused;
    for (uint32_t i = 1; i < MAX_HASHES; i++)
        if (!__atomic_exchange_n(&hash_used[i], 1, __ATOMIC_ACQ_REL)) {
            CC_SHA256_Init(&hashes[i]);
            return i;
        }
    abort();
}
void w2c_mac_sha256_add(struct w2c_mac *unused, u32 h, u32 data, u32 size)
{
    (void)unused;
    CC_SHA256_Update(&hashes[h], at(data), size);
}
void w2c_mac_sha256_end(struct w2c_mac *unused, u32 h, u32 digest)
{
    (void)unused;
    CC_SHA256_Final(at(digest), &hashes[h]);
    __atomic_store_n(&hash_used[h], 0, __ATOMIC_RELEASE);
}
