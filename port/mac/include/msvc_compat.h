/* Mac build (finding 075): the Microsoft C library names the port uses, as the WASI C
   library's. Included first in every port unit on the Mac. */
#ifndef DHD_MSVC_COMPAT_H
#define DHD_MSVC_COMPAT_H
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define strnicmp strncasecmp
#define _strdup strdup
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _unlink unlink
#define _access access
#define __declspec(x) __declspec_##x
#define __declspec_thread _Thread_local
#define __declspec_noinline __attribute__((noinline))
#define __declspec_noreturn __attribute__((noreturn))
#define __declspec_dllexport
#define __declspec_selectany __attribute__((weak))
#define __declspec_align(n) __attribute__((aligned(n)))
/* wasm has no return addresses to give: the port only prints them in diagnostics. */
#define __builtin_return_address(level) ((void *)0)

static inline int _putenv_s(const char *name, const char *value) { return setenv(name, value, 1); }
static inline int _putenv(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    char name[256];
    if (!eq || (size_t)(eq - assignment) >= sizeof name) return -1;
    memcpy(name, assignment, eq - assignment);
    name[eq - assignment] = 0;
    return eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
}
#endif
