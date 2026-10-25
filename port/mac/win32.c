/* The Win32 calls of port/mac/include/windows.h that aren't inline there (finding 075): threads
   and events over the native host's, files over the WASI C library. */
#include <windows.h>
#include <mmdeviceapi.h>

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <time.h>

static DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD error) { last_error = error; }

static DWORD error_of(int e)
{
    return e == ENOENT ? ERROR_FILE_NOT_FOUND : e == ENOTDIR ? ERROR_PATH_NOT_FOUND
         : e == EEXIST ? ERROR_ALREADY_EXISTS : e == EACCES ? ERROR_ACCESS_DENIED : (DWORD)e + 1000;
}

/* Locks made at the first use: static CRITICAL_SECTIONs and SRWLOCK_INITs start as zero. */
static uint32_t make_once(uint32_t *slot, uint32_t (*make)(void), void (*drop)(uint32_t))
{
    uint32_t have = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (have) return have;
    uint32_t made = make(), expected = 0;
    if (__atomic_compare_exchange_n(slot, &expected, made, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return made;
    drop(made);
    return expected;
}
uint32_t win32_lock_of(uint32_t *lock) { return make_once(lock, mac_lock_new, mac_lock_free); }
uint32_t win32_cond_of(uint32_t *cond) { return make_once(cond, mac_cond_new, mac_cond_free); }

/* ---- handles: threads, events, files and searches behind one kind of pointer ---- */

enum { H_THREAD = 1, H_EVENT, H_FILE, H_FIND };
typedef struct {
    int kind;
    int audio;                               /* the sound output's event (audioclient.h) */
    uint32_t thread;                         /* H_THREAD */
    uint32_t lock, cond; int manual, set, waiters;    /* H_EVENT */
    FILE *file;                              /* H_FILE */
    DIR *dir; char folder[MAX_PATH], pattern[MAX_PATH];  /* H_FIND */
} Handle;

static Handle *new_handle(int kind)
{
    Handle *h = calloc(1, sizeof *h);
    if (h) h->kind = kind;
    return h;
}

__attribute__((export_name("mac_call"))) uint32_t mac_call(uint32_t (*fn)(void *), void *arg) { return fn(arg); }
__attribute__((export_name("mac_call_void"))) void mac_call_void(void (*fn)(void *), void *arg) { fn(arg); }

/* ---- threads ----
   A thread runs on its own module stack (the host gives it a host stack); the thread's function
   is called through mac_call, which the module exports for the host. */
enum { THREAD_STACK = 1 << 20 };
typedef struct { LPTHREAD_START_ROUTINE start; void *arg; void *stack; } Start;

static uint32_t thread_main(void *p)
{
    Start s = *(Start *)p;
    free(p);
    return s.start(s.arg);  /* the module stack stays: the thread may have left pointers into it */
}

HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *arg, DWORD flags, DWORD *id)
{
    (void)security; (void)flags;
    size_t size = stack > THREAD_STACK ? stack : THREAD_STACK;
    Start *s = malloc(sizeof *s);
    Handle *h = new_handle(H_THREAD);
    uint8_t *module_stack = malloc(size);
    if (!s || !h || !module_stack) { free(s); free(h); free(module_stack); return NULL; }
    s->start = start; s->arg = arg; s->stack = module_stack;
    h->thread = mac_thread_create(thread_main, s, module_stack + size);
    if (!h->thread) { free(s); free(h); free(module_stack); return NULL; }
    if (id) *id = h->thread;
    return h;
}

uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (__stdcall *start)(void *), void *arg,
                         unsigned flags, unsigned *id)
{
    DWORD thread;
    HANDLE h = CreateThread(security, stack, (LPTHREAD_START_ROUTINE)start, arg, flags, &thread);
    if (id) *id = thread;
    return (uintptr_t)h;
}

/* ---- events ---- */

HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name)
{
    (void)security; (void)name;
    Handle *h = new_handle(H_EVENT);
    if (!h) return NULL;
    h->lock = mac_lock_new();
    h->cond = mac_cond_new();
    h->manual = manual_reset;
    h->set = initial;
    return h;
}

