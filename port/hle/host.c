#include "text_input.h"
/* The Windows calls of the native PS2 system (hle.h), apart from its code because the PS2
   kernel's names (CreateThread, ExitThread ...) are Windows' too. */
#include "hle.h"
#include "timer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#ifdef _WIN32
#include <io.h>        /* _get_osfhandle */
#include <commdlg.h>  /* the file-open dialog's struct; comdlg32 itself is loaded when needed */
#endif

#include "ps2asm.h"

#ifdef _WIN32
/* Where the game thread is: its registers, the code addresses on its stack (no frame
   pointers, so likely return addresses, some stale; build/port/link/dhd.map names them) and the
   recompiled code's registers. */
typedef struct {
    CONTEXT context;
    uint32_t returns[32], registers[5];
    unsigned count;
} Snapshot;

/* No CRT calls while the game thread is suspended: it may own a CRT lock. */
static void snapshot(Snapshot *out, const CONTEXT *c)
{
    out->context = *c;
    out->count = 0;
    extern char __ImageBase[];
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(__ImageBase + ((IMAGE_DOS_HEADER *)__ImageBase)->e_lfanew);
    uintptr_t lo = (uintptr_t)__ImageBase + nt->OptionalHeader.BaseOfCode;
    uintptr_t hi = lo + nt->OptionalHeader.SizeOfCode;
    const uint32_t *sp = (const uint32_t *)(uintptr_t)c->Esp;
    for (unsigned i = 0; i < 2048 && out->count < 32; i++) {
        uint32_t value;
        SIZE_T read;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &value, 4, &read) || read != 4) break;
        if (value >= lo && value < hi) out->returns[out->count++] = value;
    }
    out->registers[0] = ps2.gpr[29].w[0]; out->registers[1] = ps2.gpr[31].w[0];
    out->registers[2] = ps2.gpr[4].w[0]; out->registers[3] = ps2.gpr[5].w[0];
    out->registers[4] = ps2.gpr[2].w[0];
}

static void print_snapshot(const Snapshot *s)
{
    const CONTEXT *c = &s->context;
    fprintf(stderr, "  eip %08lX eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX esp %08lX\n",
            c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp);
    fprintf(stderr, "  code addresses on the stack:");
    for (unsigned i = 0; i < s->count; i++)
        fprintf(stderr, "%s%08X", i % 8 ? " " : "\n    ", s->returns[i]);
    fprintf(stderr, "\n  ps2: sp %08X ra %08X a0 %08X a1 %08X v0 %08X\n",
            s->registers[0], s->registers[1], s->registers[2], s->registers[3], s->registers[4]);
    fflush(stderr);
}

static void report(const CONTEXT *c)
{
    Snapshot s;
    snapshot(&s, c);
    print_snapshot(&s);
}

/* DHD_PEEK=address,bytes (host, hex): memory too, after a dump or a crash report. */
static void peek(void)
{
    const char *peek = getenv("DHD_PEEK");
    unsigned addr, len;
    if (peek && sscanf(peek, "%x,%x", &addr, &len) == 2)
        for (unsigned i = 0; i < len && !IsBadReadPtr((void *)(uintptr_t)(addr + i), 4); i += 4)
            fprintf(stderr, "%s%08X", i % 32 ? " " : i ? "\n  " : "  ", *(uint32_t *)(uintptr_t)(addr + i));
    fprintf(stderr, "\n");
}

static LONG WINAPI crashed(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    fprintf(stderr, "crash: exception %08lX at %08lX", r->ExceptionCode, (unsigned long)(uintptr_t)r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " (%s %08lX)", r->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long)r->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    report(e->ContextRecord);
    peek();
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Game units built with DHD_STACK_PROTECT=1 (port/tools/build.py) check their frame cookie on
   return. Replacing the CRT's check (which fast-fails without a word) names the function whose
   locals overflowed: the return address is inside it. */
extern uintptr_t __security_cookie;

void __fastcall __security_check_cookie(uintptr_t cookie)
{
    if (cookie == __security_cookie) return;
    fprintf(stderr, "crash: stack buffer overflow detected in the function at %p\n",
            __builtin_return_address(0));
    fflush(stderr);
    ExitProcess(3);
}

#endif

/* vfprintf that checks each %s pointer first (the game's debug prints get bad ones). */
void host_safe_vprint(FILE *f, const char *fmt, va_list ap)
{
    char spec[32];
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fputc(*p, f);
            continue;
        }
        size_t n = strspn(p + 1, "-+ #0123456789.lhzjt");
        if (n + 3 > sizeof spec || !p[n + 1]) {
            fputs(p, f);
            break;
        }
        memcpy(spec, p, n + 2);
        spec[n + 2] = 0;
        char conv = p[n + 1];
        p += n + 1;
        if (conv == '%')
            fputc('%', f);
        else if (conv == 's') {
            const char *s = va_arg(ap, const char *);
            fprintf(f, IsBadStringPtrA(s, 256) ? "<bad %p>" : spec, s);
        } else if (strchr("eEfgGaA", conv))
            fprintf(f, spec, va_arg(ap, double));
        else if (strstr(spec, "ll"))
            fprintf(f, spec, va_arg(ap, long long));
        else if (conv == 'n')
            (void)va_arg(ap, void *);
        else
            fprintf(f, spec, va_arg(ap, int));
    }
}

#ifdef _WIN32
void host_backtrace(void)
{
    CONTEXT c = {0};
    c.Eip = (DWORD)(uintptr_t)__builtin_return_address(0);
    c.Esp = (DWORD)(uintptr_t)__builtin_frame_address(0);
    report(&c);
}

/* DHD_DUMP=N: after N seconds, where the game thread is (then it goes on). */
static HANDLE game_thread;

/* DHD_WATCH=address[:rw],... (EE addresses, hex, up to 4): hardware data watchpoints on the
   game thread (all its fibers), on stores, or with :rw on loads too. Each hit (the first 40 per
   watchpoint) prints the word, the code address after the access and the code addresses on the
   stack, for port/tools/addr2sym.py. A diagnostic (finding 041: host addresses in EE memory). */
static uint32_t watch_address[4];
static LONG watch_hits[4];

static LONG WINAPI watch_hit(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 15)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; i++)
        if (c->Dr6 & 1u << i && InterlockedIncrement(&watch_hits[i]) <= 40) {
            extern uint64_t kernel_vsync_count;
            uint32_t value = 0;
            SIZE_T got;  /* a kernel read: a plain load would hit a load watchpoint again */
            ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)(0x08000000u + watch_address[i]), &value, 4, &got);
            fprintf(stderr, "watch: EE %08X = %08X field %llu after eip %08lX\n", watch_address[i], value,
                    (unsigned long long)kernel_vsync_count, c->Eip);
            report(c);
        }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watch_arm(void *unused)
{
    CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
    const char *spec = getenv("DHD_WATCH");
    (void)unused;
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    GetThreadContext(game_thread, &c);
    for (int i = 0; i < 4 && spec && *spec; i++) {
        char *end;
        watch_address[i] = (uint32_t)strtoul(spec, &end, 16) & ~3u;
        int rw = !strncmp(end, ":rw", 3);
        (&c.Dr0)[i] = 0x08000000u + watch_address[i];
        c.Dr7 |= 1u << (2 * i) | (rw ? 3u : 1u) << (16 + 4 * i) | 3u << (18 + 4 * i);  /* local, 4 bytes */
        fprintf(stderr, "watch: EE %08X on %s\n", watch_address[i], rw ? "loads and stores" : "stores");
        spec = strchr(end, ',');
        spec = spec ? spec + 1 : NULL;
    }
    SetThreadContext(game_thread, &c);
    ResumeThread(game_thread);
    return 0;
}
static volatile LONG dump_pending;

