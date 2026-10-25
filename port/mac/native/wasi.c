/* The WASI calls the module's C library makes (wasi_snapshot_preview1), on macOS (finding 075).

   One preopened directory, fd 3, is the file system root, so the module opens files by their
   real absolute paths; relative paths are the app's folder's (the module changes its C
   library's working directory there first, port/mac/win32.c). Windows-style paths work: '\\' is
   '/'. File descriptors are the host's own, so stdin, stdout and stderr are 0-2. */
#include "native.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define WASI(name) w2c_wasi__snapshot__preview1_##name
typedef struct w2c_wasi__snapshot__preview1 Wasi;

enum { ROOT_FD = 3 };
extern char **environ;
int main_argc;
char **main_argv;

/* WASI errno values from the host's. */
static u32 err(int e)
{
    switch (e) {
    case 0: return 0;
    case E2BIG: return 1; case EACCES: return 2; case EBADF: return 8; case EBUSY: return 10;
    case EEXIST: return 20; case EFAULT: return 21; case EFBIG: return 22; case EINTR: return 27;
    case EINVAL: return 28; case EIO: return 29; case EISDIR: return 31; case ELOOP: return 32;
    case EMFILE: return 33; case ENAMETOOLONG: return 37; case ENOENT: return 44; case ENOMEM: return 48;
    case ENOSPC: return 51; case ENOSYS: return 52; case ENOTDIR: return 54; case ENOTEMPTY: return 55;
    case EPERM: return 63; case EPIPE: return 64; case EROFS: return 69; case ESPIPE: return 70;
    case EXDEV: return 75;
    default: return 29;
    }
}

static void put32(u32 address, uint32_t v) { memcpy(at(address), &v, 4); }
static void put64(u32 address, uint64_t v) { memcpy(at(address), &v, 8); }

/* A module path under the root, as a host path: the root prefix and '\\' as '/'. */
static int host_path(u32 fd, u32 path, u32 length, char *out, size_t size)
{
    if (fd != ROOT_FD || length + 2 > size) return 0;
    out[0] = '/';
    for (u32 i = 0; i < length; i++) {
        char c = string_at(path)[i];
        out[i + 1] = c == '\\' ? '/' : c;
    }
    out[length + 1] = 0;
    return 1;
}

/* ---- arguments and the environment ---- */

u32 WASI(args_sizes_get)(Wasi *w, u32 count, u32 bytes)
{
    (void)w;
    size_t n = 0;
    for (int i = 0; i < main_argc; i++) n += strlen(main_argv[i]) + 1;
    put32(count, (uint32_t)main_argc);
    put32(bytes, (uint32_t)n);
    return 0;
}

u32 WASI(args_get)(Wasi *w, u32 argv, u32 buffer)
{
    (void)w;
    for (int i = 0; i < main_argc; i++) {
        size_t n = strlen(main_argv[i]) + 1;
        memcpy(at(buffer), main_argv[i], n);
        put32(argv + 4 * (u32)i, buffer);
        buffer += (u32)n;
    }
    return 0;
}

u32 WASI(environ_sizes_get)(Wasi *w, u32 count, u32 bytes)
{
    (void)w;
    size_t c = 0, n = 0;
    for (char **e = environ; *e; e++) c++, n += strlen(*e) + 1;
    put32(count, (uint32_t)c);
    put32(bytes, (uint32_t)n);
    return 0;
}

u32 WASI(environ_get)(Wasi *w, u32 env, u32 buffer)
{
    (void)w;
    u32 k = 0;
    for (char **e = environ; *e; e++) {
        size_t n = strlen(*e) + 1;
        memcpy(at(buffer), *e, n);
        put32(env + 4 * k++, buffer);
        buffer += (u32)n;
    }
    return 0;
}

/* ---- clocks, randomness, the process ---- */