BOOL SetEvent(HANDLE p)
{
    Handle *h = p;
    mac_lock(h->lock);
    h->set = 1;
    if (h->waiters) mac_cond_wake(h->cond, h->manual);  /* the VU kicks the GS this way per packet */
    mac_unlock(h->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE p)
{
    Handle *h = p;
    mac_lock(h->lock);
    h->set = 0;
    mac_unlock(h->lock);
    return TRUE;
}

/* The sound output's event: set when the output takes frames, which the host waits for itself. */
void win32_audio_event(HANDLE event)
{
    if (event) ((Handle *)event)->audio = 1;
}

DWORD WaitForSingleObject(HANDLE p, DWORD ms)
{
    Handle *h = p;
    if (!h) return WAIT_FAILED;
    if (h->audio) { mac_audio_wait(ms); return WAIT_OBJECT_0; }
    if (h->kind == H_THREAD) return mac_thread_join(h->thread, ms) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (h->kind != H_EVENT) return WAIT_FAILED;
    uint64_t until = ms == INFINITE ? 0 : mac_time_ns() + (uint64_t)ms * 1000000u;
    DWORD result = WAIT_OBJECT_0;
    mac_lock(h->lock);
    while (!h->set) {
        uint64_t now = mac_time_ns();
        if (until && now >= until) { result = WAIT_TIMEOUT; break; }
        h->waiters++;
        mac_cond_wait(h->cond, h->lock, until ? (uint32_t)((until - now + 999999) / 1000000) : INFINITE);
        h->waiters--;
    }
    if (result == WAIT_OBJECT_0 && !h->manual) h->set = 0;
    mac_unlock(h->lock);
    return result;
}

BOOL CloseHandle(HANDLE p)
{
    Handle *h = p;
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    if (h->kind == H_EVENT) mac_cond_free(h->cond), mac_lock_free(h->lock);
    if (h->kind == H_FILE && h->file) fclose(h->file);
    if (h->kind == H_FIND && h->dir) closedir(h->dir);
    free(h);  /* a thread runs on without its handle, as on Windows */
    return TRUE;
}

/* ---- files ---- */

DWORD GetModuleFileNameA(HMODULE module, char *out, DWORD size)
{
    (void)module;
    uint32_t n = mac_app_folder(out, size);
    int k = snprintf(out + n, size > n ? size - n : 0, "dhd");
    return n + (k > 0 ? (DWORD)k : 0) < size ? n + (DWORD)k : 0;
}

DWORD GetFileAttributesA(const char *path)
{
    struct stat st;
    if (stat(path, &st)) { last_error = error_of(errno); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetFileAttributesA(const char *path, DWORD attributes)
{
    (void)attributes;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void file_time(FILETIME *out, const struct timespec *t)
{
    uint64_t v = (uint64_t)t->tv_sec * 10000000u + (uint64_t)t->tv_nsec / 100 + 116444736000000000ull;
    out->dwLowDateTime = (DWORD)v; out->dwHighDateTime = (DWORD)(v >> 32);
}

BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out)
{
    struct stat st;
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    (void)level;
    if (stat(path, &st)) { last_error = error_of(errno); return FALSE; }
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    file_time(&d->ftCreationTime, &st.st_ctim);
    file_time(&d->ftLastAccessTime, &st.st_atim);
    file_time(&d->ftLastWriteTime, &st.st_mtim);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    if (!mkdir(path, 0755)) return TRUE;
    last_error = error_of(errno);
    return FALSE;
}

BOOL DeleteFileA(const char *path)
{
    if (!unlink(path)) return TRUE;
    last_error = error_of(errno);
    return FALSE;
}

BOOL MoveFileExA(const char *from, const char *to, DWORD flags)
{
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) {
        last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    if (!rename(from, to)) return TRUE;
    last_error = error_of(errno);
    return FALSE;
}

BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists)
{
    if (fail_if_exists && GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) {
        last_error = ERROR_ALREADY_EXISTS;
        return FALSE;
    }
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char buffer[65536];
    size_t n;
    int ok = in && out;
    while (ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0) ok = fwrite(buffer, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    if (!ok) last_error = error_of(errno);
    return ok;
}

HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security, DWORD disposition, DWORD flags,
                   HANDLE templ)
{
    (void)share; (void)security; (void)flags; (void)templ;
    const char *mode = access & GENERIC_WRITE
        ? (disposition == CREATE_ALWAYS ? "wb" : disposition == OPEN_EXISTING ? "r+b" : "ab")
        : "rb";
    Handle *h = new_handle(H_FILE);
    if (!h) return INVALID_HANDLE_VALUE;
    h->file = fopen(path, mode);
    if (!h->file && disposition == OPEN_ALWAYS && (access & GENERIC_WRITE)) h->file = fopen(path, "w+b");
    if (!h->file) { last_error = error_of(errno); free(h); return INVALID_HANDLE_VALUE; }
    return h;
}

BOOL ReadFile(HANDLE p, void *buffer, DWORD size, DWORD *read, void *overlapped)
{
    Handle *h = p;
    (void)overlapped;
    size_t n = fread(buffer, 1, size, h->file);
    if (read) *read = (DWORD)n;
    return n == size || !ferror(h->file);
}

BOOL WriteFile(HANDLE p, const void *buffer, DWORD size, DWORD *written, void *overlapped)
{
    Handle *h = p;
    (void)overlapped;
    size_t n = fwrite(buffer, 1, size, h->file);
    if (written) *written = (DWORD)n;
    return n == size;
}

BOOL FlushFileBuffers(HANDLE p) { return fflush(((Handle *)p)->file) == 0; }

/* ---- searches: FindFirstFileA's "folder\\pattern" ---- */

static BOOL find_next(Handle *h, WIN32_FIND_DATAA *out)
{
    struct dirent *e;
    while ((e = readdir(h->dir))) {
        if (fnmatch(h->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char path[MAX_PATH * 2];
        WIN32_FILE_ATTRIBUTE_DATA d;
        snprintf(path, sizeof path, "%s/%s", h->folder, e->d_name);
        memset(out, 0, sizeof *out);
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
            out->dwFileAttributes = d.dwFileAttributes;
            out->ftCreationTime = d.ftCreationTime;
            out->ftLastAccessTime = d.ftLastAccessTime;
            out->ftLastWriteTime = d.ftLastWriteTime;
            out->nFileSizeHigh = d.nFileSizeHigh;
            out->nFileSizeLow = d.nFileSizeLow;
        }
        snprintf(out->cFileName, sizeof out->cFileName, "%s", e->d_name);
        return TRUE;
    }
    last_error = ERROR_FILE_NOT_FOUND;
    return FALSE;
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *out)
{
    Handle *h = new_handle(H_FIND);
    if (!h) return INVALID_HANDLE_VALUE;
    const char *cut = strrchr(pattern, '/'), *back = strrchr(pattern, '\\');
    if (!cut || (back && back > cut)) cut = back;
    snprintf(h->folder, sizeof h->folder, "%.*s", cut ? (int)(cut - pattern) : 1, cut ? pattern : ".");
    snprintf(h->pattern, sizeof h->pattern, "%s", cut ? cut + 1 : pattern);
    if (!(h->dir = opendir(h->folder)) || !find_next(h, out)) {
        if (!h->dir) last_error = error_of(errno);
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA *out) { return find_next(find, out); }
BOOL FindClose(HANDLE find) { return CloseHandle(find); }

/* ---- times ---- */

BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local)
{
    int64_t t = (int64_t)((uint64_t)utc->dwHighDateTime << 32 | utc->dwLowDateTime);
    t += (int64_t)mac_utc_offset_seconds() * 10000000;
    local->dwLowDateTime = (DWORD)t; local->dwHighDateTime = (DWORD)((uint64_t)t >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *time, SYSTEMTIME *out)
{
    uint64_t t = (uint64_t)time->dwHighDateTime << 32 | time->dwLowDateTime;
    time_t seconds = (time_t)(t / 10000000u - 11644473600ull);
    struct tm tm;
    gmtime_r(&seconds, &tm);
    out->wYear = (WORD)(tm.tm_year + 1900); out->wMonth = (WORD)(tm.tm_mon + 1);
    out->wDayOfWeek = (WORD)tm.tm_wday; out->wDay = (WORD)tm.tm_mday;
    out->wHour = (WORD)tm.tm_hour; out->wMinute = (WORD)tm.tm_min; out->wSecond = (WORD)tm.tm_sec;
    out->wMilliseconds = (WORD)(t / 10000u % 1000u);
    return TRUE;
}

BOOL GetThreadTimes(HANDLE thread, FILETIME *created, FILETIME *exited, FILETIME *kernel, FILETIME *user)
{
    (void)thread;
    memset(created, 0, sizeof *created); memset(exited, 0, sizeof *exited);
    memset(kernel, 0, sizeof *kernel); memset(user, 0, sizeof *user);
    return FALSE;
}

/* ---- the clipboard: the pasteboard's text, kept as UTF-16 ---- */

static WCHAR clipboard[4096];
static SIZE_T clipboard_bytes;

BOOL OpenClipboard(HWND owner)
{
    (void)owner;
    clipboard_bytes = mac_clipboard_utf16(clipboard, sizeof clipboard / sizeof *clipboard) * sizeof(WCHAR);
    return TRUE;
}
BOOL CloseClipboard(void) { return TRUE; }
HANDLE GetClipboardData(UINT format) { return format == CF_UNICODETEXT && clipboard_bytes ? clipboard : NULL; }
SIZE_T GlobalSize(HANDLE h) { return h == clipboard ? clipboard_bytes : 0; }

/* ---- the keyboard ---- */

SHORT GetAsyncKeyState(int key) { return mac_key_down(key) ? (SHORT)0x8000 : 0; }

/* ---- the working folder ----
   Relative paths are the app's folder's (port/mac/native: beside the program, or the app's
   Application Support folder), as they are dhd.exe's on Windows. */
__attribute__((constructor)) static void start_in_app_folder(void)
{
    char folder[MAX_PATH];
    if (mac_app_folder(folder, sizeof folder)) chdir(folder);
}