/* Kernel details are printed by the game thread at its next safe boundary. */
void host_poll_dump(void)
{
    if (InterlockedExchange(&dump_pending, 0)) kernel_dump();
}

static DWORD WINAPI watchdog(void *seconds)
{
    Sleep((DWORD)(uintptr_t)seconds * 1000);
    Snapshot s;
    CONTEXT c = {.ContextFlags = CONTEXT_FULL};
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    BOOL ok = GetThreadContext(game_thread, &c);
    if (ok) snapshot(&s, &c);
    ResumeThread(game_thread);
    if (!ok) return 0;
    fprintf(stderr, "dump after %u s:\n", (unsigned)(uintptr_t)seconds);
    print_snapshot(&s);
    peek();
    InterlockedExchange(&dump_pending, 1);
    return 0;
}

/* The port's own OS threads by name (game, vu, gs, native, sound): the bench reports each one's
   CPU time and DHD_PROFILE_THREAD picks the one the sampling profiler watches. */
enum { NAMED_THREADS = 16 };
static struct { const char *name; HANDLE handle; } named[NAMED_THREADS];
static volatile LONG named_count;

void host_thread_name(const char *name)
{
    LONG at = InterlockedIncrement(&named_count) - 1;
    if (at >= NAMED_THREADS) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &named[at].handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    named[at].name = name;  /* published last: a reader skips a slot still being filled */
}

unsigned host_thread_cpu(const char **names, double *seconds, unsigned max)
{
    unsigned n = 0;
    for (LONG i = 0; i < named_count && i < NAMED_THREADS && n < max; i++) {
        FILETIME created, exited, kernel, user;
        if (!named[i].name || !GetThreadTimes(named[i].handle, &created, &exited, &kernel, &user)) continue;
        names[n] = named[i].name;
        seconds[n++] = (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                        ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
    }
    return n;
}

static HANDLE named_thread(const char *name)
{
    for (LONG i = 0; i < named_count && i < NAMED_THREADS; i++)
        if (named[i].name && !strcmp(named[i].name, name)) return named[i].handle;
    return NULL;
}

/* Optional statistical profile of the game thread, including driver waits.
   Capture a short stack while stopped; write only after resuming, since the
   interrupted code may hold CRT locks. The binary trace stays under build/. */
static DWORD WINAPI profiler(void *path)
{
    FILE *f = fopen(path, "wb");
    DWORD started = GetTickCount();
    unsigned buffered = 0;
    if (!f) return 0;
    fwrite("DHP4", 4, 1, f);  /* v4: the virtual field (kernel_vsync_count) in place of the scanout count */
    extern char __ImageBase[];
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(__ImageBase + ((IMAGE_DOS_HEADER *)__ImageBase)->e_lfanew);
    uintptr_t lo = (uintptr_t)__ImageBase + nt->OptionalHeader.BaseOfCode;
    uintptr_t hi = lo + nt->OptionalHeader.SizeOfCode;
    /* DHD_PROFILE_THREAD=vu (or gs, native, sound) samples that thread instead, once it exists. */
    const char *which = getenv("DHD_PROFILE_THREAD");
    HANDLE thread = which ? NULL : game_thread;
    for (;;) {
        uint32_t sample[20] = {0}, stack[2048];
        CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
        SIZE_T got;
        Sleep(which ? 1 : 5);
        if (!thread && !(thread = named_thread(which))) continue;
        if (SuspendThread(thread) == (DWORD)-1) break;
        BOOL ok = GetThreadContext(thread, &c);
        if (ok) {
            sample[0] = GetTickCount() - started;
            sample[1] = c.Eip;
            extern uint64_t kernel_vsync_count;
            ReadProcessMemory(GetCurrentProcess(), (const void *)&kernel_vsync_count, sample + 2, 4, &got);
            ReadProcessMemory(GetCurrentProcess(), (void *)0x0829DED8, sample + 3, 4, &got);
            ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)c.Esp, stack, sizeof stack, &got);
        }
        ResumeThread(thread);
        if (ok) {
            unsigned count = 4;
            for (unsigned i = 0; i < got / 4 && count < 20; i++)
                if (stack[i] >= lo && stack[i] < hi) sample[count++] = stack[i];
        }
        if (ok) fwrite(sample, sizeof sample, 1, f);
        if (++buffered == 64) { fflush(f); buffered = 0; }
    }
    fclose(f);
    return 0;
}

void host_init(void)
{
    SetUnhandledExceptionFilter(crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *dump = getenv("DHD_DUMP");
    const char *profile = getenv("DHD_PROFILE");
    host_thread_name("game");
    if (getenv("DHD_WATCH")) {
        HANDLE armer;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        AddVectoredExceptionHandler(1, watch_hit);
        armer = CreateThread(NULL, 0, watch_arm, NULL, 0, NULL);
        WaitForSingleObject(armer, INFINITE);
        CloseHandle(armer);
    }
    if (dump || profile) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        if (dump) CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)atoi(dump), 0, NULL);
        if (profile) CreateThread(NULL, 0, profiler, (void *)profile, 0, NULL);
    }
}

/* dhd.exe is a Windows program (/subsystem:windows), so a double-click opens no console window.
   Its log (stderr, stdout) goes where the starter put it: the tools' files and pipes as before;
   started from a console window, that console; else (Explorer) dhd.log beside dhd.exe, which the
   relaunched process appends to. Before anything prints. */
void host_console_init(void)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
    } else {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        snprintf(path + n, sizeof path - n, "dhd.log");
        if (!freopen(path, getenv("DHD_RELAUNCHED") ? "a" : "w", stderr)) return;
        freopen(path, "a", stdout);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
}

/* Runs this program again with host [at, at + size) reserved before anything loads (by main(),
   DLLs and the loader's threads have taken low memory, memory.c PS2_LOW_VIEW); the child ends
   with this process (a job). Returns the child's exit code, -1 if it couldn't start. */