u32 WASI(clock_time_get)(Wasi *w, u32 id, u64 precision, u32 out)
{
    (void)w; (void)precision;
    struct timespec t;
    clock_gettime(id == 0 ? CLOCK_REALTIME : id == 1 ? CLOCK_MONOTONIC : CLOCK_PROCESS_CPUTIME_ID, &t);
    put64(out, (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec);
    return 0;
}

u32 WASI(clock_res_get)(Wasi *w, u32 id, u32 out)
{
    (void)w; (void)id;
    put64(out, 1000);
    return 0;
}

u32 WASI(random_get)(Wasi *w, u32 buffer, u32 length)
{
    (void)w;
    for (u32 done = 0; done < length;) {
        u32 n = length - done > 256 ? 256 : length - done;
        if (getentropy(at(buffer + done), n)) return err(errno);
        done += n;
    }
    return 0;
}

void WASI(proc_exit)(Wasi *w, u32 code)
{
    (void)w;
    fflush(NULL);
    _exit((int)code);
}

u32 WASI(sched_yield)(Wasi *w) { (void)w; sched_yield(); return 0; }

/* poll_oneoff as the C library uses it: sleeping on one clock subscription. */
u32 WASI(poll_oneoff)(Wasi *w, u32 in, u32 out, u32 count, u32 events)
{
    (void)w;
    uint64_t longest = 0;
    for (u32 i = 0; i < count; i++) {
        const uint8_t *s = at(in + 48 * i);
        if (s[8] == 0) {  /* clock: id at 16, timeout at 24, flags at 40 (1: absolute) */
            uint64_t timeout;
            memcpy(&timeout, s + 24, 8);
            if (!(s[40] & 1) && timeout > longest) longest = timeout;
        }
    }
    if (longest) {
        struct timespec t = {(time_t)(longest / 1000000000u), (long)(longest % 1000000000u)};
        while (nanosleep(&t, &t) && errno == EINTR) {}
    }
    for (u32 i = 0; i < count; i++) {
        uint8_t *e = at(out + 32 * i);
        memset(e, 0, 32);
        memcpy(e, (uint8_t *)at(in + 48 * i), 8);  /* userdata */
        e[10] = ((uint8_t *)at(in + 48 * i))[8];   /* type */
    }
    put32(events, count);
    return 0;
}

/* ---- descriptors ---- */

u32 WASI(fd_prestat_get)(Wasi *w, u32 fd, u32 out)
{
    (void)w;
    if (fd != ROOT_FD) return 8;
    put32(out, 0);      /* a directory */
    put32(out + 4, 1);  /* "/" */
    return 0;
}

u32 WASI(fd_prestat_dir_name)(Wasi *w, u32 fd, u32 out, u32 length)
{
    (void)w;
    if (fd != ROOT_FD || length < 1) return 8;
    *(char *)at(out) = '/';
    return 0;
}

static uint8_t file_type(mode_t m)
{
    return S_ISDIR(m) ? 3 : S_ISREG(m) ? 4 : S_ISCHR(m) ? 2 : S_ISBLK(m) ? 1 : S_ISLNK(m) ? 7 : 0;
}

u32 WASI(fd_fdstat_get)(Wasi *w, u32 fd, u32 out)
{
    (void)w;
    struct stat st;
    uint8_t *s = at(out);
    memset(s, 0, 24);
    if (fd == ROOT_FD) s[0] = 3;
    else if (fstat((int)fd, &st)) return err(errno);
    else s[0] = file_type(st.st_mode);
    int flags = fd == ROOT_FD ? 0 : fcntl((int)fd, F_GETFL);
    if (flags >= 0 && (flags & O_APPEND)) s[2] = 1;
    memset(s + 8, 0xFF, 16);  /* every right */
    return 0;
}

u32 WASI(fd_fdstat_set_flags)(Wasi *w, u32 fd, u32 flags)
{
    (void)w;
    int f = fcntl((int)fd, F_GETFL);
    if (f < 0) return err(errno);
    f = (flags & 1) ? f | O_APPEND : f & ~O_APPEND;
    f = (flags & 4) ? f | O_NONBLOCK : f & ~O_NONBLOCK;
    return fcntl((int)fd, F_SETFL, f) ? err(errno) : 0;
}

static void filestat(u32 out, const struct stat *st)
{
    uint8_t *s = at(out);
    memset(s, 0, 64);
    put64(out, (uint64_t)st->st_dev);
    put64(out + 8, (uint64_t)st->st_ino);
    s[16] = file_type(st->st_mode);
    put64(out + 24, (uint64_t)st->st_nlink);
    put64(out + 32, (uint64_t)st->st_size);
    put64(out + 40, (uint64_t)st->st_atimespec.tv_sec * 1000000000u + (uint64_t)st->st_atimespec.tv_nsec);
    put64(out + 48, (uint64_t)st->st_mtimespec.tv_sec * 1000000000u + (uint64_t)st->st_mtimespec.tv_nsec);
    put64(out + 56, (uint64_t)st->st_ctimespec.tv_sec * 1000000000u + (uint64_t)st->st_ctimespec.tv_nsec);
}

u32 WASI(fd_filestat_get)(Wasi *w, u32 fd, u32 out)
{
    (void)w;
    struct stat st;
    if (fstat(fd == ROOT_FD ? -1 : (int)fd, &st)) {
        if (fd != ROOT_FD || stat("/", &st)) return err(errno);
    }
    filestat(out, &st);
    return 0;
}

u32 WASI(fd_filestat_set_size)(Wasi *w, u32 fd, u64 size)
{
    (void)w;
    return ftruncate((int)fd, (off_t)size) ? err(errno) : 0;
}

/* iovecs: {address, length} pairs of u32 */
static int iovecs(u32 list, u32 count, struct iovec *v)
{
    if (count > 64) count = 64;
    for (u32 i = 0; i < count; i++) {
        uint32_t base, length;
        memcpy(&base, at(list + 8 * i), 4);
        memcpy(&length, at(list + 8 * i + 4), 4);
        v[i].iov_base = at(base);
        v[i].iov_len = length;
    }
    return (int)count;
}

u32 WASI(fd_read)(Wasi *w, u32 fd, u32 list, u32 count, u32 done)
{
    (void)w;
    struct iovec v[64];
    ssize_t n = readv((int)fd, v, iovecs(list, count, v));
    if (n < 0) return err(errno);
    put32(done, (uint32_t)n);
    return 0;
}

u32 WASI(fd_pread)(Wasi *w, u32 fd, u32 list, u32 count, u64 offset, u32 done)
{
    (void)w;
    struct iovec v[64];
    ssize_t n = preadv((int)fd, v, iovecs(list, count, v), (off_t)offset);
    if (n < 0) return err(errno);
    put32(done, (uint32_t)n);
    return 0;
}

u32 WASI(fd_write)(Wasi *w, u32 fd, u32 list, u32 count, u32 done)
{
    (void)w;
    struct iovec v[64];
    ssize_t n = writev((int)fd, v, iovecs(list, count, v));
    if (n < 0) return err(errno);
    put32(done, (uint32_t)n);
    return 0;
}

u32 WASI(fd_pwrite)(Wasi *w, u32 fd, u32 list, u32 count, u64 offset, u32 done)
{
    (void)w;
    struct iovec v[64];
    ssize_t n = pwritev((int)fd, v, iovecs(list, count, v), (off_t)offset);
    if (n < 0) return err(errno);
    put32(done, (uint32_t)n);
    return 0;
}

u32 WASI(fd_seek)(Wasi *w, u32 fd, u64 offset, u32 whence, u32 out)
{
    (void)w;
    off_t r = lseek((int)fd, (off_t)(int64_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) return err(errno);
    put64(out, (uint64_t)r);
    return 0;
}

u32 WASI(fd_tell)(Wasi *w, u32 fd, u32 out)
{
    (void)w;
    off_t r = lseek((int)fd, 0, SEEK_CUR);
    if (r < 0) return err(errno);
    put64(out, (uint64_t)r);
    return 0;
}

u32 WASI(fd_close)(Wasi *w, u32 fd)
{
    (void)w;
    if (fd == ROOT_FD) return 0;
    return close((int)fd) ? err(errno) : 0;
}

u32 WASI(fd_sync)(Wasi *w, u32 fd) { (void)w; return fsync((int)fd) ? err(errno) : 0; }
u32 WASI(fd_datasync)(Wasi *w, u32 fd) { (void)w; return fsync((int)fd) ? err(errno) : 0; }
u32 WASI(fd_advise)(Wasi *w, u32 fd, u64 offset, u64 length, u32 advice)
{
    (void)w; (void)fd; (void)offset; (void)length; (void)advice;
    return 0;
}

/* Directory entries: {next cookie u64, inode u64, name length u32, type u8} then the name. The
   cookie is the entry's index. */
u32 WASI(fd_readdir)(Wasi *w, u32 fd, u32 buffer, u32 length, u64 cookie, u32 used)
{
    (void)w;
    int copy = dup((int)fd);
    DIR *d = copy >= 0 ? fdopendir(copy) : NULL;
    if (!d) { if (copy >= 0) close(copy); return err(errno); }
    rewinddir(d);
    struct dirent *e;
    u32 n = 0;
    uint64_t index = 0;
    while ((e = readdir(d)) && n < length) {
        if (index++ < cookie) continue;
        uint8_t head[24];
        uint64_t next = index, ino = e->d_ino;
        uint32_t namelen = (uint32_t)strlen(e->d_name);
        memcpy(head, &next, 8);
        memcpy(head + 8, &ino, 8);
        memcpy(head + 16, &namelen, 4);
        head[20] = e->d_type == DT_DIR ? 3 : e->d_type == DT_REG ? 4 : e->d_type == DT_LNK ? 7 : 0;
        head[21] = head[22] = head[23] = 0;
        u32 k = length - n < 24 ? length - n : 24;
        memcpy(at(buffer + n), head, k);
        n += k;
        k = length - n < namelen ? length - n : namelen;
        memcpy(at(buffer + n), e->d_name, k);
        n += k;
    }
    closedir(d);
    put32(used, n);
    return 0;
}

/* ---- paths ---- */

u32 WASI(path_open)(Wasi *w, u32 dirfd, u32 lookup, u32 path, u32 length, u32 oflags, u64 base, u64 inherit,
                    u32 fdflags, u32 out)
{
    (void)w; (void)lookup; (void)inherit;
    char p[4096];
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    int write = (base & (1ull << 6)) != 0;  /* fd_write */
    int read = (base & (1ull << 1)) != 0;   /* fd_read */
    int flags = write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (oflags & 1) flags |= O_CREAT;
    if (oflags & 2) flags |= O_DIRECTORY;
    if (oflags & 4) flags |= O_EXCL;
    if (oflags & 8) flags |= O_TRUNC;
    if (fdflags & 1) flags |= O_APPEND;
    if (fdflags & 4) flags |= O_NONBLOCK;
    int fd = open(p, flags | O_CLOEXEC, 0644);
    if (fd < 0 && errno == EISDIR && !(oflags & 2)) fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return err(errno);
    if (fd == ROOT_FD) {  /* keep 3 the root's */
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 4);
        close(fd);
        fd = moved;
        if (fd < 0) return err(errno);
    }
    put32(out, (uint32_t)fd);
    return 0;
}

u32 WASI(path_filestat_get)(Wasi *w, u32 dirfd, u32 flags, u32 path, u32 length, u32 out)
{
    (void)w;
    char p[4096];
    struct stat st;
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    if ((flags & 1 ? stat(p, &st) : lstat(p, &st))) return err(errno);
    filestat(out, &st);
    return 0;
}

u32 WASI(path_create_directory)(Wasi *w, u32 dirfd, u32 path, u32 length)
{
    (void)w;
    char p[4096];
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    return mkdir(p, 0755) ? err(errno) : 0;
}

u32 WASI(path_remove_directory)(Wasi *w, u32 dirfd, u32 path, u32 length)
{
    (void)w;
    char p[4096];
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    return rmdir(p) ? err(errno) : 0;
}

u32 WASI(path_unlink_file)(Wasi *w, u32 dirfd, u32 path, u32 length)
{
    (void)w;
    char p[4096];
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    return unlink(p) ? err(errno) : 0;
}

u32 WASI(path_rename)(Wasi *w, u32 fd, u32 path, u32 length, u32 to_fd, u32 to_path, u32 to_length)
{
    (void)w;
    char a[4096], b[4096];
    if (!host_path(fd, path, length, a, sizeof a) || !host_path(to_fd, to_path, to_length, b, sizeof b)) return 8;
    return rename(a, b) ? err(errno) : 0;
}

u32 WASI(path_readlink)(Wasi *w, u32 dirfd, u32 path, u32 length, u32 buffer, u32 size, u32 used)
{
    (void)w;
    char p[4096];
    if (!host_path(dirfd, path, length, p, sizeof p)) return 8;
    ssize_t n = readlink(p, at(buffer), size);
    if (n < 0) return err(errno);
    put32(used, (uint32_t)n);
    return 0;
}
