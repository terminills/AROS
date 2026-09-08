/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso internals shared by the ELF mapper and the glibc ABI shim.
*/

#ifndef LINUXSO_INTERN_H
#define LINUXSO_INTERN_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/tasks.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#include "linuxso.h"

/* Loader/shim trace: LINUXSO_DEBUG in the environment turns it on. */
void lso_log(const char *fmt, ...);
#define LSO_DEBUG(...) do { if (linuxso_debug_enabled()) lso_log(__VA_ARGS__); } while (0)

/* ---- ELF64 (only what the mapper needs) ---------------------------------- */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} lso_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} lso_Phdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} lso_Shdr;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} lso_Dyn;

typedef struct {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} lso_Sym;

typedef struct {
    uint64_t r_offset, r_info;
    int64_t r_addend;
} lso_Rela;

#define LSO_ET_DYN        3
#define LSO_EM_X86_64     62
#define LSO_PT_LOAD       1
#define LSO_PT_DYNAMIC    2
#define LSO_PT_TLS        7
#define LSO_PF_X          1
#define LSO_PF_W          2
#define LSO_PF_R          4
#define LSO_SHN_UNDEF     0
#define LSO_STB_WEAK      2
#define LSO_STT_TLS       6
#define LSO_STT_FUNC      2
#define LSO_STT_OBJECT    1
#define LSO_STT_GNU_IFUNC 10

#define LSO_DT_NULL       0
#define LSO_DT_NEEDED     1
#define LSO_DT_PLTRELSZ   2
#define LSO_DT_PLTGOT     3
#define LSO_DT_HASH       4
#define LSO_DT_STRTAB     5
#define LSO_DT_SYMTAB     6
#define LSO_DT_RELA       7
#define LSO_DT_RELASZ     8
#define LSO_DT_RELAENT    9
#define LSO_DT_INIT       12
#define LSO_DT_FINI       13
#define LSO_DT_SONAME     14
#define LSO_DT_JMPREL     23
#define LSO_DT_INIT_ARRAY 25
#define LSO_DT_FINI_ARRAY 26
#define LSO_DT_INIT_ARRAYSZ 27
#define LSO_DT_FINI_ARRAYSZ 28
#define LSO_DT_FLAGS      30

#define LSO_R_X86_64_NONE      0
#define LSO_R_X86_64_64        1
#define LSO_R_X86_64_GLOB_DAT  6
#define LSO_R_X86_64_JUMP_SLOT 7
#define LSO_R_X86_64_RELATIVE  8
#define LSO_R_X86_64_DTPMOD64  16
#define LSO_R_X86_64_DTPOFF64  17
#define LSO_R_X86_64_TPOFF64   18
#define LSO_R_X86_64_IRELATIVE 37

#define LSO_PAGE 4096UL

/* ---- loaded object -------------------------------------------------------- */

struct lso_atexit {
    struct MinNode node;
    void (*fn)(void *);
    void *arg;
    void *dso;
};

struct lso_segment {
    uintptr_t start, end;   /* absolute, page rounded */
    uint32_t flags;         /* PF_* */
    uint64_t offset;        /* file offset */
};

struct LinuxSO {
    struct MinNode node;            /* on lso_objects */
    char *path;
    char *unixpath;                 /* as reported in /proc/self/maps */
    APTR alloc;                     /* AllocMem block */
    size_t allocsize;
    uintptr_t base;                 /* load bias (page aligned) */
    size_t mapsize;
    struct lso_segment seg[8];
    int nseg;

    /* dynamic section, absolute addresses */
    const char *strtab;
    const lso_Sym *symtab;
    const uint32_t *hash;           /* DT_HASH: nbucket, nchain, buckets, chains */
    void (*init)(void);
    void (*fini)(void);
    void (**init_array)(void);
    size_t init_array_n;
    void (**fini_array)(void);
    size_t fini_array_n;