int host_relaunch_reserving(uintptr_t at, size_t size)
{
    char exe[MAX_PATH];
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {0};
    HANDLE job = CreateJobObjectA(NULL, NULL);
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    SetEnvironmentVariableA("DHD_RELAUNCHED", "1");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    /* A Windows-subsystem child gets the log handles only when given them (host_console_init). */
    fflush(stdout); fflush(stderr);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    HANDLE std[3] = {si.hStdInput, si.hStdOutput, si.hStdError};
    for (int i = 0; i < 3; i++)
        if (std[i] && std[i] != INVALID_HANDLE_VALUE) SetHandleInformation(std[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    /* A child gets normal priority whatever its parent's; keep the one the launcher chose. */
    if (!CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED | GetPriorityClass(GetCurrentProcess()),
                        NULL, NULL, &si, &pi)) {
        fprintf(stderr, "can't start %s (error %lu)\n", exe, GetLastError());
        return -1;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    AllowSetForegroundWindow(pi.dwProcessId);  /* the start window has the foreground: the game's takes it */
    if (VirtualAllocEx(pi.hProcess, (void *)at, size, MEM_RESERVE, PAGE_NOACCESS) != (void *)at)
        fprintf(stderr, "can't reserve %08lX-%08lX for PS2 RAM (error %lu)\n", (unsigned long)at,
                (unsigned long)(at + size), GetLastError());
    /* The scratchpad, the EE registers, VU memory and the GS registers too (memory.c alloc_at
       commits them): a DLL that loads later (an overlay, an antivirus) can't land there. */
    static const uintptr_t fixed[] = {0x70000000, 0x10000000, 0x11000000, 0x12000000};
    for (int i = 0; i < 4; i++)
        if (VirtualAllocEx(pi.hProcess, (void *)fixed[i], 0x10000, MEM_RESERVE, PAGE_NOACCESS) != (void *)fixed[i])
            fprintf(stderr, "can't reserve %08lX for the PS2's fixed memory (error %lu)\n", (unsigned long)fixed[i],
                    GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

/* ---- controllers ---- */
#pragma comment(lib, "user32")

typedef struct {  /* XINPUT_STATE */
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} XState;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);
typedef struct { WORD left, right; } XVibration;  /* XINPUT_VIBRATION: low- and high-frequency motor */
typedef DWORD(WINAPI *XInputSetStateFn)(DWORD, XVibration *);
static XInputSetStateFn xinput_set;

static XInputGetStateFn xinput(void)
{
    static int tried;
    static XInputGetStateFn fn;
    if (!tried++) {
        HMODULE dll = LoadLibraryA("xinput1_4.dll");
        if (!dll)
            dll = LoadLibraryA("xinput9_1_0.dll");
        if (dll) {
            fn = (XInputGetStateFn)(void *)GetProcAddress(dll, "XInputGetState");
            xinput_set = (XInputSetStateFn)(void *)GetProcAddress(dll, "XInputSetState");
        }
    }
    return fn;
}

#endif

/* The DualShock 2's motors (pad.c, scePadSetActDirect): the small one on or off, the large one
   0-255. A pad gets them as its high- and low-frequency motors (SDL: every pad with motors,
   gamepad.c; without SDL, XInput pads only), unless DHD_VIBRATION=0 (dhd.ini "vibration", the
   PC settings page). Output only: nothing comes back into the game. */
void host_pad_vibrate(int port, int light, int heavy)
{
    static int last[4] = {-1, -1, -1, -1};
    const char *setting = getenv("DHD_VIBRATION");
    if (setting && !atoi(setting)) light = heavy = 0;
    int key = (light ? 256 : 0) | (heavy & 255);
    if (port < 0 || port > 3 || key == last[port]) return;
    last[port] = key;
    if (gamepad_start()) {
        gamepad_vibrate(port, light, heavy);
        return;
    }
#ifdef _WIN32
    xinput();
    if (!xinput_set) return;
    XVibration v = {(WORD)((heavy & 255) * 257), (WORD)(light ? 0xFFFF : 0)};
    xinput_set((DWORD)port, &v);
#endif
}



#ifdef _WIN32
/* Sony controllers (DualSense, DualSense Edge, DualShock 4) through raw HID: Windows doesn't
   offer them as XInput pads. A reader thread opens the first one it finds, over USB or
   Bluetooth, and keeps its latest input report; it looks again every second while none is
   connected. Report layouts as Linux's hid-playstation and hid-sony drivers read them. */
typedef struct { ULONG size; USHORT vendor, product, version; } HidAttributes;
typedef struct { DWORD size; GUID class_guid; DWORD flags; ULONG_PTR reserved; } DevInterfaceData;
typedef struct { DWORD size; char path[1]; } DevInterfaceDetail;
typedef void(WINAPI *HidGetGuidFn)(GUID *);
typedef BOOLEAN(WINAPI *HidGetAttributesFn)(HANDLE, HidAttributes *);
typedef BOOLEAN(WINAPI *HidGetFeatureFn)(HANDLE, void *, ULONG);
typedef void *(WINAPI *GetClassDevsFn)(const GUID *, const char *, HWND, DWORD);
typedef BOOL(WINAPI *EnumInterfacesFn)(void *, void *, const GUID *, DWORD, DevInterfaceData *);
typedef BOOL(WINAPI *GetInterfaceDetailFn)(void *, DevInterfaceData *, DevInterfaceDetail *, DWORD, DWORD *, void *);
typedef BOOL(WINAPI *DestroyListFn)(void *);

enum { SONY_NONE, SONY_DUALSENSE, SONY_DS4 };
typedef struct {  /* sticks, then the buttons and triggers as the DualShock 4's simple report has them */
    uint8_t lx, ly, rx, ry, face, shoulder, l2, r2;
} SonyState;

static CRITICAL_SECTION sony_lock;
static SonyState sony_state;
static volatile LONG sony_connected;

static HANDLE sony_open(int *kind)
{
    HMODULE hid = LoadLibraryA("hid.dll"), setup = LoadLibraryA("setupapi.dll");
    HidGetGuidFn get_guid;
    HidGetAttributesFn get_attributes;
    HidGetFeatureFn get_feature;
    GetClassDevsFn get_class_devs;
    EnumInterfacesFn enum_interfaces;
    GetInterfaceDetailFn get_detail;
    DestroyListFn destroy;
    GUID guid;
    void *list;
    HANDLE found = INVALID_HANDLE_VALUE;
    DWORD i;
    if (!hid || !setup)
        return found;
    get_guid = (HidGetGuidFn)(void *)GetProcAddress(hid, "HidD_GetHidGuid");
    get_attributes = (HidGetAttributesFn)(void *)GetProcAddress(hid, "HidD_GetAttributes");
    get_feature = (HidGetFeatureFn)(void *)GetProcAddress(hid, "HidD_GetFeature");
    get_class_devs = (GetClassDevsFn)(void *)GetProcAddress(setup, "SetupDiGetClassDevsA");
    enum_interfaces = (EnumInterfacesFn)(void *)GetProcAddress(setup, "SetupDiEnumDeviceInterfaces");
    get_detail = (GetInterfaceDetailFn)(void *)GetProcAddress(setup, "SetupDiGetDeviceInterfaceDetailA");
    destroy = (DestroyListFn)(void *)GetProcAddress(setup, "SetupDiDestroyDeviceInfoList");
    if (!get_guid || !get_attributes || !get_class_devs || !enum_interfaces || !get_detail || !destroy)
        return found;
    get_guid(&guid);
    list = get_class_devs(&guid, NULL, NULL, 0x12 /* DIGCF_PRESENT | DIGCF_DEVICEINTERFACE */);
    if (list == INVALID_HANDLE_VALUE)
        return found;
    for (i = 0; found == INVALID_HANDLE_VALUE; i++) {
        union { DevInterfaceDetail d; char bytes[1024]; } detail;
        HidAttributes attributes = {sizeof attributes};
        DevInterfaceData data = {sizeof data};
        HANDLE h;
        if (!enum_interfaces(list, NULL, &guid, i, &data))
            break;
        detail.d.size = sizeof(DWORD) + sizeof(char);  /* the fixed part's size, as SetupAPI checks it */
        if (!get_detail(list, &data, &detail.d, sizeof detail, NULL, NULL))
            continue;
        h = CreateFileA(detail.d.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (get_attributes(h, &attributes) && attributes.vendor == 0x054C) {
            USHORT p = attributes.product;
            *kind = p == 0x0CE6 || p == 0x0DF2 ? SONY_DUALSENSE
                  : p == 0x05C4 || p == 0x09CC || p == 0x0BA0 ? SONY_DS4 : SONY_NONE;
            if (*kind != SONY_NONE) {
                /* Over Bluetooth, reading the calibration feature report switches the pad to its
                   full report (0x31 DualSense, 0x11 DualShock 4); over USB it changes nothing. */
                uint8_t feature[64] = {*kind == SONY_DUALSENSE ? 0x05 : 0x02};
                if (get_feature)
                    get_feature(h, feature, *kind == SONY_DUALSENSE ? 41 : 37);
                found = h;
                continue;
            }
        }
        CloseHandle(h);
    }
    destroy(list);
    return found;
}

/* One input report to the common state; 0 if it isn't one we read. */
static int sony_parse(int kind, const uint8_t *r, DWORD n, SonyState *s)
{
    const uint8_t *b;
    if (kind == SONY_DUALSENSE && (r[0] == 0x01 || r[0] == 0x31) && n >= 64) {
        /* USB (0x01) or full Bluetooth (0x31, one more header byte): sticks, L2, R2, a counter,
           then the buttons */
        b = r + (r[0] == 0x31 ? 2 : 1);
        s->lx = b[0], s->ly = b[1], s->rx = b[2], s->ry = b[3], s->l2 = b[4], s->r2 = b[5];
        s->face = b[7], s->shoulder = b[8];
        return 1;
    }
    if (kind == SONY_DS4 && r[0] == 0x11 && n >= 12)  /* DualShock 4, full Bluetooth */
        b = r + 3;
    else if (r[0] == 0x01 && n >= 10)  /* DualShock 4 over USB; either pad's simple Bluetooth report */
        b = r + 1;
    else
        return 0;
    s->lx = b[0], s->ly = b[1], s->rx = b[2], s->ry = b[3];
    s->face = b[4], s->shoulder = b[5], s->l2 = b[7], s->r2 = b[8];
    return 1;
}

static DWORD WINAPI sony_reader(void *unused)
{
    (void)unused;
    for (;;) {
        int kind = SONY_NONE;
        HANDLE h = sony_open(&kind);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        fprintf(stderr, "controller: %s connected\n", kind == SONY_DUALSENSE ? "DualSense" : "DualShock 4");
        for (;;) {
            uint8_t report[128];
            DWORD n = 0;
            SonyState s;
            if (!ReadFile(h, report, sizeof report, &n, NULL))
                break;
            if (!sony_parse(kind, report, n, &s))
                continue;
            EnterCriticalSection(&sony_lock);
            sony_state = s;
            LeaveCriticalSection(&sony_lock);
            InterlockedExchange(&sony_connected, 1);
        }
        InterlockedExchange(&sony_connected, 0);
        CloseHandle(h);
        fprintf(stderr, "controller: disconnected\n");
    }
    return 0;
}

static int sony_read(SonyState *out)
{
    static volatile LONG started;
    if (!InterlockedExchange(&started, 1)) {
        InitializeCriticalSection(&sony_lock);
        CloseHandle(CreateThread(NULL, 0, sony_reader, NULL, 0, NULL));
    }
    if (!sony_connected)
        return 0;
    EnterCriticalSection(&sony_lock);
    *out = sony_state;
    LeaveCriticalSection(&sony_lock);
    return 1;
}

#endif

/* The pressure byte of each pressure-sensitive button, in the DualShock 2's order. */
static const uint16_t pressure_order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                            PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};

static uint8_t stick(SHORT v, int flip)
{
    int x = (v >> 8) + 128;
    if (flip)
        x = 255 - x;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* The game's controls are in riding mode (D_0029E290, func_00220550): keyboard.c moves the left
   stick with the arrows then, and the d-pad in menus. */
static int riding_controls(void) { return *(volatile int32_t *)PS2_HOST(0x29E290) != 0; }

#ifdef _WIN32
/* The keyboard (layout: keyboard.c), only while a window of this process (the game's) is in
   front, and not while Alt, Ctrl or a Windows key is held, so Alt+Tab, Alt+Enter, Ctrl+Esc and
   the like stay the system's. A key let go elsewhere is let go here too. */
static void keyboard_merge(HostPad *out)
{
    uint8_t down[256] = {0};
    DWORD pid = 0;
    HWND front = GetForegroundWindow();
    if (front) GetWindowThreadProcessId(front, &pid);
    if (front && pid == GetCurrentProcessId() && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
        !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_LWIN) & 0x8000) &&
        !(GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        /* Letters are places on the keyboard, named as on a US one: the key where a US keyboard
           has Z is Square on an AZERTY (W) or QWERTZ (Y) one too. Their scan codes, A to Z: */
        static const uint8_t us_scan[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(front, NULL));
        int count;
        const uint8_t *keys = keyboard_keys(&count);
        for (int i = 0; i < count; i++) {
            UINT vk = keys[i];
            if (vk >= 'A' && vk <= 'Z') {
                UINT here = MapVirtualKeyExA(us_scan[vk - 'A'], MAPVK_VSC_TO_VK, layout);
                if (here) vk = here;
            }
            if (text_input_key_allowed(vk) && (GetAsyncKeyState((int)vk) & 0x8000)) down[keys[i]] = 1;
        }
    }
    keyboard_pad(down, riding_controls(), host_seconds(), out);
}

#else
/* The keyboard (layout: keyboard.c), while the game's window is in front (port/mac's
   keyboard state is empty otherwise); letters by their place on the keyboard, as the Mac's key
   codes are. */
static void keyboard_merge(HostPad *out)
{
    uint8_t down[256] = {0};
    int count;
    const uint8_t *keys = keyboard_keys(&count);
    for (int i = 0; i < count; i++)
        if (text_input_key_allowed(keys[i]) && (GetAsyncKeyState(keys[i]) & 0x8000)) down[keys[i]] = 1;
    keyboard_pad(down, riding_controls(), host_seconds(), out);
}

#endif

/* DHD_KEYS (tests, finding 059): fake keys from the race's GO on, as DHD_RACE_PAD, through the
   keyboard's mapping; each change is logged with the steering the rider got from it. */
static void keys_script(HostPad *out)
{
    const char *script = getenv("DHD_KEYS");
    double seconds = race_sync_seconds();
    uint8_t down[256];
    if (!script || seconds < 0) return;
    if (!keyboard_script(script, seconds, down)) {
        fprintf(stderr, "Invalid DHD_KEYS: expected start:duration:key[+key],...\n");
        exit(1);
    }
    keyboard_pad(down, riding_controls(), host_seconds(), out);
    /* the human rider (D_002AF2C0 + 0x50130) and its steering input, right +0x1190, left +0x1194 */
    uintptr_t rider = *(volatile uint32_t *)PS2_HOST(0x2FF3F0);
    int right = 0, left = 0;
    if (rider >= 0x400000 && rider < PS2_HOST(PS2_RAM_SIZE)) right = *(volatile int32_t *)(rider + 0x1190), left = *(volatile int32_t *)(rider + 0x1194);
    static int last[5] = {-1};
    int now[5] = {out->buttons, out->left_x, out->left_y, right, left};
    if (memcmp(now, last, sizeof now)) {
        hle_log("keys race=%.3f riding=%d buttons=%04X lx=%d ly=%d steer right=%d left=%d\n", seconds,
                riding_controls(), out->buttons, out->left_x, out->left_y, right, left);
        memcpy(last, now, sizeof now);
    }
}

/* DHD_NAV, DHD_AUTOPAD or DHD_AUTOPAD_FRAMES: every player gets the script's buttons (the same
   chord on every pad, so the menus accept any of them); from the race countdown DHD_RACE_PAD
   (player 0) or DHD_RACE_PAD<n> (player n, else DHD_RACE_PAD) drives each player. Returns 0
   when no script is set. */
int host_pad_script(int player, HostPad *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    const char *script = getenv("DHD_AUTOPAD");
    const char *frame_script = getenv("DHD_AUTOPAD_FRAMES");
    const char *nav = getenv("DHD_NAV");  /* menus by the screen shown (autopad.c), else by time */
    if (frame_script) script = frame_script;
    if (nav) script = nav;
    if (!script)
        return 0;
    extern uint64_t kernel_vsync_count;
    double input_time = frame_script ? (double)(kernel_vsync_count ? kernel_vsync_count - 1 : 0) : host_seconds();
    if (nav ? !autopad_nav(nav, kernel_vsync_count, &out->buttons) :
              !autopad_read(script, input_time, &out->buttons)) {
        if (nav) fprintf(stderr, "Invalid DHD_NAV: expected menu[/phase]:button[+button],...\n");
        else fprintf(stderr, "Invalid %s: expected start:duration:button[+button],...\n",
                     frame_script ? "DHD_AUTOPAD_FRAMES" : "DHD_AUTOPAD");
        exit(1);
    }
    race_sync_vsync(kernel_vsync_count);
    race_sync_pad(player, &out->buttons);
    if (player == 0) keys_script(out);
    static uint16_t previous;
    if (player == 0 && previous != out->buttons) {
        if (frame_script || nav)
            hle_log("autopad field=%llu game=%.3f wall=%.3f: %04X\n",
                    (unsigned long long)kernel_vsync_count, host_seconds(), host_wall_seconds(), out->buttons);
        else hle_log("autopad %.3f: %04X\n", host_seconds(), out->buttons);
        previous = out->buttons;
    }
    for (i = 0; i < 12; i++) out->pressure[i] = out->buttons & pressure_order[i] ? 255 : 0;
    return 1;
}

void host_pad_read(int port, HostPad *out)
{
    if (!host_pad_script(port, out))
        host_pad_device(port, out);
}

#ifdef _WIN32
/* Without SDL (DHD_SDL=0, or it didn't start): XInput pad `port`, else (port 0) a DualSense or
   DualShock 4 over raw HID. */
static void direct_pad(int port, HostPad *out)
{
    XInputGetStateFn get = xinput();
    XState x;
    size_t i;
    if (get && get((DWORD)port, &x) == ERROR_SUCCESS) {
        static const struct { WORD x; uint16_t button; } map[] = {
            {0x0001, PAD_UP}, {0x0002, PAD_DOWN}, {0x0004, PAD_LEFT}, {0x0008, PAD_RIGHT}, {0x0010, PAD_START},
            {0x0020, PAD_SELECT}, {0x0040, PAD_L3}, {0x0080, PAD_R3}, {0x0100, PAD_L1}, {0x0200, PAD_R1},
            {0x1000, PAD_CROSS}, {0x2000, PAD_CIRCLE}, {0x4000, PAD_SQUARE}, {0x8000, PAD_TRIANGLE},
        };
        for (i = 0; i < sizeof map / sizeof *map; i++)
            if (x.buttons & map[i].x)
                out->buttons |= map[i].button;
        if (x.left_trigger > 30)
            out->buttons |= PAD_L2;
        if (x.right_trigger > 30)
            out->buttons |= PAD_R2;
        out->left_x = stick(x.lx, 0), out->left_y = stick(x.ly, 1);
        out->right_x = stick(x.rx, 0), out->right_y = stick(x.ry, 1);
        for (i = 0; i < 12; i++)
            out->pressure[i] = pressure_order[i] == PAD_L2 ? x.left_trigger
                             : pressure_order[i] == PAD_R2 ? x.right_trigger
                             : out->buttons & pressure_order[i] ? 255 : 0;
        return;
    }
    SonyState sony;
    if (port == 0 && sony_read(&sony)) {
        /* d-pad: a hat, 0 up then clockwise, 8 none */
        static const uint16_t hat[9] = {PAD_UP, PAD_UP | PAD_RIGHT, PAD_RIGHT, PAD_DOWN | PAD_RIGHT, PAD_DOWN,
                                        PAD_DOWN | PAD_LEFT, PAD_LEFT, PAD_UP | PAD_LEFT, 0};
        static const struct { uint8_t bit; uint16_t button; } face[] = {
            {0x10, PAD_SQUARE}, {0x20, PAD_CROSS}, {0x40, PAD_CIRCLE}, {0x80, PAD_TRIANGLE}};
        /* Create/Share is SELECT, Options START */
        static const struct { uint8_t bit; uint16_t button; } shoulder[] = {
            {0x01, PAD_L1}, {0x02, PAD_R1}, {0x10, PAD_SELECT}, {0x20, PAD_START}, {0x40, PAD_L3}, {0x80, PAD_R3}};
        out->buttons = hat[(sony.face & 0x0F) > 8 ? 8 : sony.face & 0x0F];
        for (i = 0; i < sizeof face / sizeof *face; i++)
            if (sony.face & face[i].bit)
                out->buttons |= face[i].button;
        for (i = 0; i < sizeof shoulder / sizeof *shoulder; i++)
            if (sony.shoulder & shoulder[i].bit)
                out->buttons |= shoulder[i].button;
        if (sony.l2 > 30)
            out->buttons |= PAD_L2;
        if (sony.r2 > 30)
            out->buttons |= PAD_R2;
        out->left_x = sony.lx, out->left_y = sony.ly, out->right_x = sony.rx, out->right_y = sony.ry;
        for (i = 0; i < 12; i++)
            out->pressure[i] = pressure_order[i] == PAD_L2 ? sony.l2
                             : pressure_order[i] == PAD_R2 ? sony.r2
                             : out->buttons & pressure_order[i] ? 255 : 0;
    }
}

/* DHD_TEST_PAD=<name>: a test tool's buttons, one 16-bit word in shared memory of that name,
   merged into player 1's pad, so a tool can drive a window that doesn't have focus. */
static void test_pad_merge(HostPad *out)
{
    static volatile uint16_t *word;
    static int opened;
    if (!opened) {
        const char *name = getenv("DHD_TEST_PAD");
        opened = 1;
        HANDLE map = name && *name ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, name) : NULL;
        if (map) word = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 4);
    }
    if (word) out->buttons |= *word;
}

#else
/* The Mac reads every controller through SDL (gamepad.c). */
static void direct_pad(int port, HostPad *out) { (void)port; (void)out; }
static void test_pad_merge(HostPad *out) { (void)out; }
#endif

/* Player `port`'s controller (SDL, gamepad.c, else XInput and the Sony pads directly: one of the
   two, never both, so a pad isn't read twice), and for player 1 the keyboard as well, merged. */
void host_pad_device(int port, HostPad *out)
{
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    if (gamepad_start()) gamepad_read(port, out);
    else direct_pad(port, out);
    if (port == 0) keyboard_merge(out), test_pad_merge(out);
}

#ifdef _WIN32
void *host_fiber_main(void) { return ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH); }
void *host_fiber_create(void (*fn)(void *), void *arg)
{
    return CreateFiberEx(0, 0x40000, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)fn, arg);
}
/* The host stacks the EE threads run on (each fiber's, recorded as it yields). The game keeps
   pointers to stack locals in its own memory (a menu stack, D_003E3BD0), which on the PC are
   host addresses that depend on where Windows put the stacks; online.c's state hash leaves
   them out (finding 041). */
