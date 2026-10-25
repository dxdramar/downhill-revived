/* Mac build (finding 075): the part of the Win32 API the port's portable code uses, for the
   wasm module. Threads, locks and time come from the native host (mac_host.h), files from the
   WASI C library; port/mac/win32.c implements what isn't inline here. Window, Direct3D, WASAPI
   and WinHTTP code is not built on the Mac (each has a port/mac counterpart), so none of it is
   here. Paths may use '\\': the host's file calls take it as '/'. */
#ifndef DHD_MAC_WINDOWS_H
#define DHD_MAC_WINDOWS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mac_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WINAPI
#define CALLBACK
#define APIENTRY
#define __stdcall
#define __cdecl
#define __fastcall

typedef int BOOL, INT;
typedef float FLOAT;
typedef unsigned char BYTE, UINT8;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT, UINT32;
typedef short SHORT;
typedef int64_t LONGLONG, LONG64;
typedef uint64_t ULONGLONG, ULONG64, DWORD64, UINT64;
typedef uintptr_t DWORD_PTR, ULONG_PTR, UINT_PTR, SIZE_T;
typedef intptr_t LONG_PTR, INT_PTR, LPARAM, LRESULT;
typedef uintptr_t WPARAM;
typedef char CHAR;
typedef uint16_t WCHAR;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *HWND, *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef DWORD *LPDWORD;
typedef int (*FARPROC)(void);

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu

typedef union {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;
typedef union {
    struct { DWORD LowPart; DWORD HighPart; };
    ULONGLONG QuadPart;
} ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

/* ---- threads and synchronisation (port/mac/win32.c) ---- */

typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(void *);
HANDLE CreateThread(void *security, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *arg, DWORD flags,
                    DWORD *id);
uintptr_t _beginthreadex(void *security, unsigned stack, unsigned (__stdcall *start)(void *), void *arg,
                         unsigned flags, unsigned *id);
BOOL CloseHandle(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
#define WaitForSingleObjectEx(h, ms, alertable) WaitForSingleObject(h, ms)
HANDLE CreateEventA(void *security, BOOL manual_reset, BOOL initial, const char *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
static inline DWORD GetCurrentThreadId(void) { return mac_thread_id(); }
static inline HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static inline DWORD GetCurrentProcessId(void) { return 1; }
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
static inline BOOL SetThreadPriority(HANDLE h, int priority) { (void)h; (void)priority; return TRUE; }

/* A critical section is a recursive host lock, made at the first use. */
typedef struct { uint32_t lock; } CRITICAL_SECTION;
uint32_t win32_lock_of(uint32_t *lock);
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) { c->lock = mac_lock_new(); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *c) { if (c->lock) mac_lock_free(c->lock); c->lock = 0; }
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { mac_lock(win32_lock_of(&c->lock)); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { mac_unlock(c->lock); }

/* Slim reader/writer locks: exclusive only (a host lock). */
typedef struct { uint32_t lock; } SRWLOCK;
#define SRWLOCK_INIT {0}
static inline void InitializeSRWLock(SRWLOCK *l) { l->lock = 0; }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { mac_lock(win32_lock_of(&l->lock)); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { mac_unlock(l->lock); }
#define AcquireSRWLockShared AcquireSRWLockExclusive
#define ReleaseSRWLockShared ReleaseSRWLockExclusive

typedef struct { uint32_t cond; } CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT {0}
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { c->cond = mac_cond_new(); }
uint32_t win32_cond_of(uint32_t *cond);
static inline BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms)
{
    return mac_cond_wait(win32_cond_of(&c->cond), cs->lock, ms);
}
static inline void WakeConditionVariable(CONDITION_VARIABLE *c) { mac_cond_wake(win32_cond_of(&c->cond), 0); }
static inline void WakeAllConditionVariable(CONDITION_VARIABLE *c) { mac_cond_wake(win32_cond_of(&c->cond), 1); }

#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64 InterlockedExchangeAdd
#define InterlockedExchange64 InterlockedExchange
#define InterlockedIncrement64 InterlockedIncrement
#define InterlockedDecrement64 InterlockedDecrement
#define InterlockedCompareExchange(p, v, cmp) __sync_val_compare_and_swap((p), (cmp), (v))
#define InterlockedCompareExchange64 InterlockedCompareExchange
#define InterlockedCompareExchangePointer InterlockedCompareExchange
#define InterlockedExchangePointer InterlockedExchange
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() ((void)0)

/* ---- time ---- */

static inline void Sleep(DWORD ms) { mac_sleep_until_ns(mac_time_ns() + (uint64_t)ms * 1000000u); }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return TRUE; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *t) { t->QuadPart = (LONGLONG)mac_time_ns(); return TRUE; }
static inline ULONGLONG GetTickCount64(void) { return mac_time_ns() / 1000000u; }
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
/* FILETIME: 100 ns units since 1601. */
static inline void GetSystemTimeAsFileTime(FILETIME *f)
{
    uint64_t t = mac_wall_time_ns() / 100 + 116444736000000000ull;
    f->dwLowDateTime = (DWORD)t; f->dwHighDateTime = (DWORD)(t >> 32);
}

/* ---- files ---- */

#define FILE_ATTRIBUTE_READONLY 0x01u
#define FILE_ATTRIBUTE_HIDDEN 0x02u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define MOVEFILE_REPLACE_EXISTING 1u
#define MOVEFILE_WRITE_THROUGH 8u
DWORD GetFileAttributesA(const char *path);
BOOL SetFileAttributesA(const char *path, DWORD attributes);
BOOL CreateDirectoryA(const char *path, void *security);
BOOL DeleteFileA(const char *path);
BOOL MoveFileExA(const char *from, const char *to, DWORD flags);
BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists);
/* The program's path: a name in the program's folder (mac_app_folder), so code that cuts it at
   the last '\\' or '/' gets that folder. */
DWORD GetModuleFileNameA(HMODULE module, char *out, DWORD size);

typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *out);
BOOL FindNextFileA(HANDLE find, WIN32_FIND_DATAA *out);
BOOL FindClose(HANDLE find);

typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
BOOL GetFileAttributesExA(const char *path, GET_FILEEX_INFO_LEVELS level, void *out);

/* Whole-file handles over stdio: CreateFileA/ReadFile/WriteFile as the port uses them. */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security, DWORD disposition,
                   DWORD flags, HANDLE templ);
BOOL ReadFile(HANDLE h, void *buffer, DWORD size, DWORD *read, void *overlapped);
BOOL WriteFile(HANDLE h, const void *buffer, DWORD size, DWORD *written, void *overlapped);
BOOL FlushFileBuffers(HANDLE h);

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_ALREADY_EXISTS 183u
DWORD GetLastError(void);
void SetLastError(DWORD error);

#include "msvc_compat.h"

/* host.c checks %s arguments of the game's debug prints: a module address is readable if it is
   inside the 4 GB the host maps, which every 32-bit value is. */
static inline BOOL IsBadStringPtrA(const char *s, UINT_PTR max) { (void)max; return s == NULL; }

/* ---- messages and the process ---- */

#define MB_OK 0x0u
#define MB_OKCANCEL 0x1u
#define MB_YESNO 0x4u
#define MB_RETRYCANCEL 0x5u
#define MB_ICONERROR 0x10u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_SETFOREGROUND 0x10000u
#define MB_TOPMOST 0x40000u
#define IDOK 1
#define IDCANCEL 2
#define IDRETRY 4
#define IDYES 6
#define IDNO 7
static inline int MessageBoxA(HWND owner, const char *text, const char *caption, UINT flags)
{
    (void)owner;
    return mac_message_box(text, caption, flags);
}
static inline __attribute__((noreturn)) void ExitProcess(UINT code) { fflush(NULL); mac_exit((int)code); }
static inline BOOL TerminateProcess(HANDLE process, UINT code) { (void)process; ExitProcess(code); }

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
/* The keyboard: port/mac's keyboard state, by Windows virtual-key code. */
SHORT GetAsyncKeyState(int key);

