/* The module's memory, which is the PS2 address space (finding 075, port/mac/memory_mac.c).

   8 GB of address space are reserved: the module's 4 GB, read-write and zero, and 4 GB of
   guard after it, so that wasm2c needs no bounds checks (WASM_RT_MEMCHECK_GUARD_PAGES: a 32-bit
   address plus a 32-bit offset stays inside). The 32 MB of PS2 RAM at 0 are a shared memory
   object, mapped again at 0x20000000 and 0x30000000, where the game's uncached (address |
   0x20000000) and uncached-accelerated (| 0x30000000) pointers land, as memory.c does on
   Windows. Pages are only made when touched. */
#include "native.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

enum { RAM_SIZE = 0x02000000 };
uint8_t *space;

static void map_ram(int fd, uint32_t address)
{
    if (mmap(space + address, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != space + address) {
        perror("dhd: mapping PS2 RAM");
        exit(1);
    }
}

void space_init(void)
{
    space = mmap(NULL, 0x200000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (space == MAP_FAILED || mprotect(space, 0x100000000ull, PROT_READ | PROT_WRITE)) {
        perror("dhd: reserving the module's memory");
        exit(1);
    }
    char name[64];
    snprintf(name, sizeof name, "/dhd-ram-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
    if (fd < 0 || ftruncate(fd, RAM_SIZE)) {
        perror("dhd: PS2 RAM");
        exit(1);
    }
    map_ram(fd, 0);
    map_ram(fd, 0x20000000);
    map_ram(fd, 0x30000000);
    close(fd);
}