static uintptr_t stack_range[64][2];
static int stack_ranges;
static void note_stack(void *local)
{
    uintptr_t top = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    MEMORY_BASIC_INFORMATION m;
    for (int i = 0; i < stack_ranges; i++)
        if (stack_range[i][1] == top) return;
    if (stack_ranges < 64 && VirtualQuery(local, &m, sizeof m)) {
        stack_range[stack_ranges][0] = (uintptr_t)m.AllocationBase;
        stack_range[stack_ranges++][1] = top;
    }
}
int host_stack_ranges(uintptr_t (**ranges)[2])
{
    int here;
    note_stack(&here);   /* the running fiber may not have yielded yet */
    *ranges = stack_range;
    return stack_ranges;
}

void host_fiber_switch(void *fiber)
{
    /* Save the complete x86 state on this fiber's own stack. Windows' flag
       alone does not preserve live x87 register contents in our WOW64 fixture.
       Keep sticky faults too; do not clear a bad game ABI result at a yield. */
    unsigned char state[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave %0" : "=m"(state) : : "memory");
    note_stack(state);
    SwitchToFiber(fiber);
    __asm__ volatile("fxrstor %0" : : "m"(state) : "memory");
}
void host_fiber_delete(void *fiber) { DeleteFiber(fiber); }
void *host_fiber_current(void) { return GetCurrentFiber(); }

double host_wall_seconds(void)
{
    static LARGE_INTEGER start, freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

double host_seconds(void)
{
    return ee_frame_clock_enabled() ? (double)ee_frame_clock_cycles() / 147456000.0 : host_wall_seconds();
}

void host_sleep_ms(unsigned ms) { Sleep(ms); }

/* Until a QueryPerformanceCounter time: a high-resolution waitable timer (Windows 10 1803+; one
   per thread, the EE and the presenter both wait) sleeps to a millisecond before it (it woke up
   to 0.85 ms late, finding 052), then a pause-instruction spin. Without that timer, Sleep at 1 ms
   timer resolution to 2 ms before. */
void host_wait_wall(double seconds)
{
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    static volatile LONG coarse;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
        if (!timer && !InterlockedExchange(&coarse, 1)) {
            HMODULE winmm = LoadLibraryA("winmm.dll");
            FARPROC begin = winmm ? GetProcAddress(winmm, "timeBeginPeriod") : NULL;
            if (begin) ((UINT (WINAPI *)(UINT))(void *)begin)(1);
        }
    }
    double left = seconds - host_wall_seconds();
    if (timer && left > 0.001) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((left - 0.001) * 1e7);  /* relative, 100 ns units */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
    } else if (!timer && left > 0.002)
        Sleep((DWORD)((left - 0.002) * 1000));
    while (host_wall_seconds() < seconds)
        YieldProcessor();
}

