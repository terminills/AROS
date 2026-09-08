/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso file ABI: Linux open flags and struct stat64 layout on top of
    posixc, plus the handful of /proc and /sys files the guest reads
    (synthesised into T: so the ordinary read/close path serves them).
*/

#include <proto/exec.h>
#include <proto/dos.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linuxso_intern.h"
#include "lso_lx.h"


/* Linux x86-64 open(2) flags */
#define LX_O_ACCMODE   03
#define LX_O_WRONLY    01
#define LX_O_RDWR      02
#define LX_O_CREAT     0100
#define LX_O_EXCL      0200
#define LX_O_TRUNC     01000
#define LX_O_APPEND    02000
#define LX_O_NONBLOCK  04000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW  0400000
#define LX_O_CLOEXEC   02000000

int lso_open_flags_to_aros(int lx)
{
    int f;

    switch (lx & LX_O_ACCMODE)
    {
    case LX_O_WRONLY: f = O_WRONLY; break;
    case LX_O_RDWR:   f = O_RDWR; break;
    default:          f = O_RDONLY; break;
    }
    if (lx & LX_O_CREAT)     f |= O_CREAT;
    if (lx & LX_O_EXCL)      f |= O_EXCL;
    if (lx & LX_O_TRUNC)     f |= O_TRUNC;
    if (lx & LX_O_APPEND)    f |= O_APPEND;
    if (lx & LX_O_NONBLOCK)  f |= O_NONBLOCK;
    if (lx & LX_O_DIRECTORY) f |= O_DIRECTORY;
    if (lx & LX_O_NOFOLLOW)  f |= O_NOFOLLOW;
    if (lx & LX_O_CLOEXEC)   f |= O_CLOEXEC;
    return f;
}

/* ---- synthetic /proc and /sys ------------------------------------------- */

static int synth_counter;

/*
 * Returns a T: path holding the content of a /proc or /sys file the guest
 * asked for, or NULL when the name is not one we serve.  buf receives the
 * path.
 */
static const char *synth_path(const char *path, char *buf, size_t buflen)
{
    char *content = NULL;
    size_t len = 0;
    BPTR fh;
    char small[64];

    if (strcmp(path, "/proc/self/maps") == 0)
    {
        content = AllocVec(8192, MEMF_ANY | MEMF_CLEAR);
        if (!content)
            return NULL;
        lso_procfs_maps(content, 8192);
        len = strlen(content);
    }
    else if (strcmp(path, "/sys/devices/system/cpu/present") == 0 ||
             strcmp(path, "/sys/devices/system/cpu/online") == 0 ||
             strcmp(path, "/sys/devices/system/cpu/possible") == 0)
    {
        unsigned int n = lso_ncpus();
        snprintf(small, sizeof(small), n > 1 ? "0-%u\n" : "0\n", n - 1);
        content = small;
        len = strlen(small);
    }
    else if (strcmp(path, "/proc/self/cmdline") == 0)
    {
        content = small;
        len = snprintf(small, sizeof(small), "chromium") + 1;
    }
    else
        return NULL;

    snprintf(buf, buflen, "T:linuxso.%p.%d", FindTask(NULL), __sync_add_and_fetch(&synth_counter, 1));
    fh = Open(buf, MODE_NEWFILE);
    if (fh)
    {
        Write(fh, content, len);
        Close(fh);
    }
    if (content != small)
        FreeVec(content);
    LSO_DEBUG("synthesised %s -> %s (%lu bytes)", path, buf, (unsigned long)len);
    return fh ? buf : NULL;
}

static int is_proc_or_sys(const char *path)
{
    return strncmp(path, "/proc/", 6) == 0 || strncmp(path, "/sys/", 5) == 0 ||
           strcmp(path, "/proc") == 0 || strcmp(path, "/sys") == 0 ||
           (strncmp(path, "/dev/", 5) == 0 && strcmp(path, "/dev/null") != 0);
}

/* ---- /dev/urandom ---------------------------------------------------------
 * A real fd number (NIL: underneath) whose reads are answered with random
 * bytes for as long as the guest keeps asking; BoringSSL aborts on a short
 * read, so a file snapshot would not do.
 */
#define LSO_MAX_RANDOM_FDS 8
static int random_fds[LSO_MAX_RANDOM_FDS];   /* fd + 1, 0 = free */