    /* PT_TLS */
    const void *tls_image;
    size_t tls_filesz, tls_memsz, tls_align;
    unsigned int tls_modid;         /* 1-based, index into the per-task DTV */

    /* .eh_frame registration with the program's libgcc */
    void *eh_frame;
    void *eh_object;                /* libgcc's opaque struct object */

    struct MinList atexit;          /* struct lso_atexit, newest first */
    int refcount;
};

/* Registry of loaded objects (for /proc/self/maps and dlsym) */
extern struct MinList lso_objects;
extern struct SignalSemaphore lso_lock;

/* glibc x86-64 time structures (AROS time_t is 32-bit, so these differ) */
struct lx_timespec { int64_t tv_sec, tv_nsec; };
struct lx_timeval { int64_t tv_sec, tv_usec; };

/* ---- per-task shim state ------------------------------------------------- */

#define LSO_MAX_TLS_MODULES 8

struct lso_task {
    int lx_errno;                   /* glibc-numbered errno for __errno_location */
    void *tls[LSO_MAX_TLS_MODULES]; /* per-module TLS blocks, lazily created */
    struct Task *task;
    char thread_name[32];
};

struct lso_task *lso_task_get(void);   /* create on first use */
void lso_task_free(struct Task *task); /* at thread exit */
void lso_tls_release(struct lso_task *lt);

/* ---- shim table ---------------------------------------------------------- */

struct lso_symbol {
    const char *name;
    void *addr;
};

/* Sorted by name; lso_shim_lookup() binary-searches it. */
void *lso_shim_lookup(const char *name);
void lso_shim_init(void);                   /* once, before the first object loads */
int lso_clock_gettime(int lx_clock, struct timespec *ts);
unsigned int lso_ncpus(void);

/* errno plumbing */
int lso_errno_to_linux(int aros_errno);
void lso_set_errno_from_aros(void);         /* copy the task's AROS errno, translated */
void lso_set_errno(int lx_errno);
int lso_pthread_ret(int aros_ret);          /* translate a pthread-style return code */

/* Linux x86-64 errno numbers used by the shim */
#define LX_EPERM   1
#define LX_ENOENT  2
#define LX_ESRCH   3
#define LX_EINTR   4
#define LX_EIO     5
#define LX_EBADF   9
#define LX_EAGAIN  11
#define LX_ENOMEM  12
#define LX_EACCES  13
#define LX_EFAULT  14
#define LX_EBUSY   16
#define LX_EEXIST  17
#define LX_ENOTDIR 20
#define LX_EISDIR  21
#define LX_EINVAL  22
#define LX_ENFILE  23
#define LX_EMFILE  24
#define LX_ENOSPC  28
#define LX_ESPIPE  29
#define LX_EPIPE   32
#define LX_ERANGE  34
#define LX_EDEADLK 35
#define LX_ENOSYS  38
#define LX_ENOTSUP 95
#define LX_ETIMEDOUT 110

/* Linux syscall emulation (native #UD trap path and the syscall() wrapper) */
long lso_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6);
int lso_syscall_trap_install(void);
long lso_getrandom(void *buf, size_t len);       /* rdrand-backed, never short */
void lso_syscall_trap_remove(void);

/* memory (mmap emulation) */
void *lso_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int lso_munmap(void *addr, size_t len);

/* files: Linux <-> AROS open flags, glibc struct stat, synthetic /proc */
int lso_open_flags_to_aros(int lxflags);
int lso_open(const char *path, int lxflags, int mode);
int lso_stat_to_glibc(const char *path, int fd, void *lxstat);
void lso_procfs_maps(char *buf, size_t len);

/* the loaded-object side of dlsym/__cxa_atexit */
void *lso_objects_sym(const char *name);
void lso_atexit_push(void (*fn)(void *), void *arg, void *dso);
void lso_atexit_run(void *dso);

#endif /* LINUXSO_INTERN_H */