#endif

/* The startup movies (func_001EBFD8: the logos, then the attract movie) play unless dhd.ini's
   skip_movies (DHD_SKIP_MOVIES, PC SETTINGS > Skip Movies) is nonzero; movies play natively
   (port/hle/movie.c, finding 055). The race, gate and bench tools set DHD_SKIP_MOVIES=1, so their
   runs stay as they were. OPTIONS > VIDEO CLIPS always plays the clip the player chose. */
int host_skip_movies(void)
{
    static int logged;
    const char *skip = getenv("DHD_SKIP_MOVIES");
    if (!skip || !atoi(skip)) return 0;
    if (!logged++) hle_log("DHD_SKIP_MOVIES: bypassing startup movie sequence\n");
    return 1;
}

/* PC settings: dhd.ini next to dhd.exe (or the file DHD_SETTINGS names), one "key = value" a
   line, [sections] and ;/# comments ignored. Each setting becomes the environment variable it
   stands for, unless that variable is already set: the environment overrides the file. A line
   may also name a DHD_* variable directly. Options are host-side only; none reaches game state. */
static void setting(const char *name, const char *value)
{
    if (!getenv(name)) _putenv_s(name, value);
}

static void apply_setting(const char *key, const char *value)
{
    if (!_stricmp(key, "game_options")) return; /* read after the game's own boot/load */
    static const struct { const char *key, *name; } plain[] = {
        {"iso", "DHD_ISO"}, {"skip_movies", "DHD_SKIP_MOVIES"}, {"window", "DHD_WINDOW"},
        {"width", "DHD_WINDOW_WIDTH"}, {"height", "DHD_WINDOW_HEIGHT"}, {"monitor", "DHD_MONITOR"},
        {"scale", "DHD_SCALE"}, {"widescreen", "DHD_WIDESCREEN"}, {"msaa", "DHD_MSAA"}, {"fxaa", "DHD_FXAA"},
        {"filter", "DHD_FILTER"}, {"vibration", "DHD_VIBRATION"}, {"remap", "DHD_REMAP"}, {"memory_card", "DHD_MEMCARD"},
        {"sound_latency_ms", "DHD_SND_LATENCY_MS"},
        {"server", "DHD_NET_SERVER"}, {"name", "DHD_NET_NAME"}, {"rider", "DHD_NET_RIDER"},  /* online (port/online) */
        {"update_url", "DHD_UPDATE_URL"}, {"auto_update", "DHD_AUTO_UPDATE"},  /* the updater (update.c) */
        {"texture_pack", "DHD_TEXTURE_PACK"}, {"textures", "DHD_TEXTURES"},  /* texture packs (port/gs/texpack.c) */
        {"texture_cache_mb", "DHD_TEXTURE_CACHE_MB"},
        {"font", "DHD_FONT"},  /* the sharp font (port/gs/font_sharp.c) */
        {"fps", "DHD_FPS"},
    };
    if (!_strnicmp(key, "DHD_", 4)) { setting(key, value); return; }
    if (!_stricmp(key, "renderer")) { setting("DHD_RENDER", _stricmp(value, "native") ? "exact" : "native"); return; }
    /* fps selects real offline simulation steps (finding 067). */
    for (unsigned i = 0; i < sizeof plain / sizeof *plain; i++)
        if (!_stricmp(key, plain[i].key)) { setting(plain[i].name, value); return; }
    hle_log("settings: unknown key %s\n", key);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = 0;
    return s;
}

