#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <execinfo.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exports_gen.h>
#include "bridge.h"
#include "core_host.h"
#include "memory_layout.h"
#include "wasm-rt-impl.h"
static w2c_core core;
#ifndef HOST64_FAKE_MODULE
/* WASM_RT_TRAP_HANDLER (build_host64.py): a trap can come from any EE fiber,
   whose native stack is not main's, so it cannot longjmp back. Report the
   trap and end the process with 0x7700 + the trap code. */
void host64_trap(wasm_rt_trap_t code) {
#ifdef _WIN32
    void *frames[48]; char text[160]; DWORD written;
    USHORT n=RtlCaptureStackBackTrace(1,48,frames,NULL);
    HANDLE err=GetStdHandle(STD_ERROR_HANDLE);
    uintptr_t base=(uintptr_t)GetModuleHandleW(NULL);
    int k=snprintf(text,sizeof text,"host64 trap: %s (EE fiber %d, wasm depth %u)\n",
                   wasm_rt_strerror(code),host64_fiber_id(),wasm_rt_call_stack_depth);
    WriteFile(err,text,(DWORD)k,&written,NULL);
    for (USHORT i=0;i<n;i++) {
        k=snprintf(text,sizeof text,"  at rva 0x%llx\n",(unsigned long long)((uintptr_t)frames[i]-base));
        WriteFile(err,text,(DWORD)k,&written,NULL);
    }
    TerminateProcess(GetCurrentProcess(),0x7700u+(UINT)code);
#else
    void *frames[48]; int n=backtrace(frames,48);
    dprintf(STDERR_FILENO,"host64 trap: %s (EE fiber %d, wasm depth %u)\n",
            wasm_rt_strerror(code),host64_fiber_id(),wasm_rt_call_stack_depth);
    backtrace_symbols_fd(frames,n,STDERR_FILENO);
    _exit(0x7700u+(unsigned)code);
#endif
    for (;;) {}
}
#endif
static int run(int argc,char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#endif
    struct w2c_env env;
    wasm_rt_init(); host64_exports_bind(&env,&core);
    wasm2c_core_instantiate(&core,&env);
    host64_bridge_bind(w2c_core_memory(&core));
    host64_fiber_bind(w2c_core_0x5F_stack_pointer(&core));
    if (!host64_memory_commit_stack(*w2c_core_0x5F_stack_pointer(&core))) {
        fprintf(stderr,"host64: module shadow stack at 0x%08x is neither below the low RAM view"
                " nor above 0x%llx\n",*w2c_core_0x5F_stack_pointer(&core),HOST64_MODULE_BASE);
        return 3;
    }
    extern int hle_verbose;
    hle_verbose=getenv("DHD_LOG")!=NULL;
#ifdef HOST64_FAKE_MODULE
    if (argc==3 && !strcmp(argv[1],"--negative")) {
        unsigned test=(unsigned)strtoul(argv[2],NULL,0);
        WASM_RT_SAVE_STACK_DEPTH();
        wasm_rt_trap_t trap=WASM_RT_SETJMP(g_wasm_rt_jmp_buf);
        if (!trap) {
            (void)env.exports.w2c_core_boundary_negative(&core,test);
            fprintf(stderr,"FAIL: boundary negative %u did not trap\n",test); return 1;
        }
        wasm_rt_trap_t expected=(test==1 || test==5 || test==6 || test==8 || test==9 || test==10 || test==11) ? WASM_RT_TRAP_OOB : WASM_RT_TRAP_CALL_INDIRECT;
        if (trap!=expected) { fprintf(stderr,"FAIL: wrong boundary trap %d\n",trap); return 1; }
        printf("PASS: boundary negative %u caught %s\n",test,wasm_rt_strerror(trap));
        wasm2c_core_free(&core); wasm_rt_free(); return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    int code=host64_module_main(&env);
    if (host_fiber_current()) host64_fiber_shutdown();
    wasm2c_core_free(&core); wasm_rt_free();
    printf("host64 module exit: %d\n",code);
    return code;
}

#ifdef DHD_MAIN_THREAD_SDL
/* macOS (build_host64_macos.py): Cocoa's windows and their events belong to the main thread, so
   the main thread is SDL's (sdl_host.c's sdl_host_main) and the game runs on a thread of its own,
   with a main thread's stack. The process ends with the game's code, from that thread. */
#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/stat.h>
void sdl_host_main(void);

/* An app bundle is no place to write: from a .app the game keeps dhd.ini, saves/ and dhd.log in
   ~/Library/Application Support/Downhill Revived (DHD_SETTINGS and DHD_MEMCARD_DIR, unless set),
   starting from the bundle's dhd.ini template (port/tools/package_mac.py). A plain dhd64 keeps
   them beside itself, as on Windows. */
static void app_folder(void) {
    char exe[PATH_MAX], real[PATH_MAX], folder[PATH_MAX], path[PATH_MAX];
    uint32_t size=sizeof exe;
    if (_NSGetExecutablePath(exe,&size) || !realpath(exe,real)) return;
    char *bundle=strstr(real,".app/Contents/MacOS/");
    const char *home=getenv("HOME");
    if (!bundle || !home) return;
    snprintf(folder,sizeof folder,"%s/Library/Application Support/Downhill Revived",home);
    mkdir(folder,0755);
    snprintf(path,sizeof path,"%s/dhd.ini",folder);
    if (access(path,F_OK)) {
        char template[PATH_MAX];
        snprintf(template,sizeof template,"%.*s.app/Contents/Resources/dhd.ini",(int)(bundle-real),real);
        FILE *in=fopen(template,"rb"), *out=in ? fopen(path,"wb") : NULL;
        char buffer[4096]; size_t n;
        while (out && (n=fread(buffer,1,sizeof buffer,in))>0) fwrite(buffer,1,n,out);
        if (in) fclose(in);
        if (out) fclose(out);
    }
    setenv("DHD_SETTINGS",path,0);
    snprintf(path,sizeof path,"%s/saves",folder);
    setenv("DHD_MEMCARD_DIR",path,0);
    if (!isatty(STDERR_FILENO)) {  /* started from Finder: the log in the folder */
        snprintf(path,sizeof path,"%s/dhd.log",folder);
        if (freopen(path,"w",stderr)) freopen(path,"a",stdout);
    }
}
static int saved_argc;
static char **saved_argv;
static void *game_thread(void *unused) {
    (void)unused;
    int code=run(saved_argc,saved_argv);
    fflush(NULL);
    _exit(code);
}
int main(int argc,char **argv) {
    pthread_attr_t a; pthread_t t;
    saved_argc=argc; saved_argv=argv;
    app_folder();
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a,64u<<20);
    pthread_attr_set_qos_class_np(&a,QOS_CLASS_USER_INTERACTIVE,0);
    if (pthread_create(&t,&a,game_thread,NULL)) { perror("host64: game thread"); return 1; }
    sdl_host_main();  /* never returns */
    return 0;
}
#else
int main(int argc,char **argv) { return run(argc,argv); }
#endif