static int random_open(void)
{
    int fd = open("NIL:", O_RDONLY), i;

    if (fd < 0)
        return -1;
    Forbid();
    for (i = 0; i < LSO_MAX_RANDOM_FDS; i++)
        if (!random_fds[i])
        {
            random_fds[i] = fd + 1;
            break;
        }
    Permit();
    if (i == LSO_MAX_RANDOM_FDS)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int random_slot(int fd)
{
    int i;

    for (i = 0; i < LSO_MAX_RANDOM_FDS; i++)
        if (random_fds[i] == fd + 1)
            return i;
    return -1;
}

int lso_open(const char *path, int lxflags, int mode)
{
    char tmp[64];
    int fd;

    if (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0)
    {
        fd = random_open();
        LSO_DEBUG("open(%s) -> random fd %d", path, fd);
        if (fd < 0)
            lso_set_errno(LX_EMFILE);
        return fd;
    }
    if (is_proc_or_sys(path))
    {
        const char *p = synth_path(path, tmp, sizeof(tmp));
        if (!p)
        {
            LSO_DEBUG("open(%s) -> ENOENT (not synthesised)", path);
            lso_set_errno(LX_ENOENT);
            return -1;
        }
        fd = open(p, O_RDONLY);
    }
    else
        fd = open(path, lso_open_flags_to_aros(lxflags), mode);

    if (fd < 0)
    {
        LSO_DEBUG("open(%s, %#o) -> errno %d", path, lxflags, errno);
        lso_set_errno_from_aros();
        return -1;
    }
    LSO_DEBUG("open(%s, %#o) -> %d", path, lxflags, fd);
    return fd;
}

FILE *lso_fopen(const char *path, const char *fmode)
{
    char tmp[64];
    FILE *f;

    if (is_proc_or_sys(path))
    {
        const char *p = synth_path(path, tmp, sizeof(tmp));
        if (!p)
        {
            LSO_DEBUG("fopen(%s) -> ENOENT (not synthesised)", path);
            lso_set_errno(LX_ENOENT);
            return NULL;
        }
        f = fopen(p, "r");
    }
    else
        f = fopen(path, fmode);
    if (!f)
        lso_set_errno_from_aros();
    LSO_DEBUG("fopen(%s, %s) -> %p", path, fmode, f);
    return f;
}

/* ---- struct stat64 -------------------------------------------------------- */

struct lx_stat64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t lx_atime, lx_atime_nsec;   /* glibc's st_atime is a macro */
    int64_t lx_mtime, lx_mtime_nsec;
    int64_t lx_ctime, lx_ctime_nsec;
    int64_t reserved[3];
};

static void stat_convert(const struct stat *s, struct lx_stat64 *l)
{
    memset(l, 0, sizeof(*l));
    l->st_dev = s->st_dev;
    l->st_ino = s->st_ino;
    l->st_nlink = s->st_nlink;
    l->st_mode = s->st_mode;        /* S_IF* bits are the traditional octal values on both */
    l->st_uid = s->st_uid;
    l->st_gid = s->st_gid;
    l->st_rdev = s->st_rdev;
    l->st_size = s->st_size;
    l->st_blksize = s->st_blksize ? s->st_blksize : 4096;
    l->st_blocks = (s->st_size + 511) / 512;
    l->lx_atime = s->st_atime;
    l->lx_mtime = s->st_mtime;
    l->lx_ctime = s->st_ctime;
}

int lso_stat_to_glibc(const char *path, int fd, void *out)
{
    struct stat s;
    int r;

    if (path)
    {
        if (is_proc_or_sys(path))
        {
            lso_set_errno(LX_ENOENT);
            return -1;
        }
        r = stat(path, &s);
    }
    else
        r = fstat(fd, &s);
    if (r < 0)
    {
        LSO_DEBUG("stat(%s/%d) -> errno %d", path ? path : "-", fd, errno);
        lso_set_errno_from_aros();
        return -1;
    }
    stat_convert(&s, (struct lx_stat64 *)out);
    return 0;
}

int lx_xstat64(int ver, const char *path, void *buf)   { (void)ver; return lso_stat_to_glibc(path, -1, buf); }
int lx_lxstat64(int ver, const char *path, void *buf)  { (void)ver; return lso_stat_to_glibc(path, -1, buf); }
int lx_fxstat64(int ver, int fd, void *buf)            { (void)ver; return lso_stat_to_glibc(NULL, fd, buf); }
int lx_stat64(const char *path, void *buf)             { return lso_stat_to_glibc(path, -1, buf); }
int lx_fstat64(int fd, void *buf)                      { return lso_stat_to_glibc(NULL, fd, buf); }

/* ---- fd calls with errno translation ------------------------------------- */

int lx_open64(const char *path, int flags, ...)
{
    int mode = 0;

    if (flags & LX_O_CREAT)
    {
        __builtin_va_list ap;
        __builtin_va_start(ap, flags);
        mode = __builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }
    return lso_open(path, flags, mode);
}

int lx_creat64(const char *path, int mode)
{
    return lso_open(path, LX_O_WRONLY | LX_O_CREAT | LX_O_TRUNC, mode);
}

int lx_close(int fd)
{
    int r, slot = random_slot(fd);

    if (slot >= 0)
        random_fds[slot] = 0;
    r = close(fd);
    if (r < 0)
        lso_set_errno_from_aros();
    return r;
}

long lx_read(int fd, void *buf, size_t n)
{
    ssize_t r;

    if (random_slot(fd) >= 0)
        return lso_getrandom(buf, n);
    r = read(fd, buf, n);
    if (r < 0)
        lso_set_errno_from_aros();
    return r;
}

long lx_write(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    if (r < 0)
        lso_set_errno_from_aros();
    return r;
}

long lx_pread64(int fd, void *buf, size_t n, int64_t off)
{
    ssize_t r = pread(fd, buf, n, (off_t)off);
    if (r < 0)
        lso_set_errno_from_aros();
    return r;
}

int64_t lx_lseek64(int fd, int64_t off, int whence)
{
    off_t r = lseek(fd, (off_t)off, whence);
    if (r < 0)
        lso_set_errno_from_aros();
    return r;
}