/* dhd.ini's path: DHD_SETTINGS, else beside dhd.exe. */
static void settings_path(char *path, size_t size)
{
    const char *named = getenv("DHD_SETTINGS");
    if (named) { snprintf(path, size, "%s", named); return; }
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, size - n, "dhd.ini");
}

/* A value in double quotes is taken as it is (a path may hold ; or #), else up to a comment. */
static char *setting_value(char *value)
{
    if (*value == '"') {
        char *end = strchr(value + 1, '"');
        if (end) { *end = 0; return value + 1; }
    }
    char *comment = strpbrk(value, ";#");
    if (comment && (comment == value || comment[-1] == ' ' || comment[-1] == '\t')) { *comment = 0; value = trim(value); }
    return value;
}

static int iso_from_environment;  /* DHD_ISO was set before dhd.ini was read: it overrides the file */

/* The PC settings the environment sets (so dhd.ini can't), for the settings page to show them
   locked. The first process looks before reading dhd.ini; the relaunched one, whose environment
   already holds dhd.ini's values, is told in DHD_ENV_OVERRIDES. */
static void note_overrides(void)
{
    static const char *const names[] = {"DHD_SCALE", "DHD_WINDOW", "DHD_WINDOW_WIDTH", "DHD_WINDOW_HEIGHT",
                                        "DHD_ASPECT", "DHD_PRESENT_FILTER", "DHD_WIDESCREEN", "DHD_SKIP_MOVIES",
                                        "DHD_VIBRATION", "DHD_RENDER", "DHD_TEXTURES", "DHD_FONT"};
    char list[256] = ",";
    if (getenv("DHD_RELAUNCHED")) return;
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
        if (getenv(names[i]) && strlen(list) + strlen(names[i]) + 2 < sizeof list)
            strcat(strcat(list, names[i]), ",");
    _putenv_s("DHD_ENV_OVERRIDES", list);
}