/* ---- displays, the clipboard (port/mac/native's display and pasteboard) ---- */

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef void *HMONITOR;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u
MAC_IMPORT(display) void mac_display(uint32_t *width, uint32_t *height, uint32_t *refresh_hz);
static inline HMONITOR MonitorFromPoint(POINT p, DWORD flags) { (void)p; (void)flags; return (HMONITOR)1; }
static inline BOOL GetMonitorInfoA(HMONITOR m, MONITORINFO *info)
{
    uint32_t w, h, hz;
    (void)m;
    mac_display(&w, &h, &hz);
    info->rcMonitor.left = info->rcMonitor.top = 0;
    info->rcMonitor.right = (LONG)w; info->rcMonitor.bottom = (LONG)h;
    info->rcWork = info->rcMonitor;
    return TRUE;
}
typedef struct { WORD dmSize; DWORD dmPelsWidth, dmPelsHeight, dmDisplayFrequency; } DEVMODEA;
#define ENUM_CURRENT_SETTINGS ((DWORD)-1)
static inline BOOL EnumDisplaySettingsA(const char *device, DWORD mode, DEVMODEA *out)
{
    uint32_t w, h, hz;
    (void)device; (void)mode;
    mac_display(&w, &h, &hz);
    out->dmPelsWidth = w; out->dmPelsHeight = h; out->dmDisplayFrequency = hz;
    return TRUE;
}
/* No DLLs: optional Windows features looked up at run time are simply absent. */
static inline HMODULE GetModuleHandleA(const char *name) { (void)name; return NULL; }
static inline HMODULE LoadLibraryA(const char *name) { (void)name; return NULL; }
static inline FARPROC GetProcAddress(HMODULE module, const char *name) { (void)module; (void)name; return NULL; }

#define CF_UNICODETEXT 13u
BOOL OpenClipboard(HWND owner);   /* takes the pasteboard's text, as UTF-16 */
BOOL CloseClipboard(void);
HANDLE GetClipboardData(UINT format);
static inline void *GlobalLock(HANDLE h) { return h; }
static inline BOOL GlobalUnlock(HANDLE h) { (void)h; return TRUE; }
SIZE_T GlobalSize(HANDLE h);

/* ---- file times ---- */

BOOL FileTimeToLocalFileTime(const FILETIME *utc, FILETIME *local);
BOOL FileTimeToSystemTime(const FILETIME *time, SYSTEMTIME *out);
BOOL GetThreadTimes(HANDLE thread, FILETIME *created, FILETIME *exited, FILETIME *kernel, FILETIME *user);

/* ---- memory protection and faults: not on the Mac ----
   The module's memory can't be protected page by page, so graphics.c's deferred store-image
   reads (guard pages and a fault handler) find no handler and read at once instead. */
#define PAGE_NOACCESS 0x01u
#define PAGE_READWRITE 0x04u
typedef struct { DWORD ExceptionCode, ExceptionFlags; void *ExceptionRecord, *ExceptionAddress;
                 DWORD NumberParameters; ULONG_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; void *ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (CALLBACK *PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
static inline BOOL VirtualProtect(void *address, SIZE_T size, DWORD protection, DWORD *old)
{
    (void)address; (void)size; (void)protection; (void)old;
    return FALSE;
}
static inline void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
    (void)first; (void)handler;
    return NULL;
}


/* ---- the game's window (port/mac/native/window.m) ---- */

/* The client area of the game's window, in pixels (the backing store's). */
MAC_IMPORT(window_size) void mac_window_size(uint32_t *width, uint32_t *height);
static inline BOOL GetClientRect(HWND window, RECT *r)
{
    uint32_t w = 0, h = 0;
    if (window) mac_window_size(&w, &h);
    r->left = r->top = 0; r->right = (LONG)w; r->bottom = (LONG)h;
    return window != NULL;
}

/* No high-resolution waitable timers: callers fall back to Sleep. */
#define TIMER_ALL_ACCESS 0x1F0003u
static inline HANDLE CreateWaitableTimerExW(void *a, const void *name, DWORD flags, DWORD access)
{
    (void)a; (void)name; (void)flags; (void)access;
    return NULL;
}
static inline BOOL SetWaitableTimer(HANDLE t, const LARGE_INTEGER *due, LONG period, void *f, void *arg, BOOL resume)
{
    (void)t; (void)due; (void)period; (void)f; (void)arg; (void)resume;
    return FALSE;
}

/* Bitmap files (debug pictures) */
#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
typedef struct {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
#define BI_RGB 0

/* wasm has no return addresses to walk: diagnostics print none. */
static inline WORD CaptureStackBackTrace(DWORD skip, DWORD count, void **frames, DWORD *hash)
{
    (void)skip; (void)count; (void)frames; (void)hash;
    return 0;
}

static inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzl(mask);
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
