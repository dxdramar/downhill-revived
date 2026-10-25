/* The WASI C library's allocator for the Mac build (finding 075) takes no lock: the module runs
   on several host threads (the graphics workers, sound, the network) that allocate as the game
   does. Each allocator call is made under one host lock instead (link_mac.py --wrap). */
#include <stddef.h>
#include "mac_host.h"

void *__real_malloc(size_t size);
void __real_free(void *p);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *p, size_t size);
void *__real_aligned_alloc(size_t alignment, size_t size);
int __real_posix_memalign(void **out, size_t alignment, size_t size);

static uint32_t lock;

static void take(void)
{
    uint32_t have = __atomic_load_n(&lock, __ATOMIC_ACQUIRE);
    if (!have) {
        uint32_t made = mac_lock_new(), expected = 0;
        if (__atomic_compare_exchange_n(&lock, &expected, made, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) have = made;
        else mac_lock_free(made), have = expected;
    }
    mac_lock(have);
}

static void give(void) { mac_unlock(lock); }

/* DHD_NO_FREE (diagnostics): nothing is ever freed and realloc always moves, so a stale pointer
   can't write into memory given out again. */
#include <stdlib.h>
#include <string.h>
size_t malloc_usable_size(void *p);
static int keep_all = -1;
static int no_free(void)
{
    if (keep_all < 0) keep_all = getenv("DHD_NO_FREE") != NULL;
    return keep_all;
}

void *__wrap_malloc(size_t size) { take(); void *p = __real_malloc(size); give(); return p; }
void __wrap_free(void *p)
{
    if (no_free()) return;
    take(); __real_free(p); give();
}
void *__wrap_calloc(size_t count, size_t size) { take(); void *p = __real_calloc(count, size); give(); return p; }
void *__wrap_realloc(void *p, size_t size)
{
    if (p && no_free()) {
        take();
        void *q = __real_malloc(size);
        size_t had = malloc_usable_size(p);
        if (q) memcpy(q, p, had < size ? had : size);
        give();
        return q;
    }
    take(); void *q = __real_realloc(p, size); give(); return q;
}
void *__wrap_aligned_alloc(size_t alignment, size_t size)
{
    take(); void *p = __real_aligned_alloc(alignment, size); give(); return p;
}
int __wrap_posix_memalign(void **out, size_t alignment, size_t size)
{
    take(); int r = __real_posix_memalign(out, alignment, size); give(); return r;
}