int host_setting_locked(const char *name)
{
    char key[64];
    const char *list = getenv("DHD_ENV_OVERRIDES");
    snprintf(key, sizeof key, ",%s,", name);
    return list && strstr(list, key) != NULL;
}

/* The official public race server (online/server, deployed by the project). DHD_NET_SERVER or
   a "server =" line with an address replaces it, for anyone running their own. */
#define OFFICIAL_SERVER "wss://dhd.skuza.app"

static int settings_has(const char *key);

/* A player's settings where dhd.ini doesn't say (round 40, finding 056): the native renderer at
   2x, borderless on the monitor at its own resolution, widescreen in the monitor's shape with
   the HUD kept in shape, the startup movies played (skip_movies 0, finding 055). Only
   with a dhd.ini (the game writes one on the first start; the release ships none): the tools
   run without one and keep the old defaults, and a line in dhd.ini or a DHD_* variable wins. */
static const char *const player_defaults[][3] = {
    {"renderer", "native", "DHD_RENDER"}, {"scale", "2", "DHD_SCALE"}, {"window", "borderless", "DHD_WINDOW"},
    {"widescreen", "1", "DHD_WIDESCREEN"}, {"DHD_ASPECT", "wide", "DHD_ASPECT"}, {"skip_movies", "0", "DHD_SKIP_MOVIES"},
};

void host_settings_load(void)
{
    char path[MAX_PATH];
    iso_from_environment = getenv("DHD_ISO") != NULL;
    note_overrides();
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    char line[1024];
    while (f && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        char *key = trim(s), *value = setting_value(trim(eq + 1));
        if (*key && *value) apply_setting(key, value);
    }
    if (f) {
        fclose(f);
        for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
            if (!settings_has(player_defaults[i][0])) setting(player_defaults[i][2], player_defaults[i][1]);
    }
    setting("DHD_NET_SERVER", OFFICIAL_SERVER);  /* no server set (an empty "server =" sets none) */
}

/* Whether dhd.ini has a line for key. */
static int settings_has(const char *key)
{
    char path[MAX_PATH], line[1024];
    int found = 0;
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && !found && fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!*s || *s == ';' || *s == '#' || *s == '[' || !eq) continue;
        *eq = 0;
        found = !_stricmp(trim(s), key);
    }
    if (f) fclose(f);
    return found;
}

/* What a new dhd.ini starts with. The release ships none (port/tools/package_release.py), so
   unzipping a new version over an old folder never replaces the player's: the game writes this
   the first time it saves a setting (the disc image, on the first start). */
static const char settings_template[] =
    "; Downhill Revived settings: one \"key = value\" per line. The game adds your disc image (iso)\n"
    "; on the first start, and OPTIONS > PC SETTINGS writes the display settings here. DHD_*\n"
    "; environment variables override them.\n"
    "\n"
    "; scale: the 3D picture's resolution, 1-4 times the PlayStation 2's, or window.\n"
    "; window: windowed (width x height), borderless or fullscreen (the monitor's own resolution).\n"
    "; widescreen: 1 the monitor's shape (a wider view, the HUD and menus kept in shape), 0 off,\n"
    ";   or a shape such as 21:9. DHD_ASPECT, the picture's shape: wide (widescreen's), 4:3, 16:9,\n"
    ";   stretch or pixels.\n"
    "renderer = native\n"
    "scale = 2\n"
    "window = borderless\n"
    "widescreen = 1\n"
    "DHD_ASPECT = wide\n"
    "skip_movies = 0\n"
    "; Offline real simulation rate: 60 (original), 120, 144 or monitor (max 144).\n"
    "; Online always uses 60. Frame rate changes apply at the next start.\n"
    "fps = 60\n"
    "\n"
    "; Online races use the official server, wss://dhd.skuza.app, when\n"
    "; server is empty. To use a server of your own, put its address here, for example\n"
    ";   server = wss://races.example.org\n"
    "server =\n"
    "\n"
    "; At start the game looks for a newer release and offers it; online races need the newest.\n"
    "; auto_update = ask (asks first), on (updates without asking) or off (never looks).\n"
    "; update_url: where to look, a latest.json or a GitHub releases API address; empty means\n"
    "; the official releases. An update replaces dhd.exe and the text files, never dhd.ini or saves.\n"
    "update_url =\n"
    "auto_update = ask\n"
    "\n"
    "; A texture pack (upscale mod) made for PCSX2 works unchanged: texture_pack is its folder of\n"
    "; PNGs, the full path of PCSX2's textures\\SCUS-97177\\replacements folder for example (in\n"
    "; Documents\\PCSX2). When empty, a textures\\SCUS-97177\\replacements folder beside dhd.exe is\n"
    "; used if there is one. textures = off draws the game's own textures (PC SETTINGS > Texture\n"
    "; Pack).\n"
    "texture_pack =\n"
    "textures = on\n"
    "\n"
    "; font: sharp draws the game's font with clean edges at any resolution (made in memory from\n"
    "; the font on your disc each time the game starts); original draws it as the PS2 does.\n"
    "font = sharp\n";

/* Writes key = value into dhd.ini: replaces the key's first line, else adds one at the end; every
   other line (comments, sections, unknown keys) stays as it was. A missing dhd.ini starts as
   settings_template. Nonzero when saved. */
static int settings_save(const char *key, const char *value)
{
    char path[MAX_PATH], temp[MAX_PATH + 8], line[1024];
    settings_path(path, sizeof path);
    snprintf(temp, sizeof temp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(temp, "w");
    if (!out) { if (in) fclose(in); return 0; }
    const char *quote = strpbrk(value, ";#\"") || value[0] == ' ' ? "\"" : "";
    int written = 0;
    if (!in) fputs(settings_template, out);
    while (in && fgets(line, sizeof line, in)) {
        char copy[1024], *s, *eq;
        snprintf(copy, sizeof copy, "%s", line);
        s = trim(copy);
        if (!written && (eq = strchr(s, '=')) && *s != ';' && *s != '#' && *s != '[') {
            *eq = 0;
            if (!_stricmp(trim(s), key)) { fprintf(out, "%s = %s%s%s\n", key, quote, value, quote); written = 1; continue; }
        }
        fputs(line, out);
        if (!strchr(line, '\n')) fputc('\n', out);
    }
    if (!written) fprintf(out, "%s = %s%s%s\n", key, quote, value, quote);
    if (in) fclose(in);
    int ok = !ferror(out);
    ok &= fclose(out) == 0;
    if (ok) ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(temp);
    return ok;
}

int host_setting_save(const char *key, const char *value) { return settings_save(key, value); }

/* Read afresh after each game-data load; these values never become DHD_* overrides. */
int host_game_options_read(char *value, size_t size)
{
    char path[MAX_PATH], line[1024];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq = strchr(s, '=');
        if (!eq || *s == ';' || *s == '#' || *s == '[') continue;
        *eq = 0;
        if (!_stricmp(trim(s), "game_options")) {
            snprintf(value, size, "%s", setting_value(trim(eq + 1)));
            found = 1; break;
        }
    }
    fclose(f);
    return found;
}

/* ---- the disc image (first start) ---- */

static uint32_t le32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Whether path is Downhill Domination (USA): the ISO 9660 root holds a SYSTEM.CNF whose boot
   line names SCUS_971.77, and SCUS_971.77 is the retail one (game_data_read: the game's data
   comes from it). NULL if it is, else why not. */
static const char *disc_problem(const char *path, char *why, size_t size)
{
    enum { SECTOR = 2048 };
    static uint8_t sector[SECTOR];
    static char text[SECTOR + 1];
    FILE *f = fopen(path, "rb");
    if (!f) return "The file can't be opened.";
    const char *problem = NULL;
    uint32_t cnf = 0, cnf_size = 0;
#define READ(lsn) (_fseeki64(f, (long long)(lsn) * SECTOR, SEEK_SET) == 0 && fread(sector, SECTOR, 1, f) == 1)
    if (!READ(16) || sector[0] != 1 || memcmp(sector + 1, "CD001", 5)) {
        problem = "It isn't a disc image (no ISO 9660 volume in it).";
        goto done;
    }
    /* The primary volume descriptor's root directory record (at 156): extent at +2, size at +10. */
    uint32_t dir = le32_at(sector + 156 + 2), dir_size = le32_at(sector + 156 + 10);
    for (uint32_t off = 0; off < dir_size && off < 16 * SECTOR && !cnf; off += SECTOR) {
        if (!READ(dir + off / SECTOR)) break;
        for (unsigned i = 0; i + 33 < SECTOR && sector[i]; i += sector[i]) {
            const uint8_t *r = sector + i;
            if (r[32] >= 10 && i + 33 + r[32] <= SECTOR && !_strnicmp((const char *)r + 33, "SYSTEM.CNF", 10) &&
                (r[32] == 10 || r[33 + 10] == ';')) {
                cnf = le32_at(r + 2);
                cnf_size = le32_at(r + 10);
                break;
            }
        }
    }
    if (!cnf || !READ(cnf)) {
        problem = "It has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
        goto done;
    }
    memcpy(text, sector, SECTOR);
    text[cnf_size < SECTOR ? cnf_size : SECTOR] = 0;
    if (strstr(text, "SCUS_971.77")) {
        problem = game_data_read(path, NULL);  /* the game's program and data, unchanged (game_data.c) */
        goto done;
    }
    /* Name the game it is: BOOT2 = cdrom0:\SLUS_123.45;1 */
    const char *boot = strstr(text, "BOOT2");
    char id[32] = "";
    if (boot && (boot = strpbrk(boot, "\\:"))) {
        while (*boot == '\\' || *boot == ':') boot++;
        size_t n = strcspn(boot, ";\r\n");
        snprintf(id, sizeof id, "%.*s", (int)(n < 31 ? n : 31), boot);
    }
    snprintf(why, size, "It is another PlayStation 2 disc%s%s%s, not Downhill Domination (USA), SCUS-97177.",
             *id ? " (" : "", id, *id ? ")" : "");
    problem = why;
done:
#undef READ
    fclose(f);
    return problem;
}

#ifdef _WIN32
typedef BOOL(WINAPI *GetOpenFileNameFn)(OPENFILENAMEA *);

/* The file-open dialog for a disc image (comdlg32 loaded here: no new import); 0 if cancelled. */
static int choose_disc(char *path, size_t size)
{
    static GetOpenFileNameFn open_file;
    if (!open_file) {
        HMODULE dll = LoadLibraryA("comdlg32.dll");
        if (dll) open_file = (GetOpenFileNameFn)(void *)GetProcAddress(dll, "GetOpenFileNameA");
        if (!open_file) return 0;
    }
    OPENFILENAMEA o = {sizeof o};
    char folder[MAX_PATH];  /* dhd.exe's: players often keep the disc image beside the game */
    DWORD n = GetModuleFileNameA(NULL, folder, sizeof folder);
    while (n && folder[n - 1] != '\\' && folder[n - 1] != '/') n--;
    folder[n] = 0;
    o.lpstrInitialDir = folder;
    path[0] = 0;
    o.lpstrFilter = "Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    o.nFilterIndex = 1;
    o.lpstrFile = path; o.nMaxFile = (DWORD)size;
    o.lpstrTitle = "Choose your Downhill Domination (USA) disc image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return open_file(&o) && path[0];
}

#else
/* The open panel for a disc image; 0 if cancelled. */
static int choose_disc(char *path, size_t size)
{
    return mac_choose_file("Choose your Downhill Domination (USA) disc image", "iso", path, (uint32_t)size);
}
#endif

/* Before the relaunch (only the first of the two dhd.exe asks): the disc image dhd.ini names
   (iso), if it is there and is Downhill Domination (USA); else a message, a file-open dialog,
   the choice checked and saved to dhd.ini. DHD_ISO in the environment overrides the file and is
   never questioned (the tools set it). Returns 0 to go on, 1 if the player cancelled. */
int host_disc_ready(void)
{
    static const char title[] = "Downhill Revived";
    static char why[256], chosen[MAX_PATH], message[1024];
    if (iso_from_environment || getenv("DHD_HEADLESS")) return 0;
    const char *iso = getenv("DHD_ISO");
    const char *problem = iso ? disc_problem(iso, why, sizeof why) : NULL;
    if (iso && !problem) return 0;
    if (!iso)
        snprintf(message, sizeof message, "Choose your Downhill Domination (USA) disc image.\n\n"
                 "The game plays from your own copy of the disc, as an .iso file (SCUS-97177). "
                 "You choose it once; the game remembers it.");
    else if (GetFileAttributesA(iso) == INVALID_FILE_ATTRIBUTES)
        snprintf(message, sizeof message, "The disc image chosen before is gone:\n%s\n\n"
                 "Choose your Downhill Domination (USA) disc image.", iso);
    else
        snprintf(message, sizeof message, "The disc image chosen before can't be used:\n%s\n%s\n\n"
                 "Choose your Downhill Domination (USA) disc image.", iso, problem);
    if (MessageBoxA(NULL, message, title, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK) return 1;
    for (;;) {
        if (!choose_disc(chosen, sizeof chosen)) return 1;
        if (!(problem = disc_problem(chosen, why, sizeof why))) break;
        snprintf(message, sizeof message, "%s\n\n%s\n\nChoose your Downhill Domination (USA) disc image (SCUS-97177).",
                 chosen, problem);
        if (MessageBoxA(NULL, message, title, MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) != IDRETRY) return 1;
    }
    _putenv_s("DHD_ISO", chosen);
    hle_log("settings: disc image %s\n", chosen);
    /* The player defaults where dhd.ini doesn't say (a new dhd.ini has them from its template;
       without dhd.ini the tools' defaults stay, the exact renderer among them). This start uses
       them too. */
    int saved = settings_save("iso", chosen);
    for (unsigned i = 0; i < sizeof player_defaults / sizeof *player_defaults; i++)
        if (!settings_has(player_defaults[i][0])) {
            setting(player_defaults[i][2], player_defaults[i][1]);
            if (saved) settings_save(player_defaults[i][0], player_defaults[i][1]);
        }
    if (!saved) {
        char path[MAX_PATH];
        settings_path(path, sizeof path);
        snprintf(message, sizeof message, "The disc image can't be saved in\n%s\n\n"
                 "The game starts with it now, but will ask again next time.", path);
        MessageBoxA(NULL, message, title, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    return 0;
}

/* Widescreen: port/hle/widescreen.c. */
