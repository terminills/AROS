/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    linuxso ELF ET_DYN mapper: PT_LOAD into exec memory, dynamic
    relocation against the shim table, .eh_frame registration and the
    ELF constructor/destructor protocol.
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <aros/debug.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linuxso_intern.h"

struct MinList lso_objects;
struct SignalSemaphore lso_lock;
static int lso_initialised;
static int lso_debug = -1;
static unsigned int lso_next_tls_modid = 1;

/* libgcc's frame registry (unwind-dw2-fde.c) */
extern void __register_frame_info(const void *begin, void *ob);
extern void *__deregister_frame_info(const void *begin);

int linuxso_debug_enabled(void)
{
    if (lso_debug < 0)
        lso_debug = getenv("LINUXSO_DEBUG") != NULL;
    return lso_debug;
}

void lso_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    kprintf("[linuxso] %s\n", line);
}

static void lso_init_once(void)
{
    int first;

    Forbid();
    first = !lso_initialised;
    if (first)
    {
        NEWLIST(&lso_objects);
        InitSemaphore(&lso_lock);
        lso_initialised = 1;
    }
    Permit();
    if (first)
    {
        lso_shim_init();
        lso_syscall_trap_install();
    }
}

static void seterr(char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!errbuf || !errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    while (done < len)
    {
        ssize_t n = pread(fd, (char *)buf + done, len - done, (off_t)(off + done));
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;

    while (*name)
    {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* Find a defined symbol in one object via DT_HASH. */
static const lso_Sym *find_defined(struct LinuxSO *so, const char *name)
{
    uint32_t nbucket, nchain, i;
    const uint32_t *bucket, *chain;

    if (!so->hash)
        return NULL;
    nbucket = so->hash[0];
    nchain = so->hash[1];
    bucket = so->hash + 2;
    chain = bucket + nbucket;
    if (!nbucket)
        return NULL;

    for (i = bucket[elf_hash(name) % nbucket]; i && i < nchain; i = chain[i])
    {
        const lso_Sym *s = &so->symtab[i];
        if (s->st_shndx != LSO_SHN_UNDEF && strcmp(so->strtab + s->st_name, name) == 0)
            return s;
    }
    return NULL;
}

void *linuxso_sym(struct LinuxSO *so, const char *name)
{
    const lso_Sym *s;

    if (!so || !name)
        return NULL;
    s = find_defined(so, name);
    if (!s)
        return NULL;
    if ((s->st_info & 0xf) == LSO_STT_TLS)
        return NULL;
    return (void *)(so->base + s->st_value);
}

void *lso_objects_sym(const char *name)
{
    struct LinuxSO *so;
    void *p = NULL;

    ObtainSemaphoreShared(&lso_lock);
    ForeachNode(&lso_objects, so)
    {
        p = linuxso_sym(so, name);
        if (p)
            break;
    }
    ReleaseSemaphore(&lso_lock);
    return p;
}

void lso_atexit_push(void (*fn)(void *), void *arg, void *dso)
{
    struct lso_atexit *a = AllocMem(sizeof(*a), MEMF_ANY | MEMF_CLEAR);
    struct LinuxSO *so;

    if (!a)
        return;
    a->fn = fn;
    a->arg = arg;
    a->dso = dso;

    ObtainSemaphore(&lso_lock);
    /* the CDM passes its own __dso_handle, which lives inside its mapping */
    ForeachNode(&lso_objects, so)
    {
        if ((uintptr_t)dso >= so->base && (uintptr_t)dso < so->base + so->mapsize)
        {
            AddHead((struct List *)&so->atexit, (struct Node *)&a->node);
            a = NULL;
            break;
        }
    }
    if (a)
    {
        /* unknown owner: attach to the most recently loaded object */
        so = (struct LinuxSO *)GetHead(&lso_objects);
        if (so)
            AddHead((struct List *)&so->atexit, (struct Node *)&a->node);
        else
            FreeMem(a, sizeof(*a));
    }
    ReleaseSemaphore(&lso_lock);
}

static void run_atexit(struct LinuxSO *so)
{
    struct lso_atexit *a;

    while ((a = (struct lso_atexit *)RemHead((struct List *)&so->atexit)))
    {
        LSO_DEBUG("atexit %p(%p)", a->fn, a->arg);
        a->fn(a->arg);
        FreeMem(a, sizeof(*a));
    }
}

void lso_atexit_run(void *dso)
{
    struct LinuxSO *so;

    ObtainSemaphore(&lso_lock);
    ForeachNode(&lso_objects, so)
    {
        if (!dso || ((uintptr_t)dso >= so->base && (uintptr_t)dso < so->base + so->mapsize))
            run_atexit(so);
    }
    ReleaseSemaphore(&lso_lock);
}

static const char *sym_name(struct LinuxSO *so, uint32_t idx)
{
    return so->strtab + so->symtab[idx].st_name;
}

/*
 * Resolve the symbol of one relocation.  Defined symbols bind inside the
 * object; undefined ones go to the shim.  Returns 0 and records the name
 * when a non-weak symbol is missing.
 */
static int resolve(struct LinuxSO *so, uint32_t idx, uintptr_t *value,
                   char *missing, size_t missinglen)
{
    const lso_Sym *s = &so->symtab[idx];
    const char *name = sym_name(so, idx);
    void *p;

    if (s->st_shndx != LSO_SHN_UNDEF)
    {
        *value = so->base + s->st_value;
        return 1;
    }
    p = lso_shim_lookup(name);
    if (p)
    {
        *value = (uintptr_t)p;
        return 1;
    }
    if ((s->st_info >> 4) == LSO_STB_WEAK)
    {
        LSO_DEBUG("weak undefined %s -> 0", name);
        *value = 0;
        return 1;
    }
    if (missing[0])
        strncat(missing, " ", missinglen - strlen(missing) - 1);
    strncat(missing, name, missinglen - strlen(missing) - 1);
    *value = 0;
    return 0;
}

static int apply_relocs(struct LinuxSO *so, const lso_Rela *rela, size_t n,
                        char *missing, size_t missinglen)
{
    size_t i;
    int failed = 0;

    for (i = 0; i < n; i++)
    {
        uint32_t type = (uint32_t)rela[i].r_info;
        uint32_t sym = (uint32_t)(rela[i].r_info >> 32);
        uint64_t *where = (uint64_t *)(so->base + rela[i].r_offset);
        uintptr_t S = 0;

        switch (type)
        {
        case LSO_R_X86_64_NONE:
            break;
        case LSO_R_X86_64_RELATIVE:
            *where = so->base + rela[i].r_addend;
            break;
        case LSO_R_X86_64_64:
            if (!resolve(so, sym, &S, missing, missinglen)) failed++;
            *where = S + rela[i].r_addend;
            break;
        case LSO_R_X86_64_GLOB_DAT:
            if (!resolve(so, sym, &S, missing, missinglen)) failed++;
            *where = S + rela[i].r_addend;
            break;
        case LSO_R_X86_64_JUMP_SLOT:
            if (!resolve(so, sym, &S, missing, missinglen)) failed++;
            *where = S;
            break;
        case LSO_R_X86_64_DTPMOD64:
            /* module id of the TLS block; only our own module is supported */
            *where = so->tls_modid;
            break;
        case LSO_R_X86_64_DTPOFF64:
            *where = so->symtab[sym].st_value + rela[i].r_addend;
            break;
        case LSO_R_X86_64_TPOFF64:
            /* initial-exec TLS needs a %fs-based block we do not own */
            if (missing[0]) strncat(missing, " ", missinglen - strlen(missing) - 1);
            strncat(missing, "<R_X86_64_TPOFF64>", missinglen - strlen(missing) - 1);
            failed++;
            break;
        default:
            LSO_DEBUG("unsupported relocation type %u at %p", type, where);
            if (missing[0]) strncat(missing, " ", missinglen - strlen(missing) - 1);
            strncat(missing, "<reloc-type>", missinglen - strlen(missing) - 1);
            failed++;
            break;
        }
    }
    return failed;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = AllocVec(n, MEMF_ANY);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* "SYS:Applications/x/y.so" -> "/SYS/Applications/x/y.so" for /proc/self/maps */
static char *unix_path(const char *p)
{
    const char *colon = strchr(p, ':');
    char *u;
    size_t n = strlen(p);

    if (!colon || p[0] == '/')
        return dupstr(p);
    u = AllocVec(n + 2, MEMF_ANY);
    if (!u)
        return NULL;
    u[0] = '/';
    memcpy(u + 1, p, colon - p);
    u[1 + (colon - p)] = '/';
    strcpy(u + 2 + (colon - p), colon + 1);
    return u;
}

static void free_object(struct LinuxSO *so)
{
    if (so->alloc)
        FreeMem(so->alloc, so->allocsize);
    if (so->eh_object)
        FreeMem(so->eh_object, 64);
    FreeVec(so->path);
    FreeVec(so->unixpath);
    FreeMem(so, sizeof(*so));
}

struct LinuxSO *linuxso_open(const char *path, char *errbuf, size_t errlen)
{
    struct LinuxSO *so;
    lso_Ehdr eh;
    lso_Phdr *ph = NULL;
    lso_Shdr *sh = NULL;
    char *shstr = NULL;
    int fd = -1, i;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    const lso_Dyn *dyn = NULL;
    const lso_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0;
    char missing[1024];

    lso_init_once();
    if (errbuf && errlen)
        errbuf[0] = 0;

    so = AllocMem(sizeof(*so), MEMF_ANY | MEMF_CLEAR);
    if (!so)
    {
        seterr(errbuf, errlen, "out of memory");
        return NULL;
    }
    NEWLIST(&so->atexit);
    so->refcount = 1;
    so->path = dupstr(path);
    so->unixpath = unix_path(path);

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        seterr(errbuf, errlen, "cannot open %s", path);
        goto fail;
    }
    if (read_at(fd, &eh, sizeof(eh), 0) < 0 || memcmp(eh.e_ident, "\177ELF\002\001", 6) != 0)
    {
        seterr(errbuf, errlen, "%s: not an ELF64 little-endian object", path);
        goto fail;
    }
    if (eh.e_type != LSO_ET_DYN || eh.e_machine != LSO_EM_X86_64)
    {
        seterr(errbuf, errlen, "%s: not an x86-64 ET_DYN object (type %u machine %u)",
               path, eh.e_type, eh.e_machine);
        goto fail;
    }
    if (eh.e_phentsize != sizeof(lso_Phdr) || eh.e_phnum == 0 || eh.e_phnum > 64)
    {
        seterr(errbuf, errlen, "%s: bad program header table", path);
        goto fail;
    }

    ph = AllocVec(eh.e_phnum * sizeof(lso_Phdr), MEMF_ANY);
    if (!ph || read_at(fd, ph, eh.e_phnum * sizeof(lso_Phdr), eh.e_phoff) < 0)
    {
        seterr(errbuf, errlen, "%s: cannot read program headers", path);
        goto fail;
    }

    /* extent of the image */
    for (i = 0; i < eh.e_phnum; i++)
    {
        if (ph[i].p_type != LSO_PT_LOAD)
            continue;
        if (ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr & ~(LSO_PAGE - 1);
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (hi == 0)
    {
        seterr(errbuf, errlen, "%s: no PT_LOAD segments", path);
        goto fail;
    }
    hi = (hi + LSO_PAGE - 1) & ~(LSO_PAGE - 1);
    so->mapsize = hi - lo;
    so->allocsize = so->mapsize + LSO_PAGE;
    /* Plain exec memory is executable on every AROS target LoadSeg works on. */
    so->alloc = AllocMem(so->allocsize, MEMF_ANY | MEMF_CLEAR);
    if (!so->alloc)
    {
        seterr(errbuf, errlen, "%s: cannot allocate %lu bytes for the image",
               path, (unsigned long)so->allocsize);
        goto fail;
    }
    so->base = (((uintptr_t)so->alloc + LSO_PAGE - 1) & ~(LSO_PAGE - 1)) - lo;
    LSO_DEBUG("%s: image %lu bytes at %p (bias %p)", path,
              (unsigned long)so->mapsize, (void *)(so->base + lo), (void *)so->base);

    /* map the segments */
    for (i = 0; i < eh.e_phnum; i++)
    {
        lso_Phdr *p = &ph[i];

        switch (p->p_type)
        {
        case LSO_PT_LOAD:
            if (p->p_filesz &&
                read_at(fd, (void *)(so->base + p->p_vaddr), p->p_filesz, p->p_offset) < 0)
            {
                seterr(errbuf, errlen, "%s: short read on segment %d", path, i);
                goto fail;
            }
            if (so->nseg < (int)(sizeof(so->seg) / sizeof(so->seg[0])))
            {
                so->seg[so->nseg].start = (so->base + p->p_vaddr) & ~(LSO_PAGE - 1);
                so->seg[so->nseg].end = (so->base + p->p_vaddr + p->p_memsz + LSO_PAGE - 1) & ~(LSO_PAGE - 1);
                so->seg[so->nseg].flags = p->p_flags;
                so->seg[so->nseg].offset = p->p_offset & ~(LSO_PAGE - 1);
                so->nseg++;
            }
            LSO_DEBUG("  PT_LOAD %c%c%c vaddr %p filesz %lx memsz %lx",
                      (p->p_flags & LSO_PF_R) ? 'r' : '-', (p->p_flags & LSO_PF_W) ? 'w' : '-',
                      (p->p_flags & LSO_PF_X) ? 'x' : '-', (void *)(so->base + p->p_vaddr),
                      (unsigned long)p->p_filesz, (unsigned long)p->p_memsz);
            break;
        case LSO_PT_DYNAMIC:
            dyn = (const lso_Dyn *)(so->base + p->p_vaddr);
            break;
        case LSO_PT_TLS:
            so->tls_image = (const void *)(so->base + p->p_vaddr);
            so->tls_filesz = p->p_filesz;
            so->tls_memsz = p->p_memsz;
            so->tls_align = p->p_align ? p->p_align : 16;
            break;
        default:
            break;
        }
    }
    if (!dyn)
    {
        seterr(errbuf, errlen, "%s: no PT_DYNAMIC", path);
        goto fail;
    }
    if (so->tls_memsz)
    {
        if (lso_next_tls_modid > LSO_MAX_TLS_MODULES)
        {
            seterr(errbuf, errlen, "%s: too many TLS modules", path);
            goto fail;
        }
        so->tls_modid = lso_next_tls_modid++;
        LSO_DEBUG("  PT_TLS image %p filesz %lx memsz %lx align %lx -> module %u",
                  so->tls_image, (unsigned long)so->tls_filesz, (unsigned long)so->tls_memsz,
                  (unsigned long)so->tls_align, so->tls_modid);
    }

    /* dynamic section */
    for (; dyn->d_tag != LSO_DT_NULL; dyn++)
    {
        switch (dyn->d_tag)
        {
        case LSO_DT_STRTAB:     so->strtab = (const char *)(so->base + dyn->d_val); break;
        case LSO_DT_SYMTAB:     so->symtab = (const lso_Sym *)(so->base + dyn->d_val); break;
        case LSO_DT_HASH:       so->hash = (const uint32_t *)(so->base + dyn->d_val); break;
        case LSO_DT_RELA:       rela = (const lso_Rela *)(so->base + dyn->d_val); break;
        case LSO_DT_RELASZ:     relasz = dyn->d_val; break;
        case LSO_DT_JMPREL:     jmprel = (const lso_Rela *)(so->base + dyn->d_val); break;
        case LSO_DT_PLTRELSZ:   pltrelsz = dyn->d_val; break;
        case LSO_DT_INIT:       so->init = (void (*)(void))(so->base + dyn->d_val); break;
        case LSO_DT_FINI:       so->fini = (void (*)(void))(so->base + dyn->d_val); break;
        case LSO_DT_INIT_ARRAY: so->init_array = (void (**)(void))(so->base + dyn->d_val); break;
        case LSO_DT_INIT_ARRAYSZ: so->init_array_n = dyn->d_val / sizeof(void *); break;
        case LSO_DT_FINI_ARRAY: so->fini_array = (void (**)(void))(so->base + dyn->d_val); break;
        case LSO_DT_FINI_ARRAYSZ: so->fini_array_n = dyn->d_val / sizeof(void *); break;
        case LSO_DT_NEEDED:
            LSO_DEBUG("  NEEDED %s (provided by the shim)", so->strtab ? so->strtab + dyn->d_val : "?");
            break;
        default:
            break;
        }
    }
    if (!so->strtab || !so->symtab)
    {
        seterr(errbuf, errlen, "%s: no dynamic symbol table", path);
        goto fail;
    }
    if (!so->hash)
    {
        seterr(errbuf, errlen, "%s: no DT_HASH (DT_GNU_HASH only is not supported)", path);
        goto fail;
    }

    /* relocate */
    missing[0] = 0;
    if (apply_relocs(so, rela, relasz / sizeof(lso_Rela), missing, sizeof(missing)) +
        apply_relocs(so, jmprel, pltrelsz / sizeof(lso_Rela), missing, sizeof(missing)))
    {
        seterr(errbuf, errlen, "%s: unresolved symbols: %s", path, missing);
        goto fail;
    }
    LSO_DEBUG("  relocated %lu + %lu entries", (unsigned long)(relasz / sizeof(lso_Rela)),
              (unsigned long)(pltrelsz / sizeof(lso_Rela)));

    /* .eh_frame: let the program's libgcc unwind through the object */
    if (eh.e_shoff && eh.e_shentsize == sizeof(lso_Shdr) && eh.e_shnum && eh.e_shnum < 512 &&
        eh.e_shstrndx < eh.e_shnum)
    {
        sh = AllocVec(eh.e_shnum * sizeof(lso_Shdr), MEMF_ANY);
        if (sh && read_at(fd, sh, eh.e_shnum * sizeof(lso_Shdr), eh.e_shoff) == 0)
        {
            shstr = AllocVec(sh[eh.e_shstrndx].sh_size + 1, MEMF_ANY | MEMF_CLEAR);
            if (shstr && read_at(fd, shstr, sh[eh.e_shstrndx].sh_size, sh[eh.e_shstrndx].sh_offset) == 0)
            {
                for (i = 0; i < eh.e_shnum; i++)
                {
                    if (sh[i].sh_name < sh[eh.e_shstrndx].sh_size &&
                        strcmp(shstr + sh[i].sh_name, ".eh_frame") == 0 && sh[i].sh_addr)
                    {
                        so->eh_frame = (void *)(so->base + sh[i].sh_addr);
                        break;
                    }
                }
            }
        }
    }
    if (so->eh_frame)
    {
        so->eh_object = AllocMem(64, MEMF_ANY | MEMF_CLEAR);
        if (so->eh_object)
            __register_frame_info(so->eh_frame, so->eh_object);
        LSO_DEBUG("  .eh_frame at %p registered", so->eh_frame);
    }
    else
        LSO_DEBUG("  no .eh_frame section found; C++ exceptions inside the object will terminate");

    FreeVec(shstr);
    FreeVec(sh);
    FreeVec(ph);
    close(fd);
    shstr = NULL; sh = NULL; ph = NULL; fd = -1;

    ObtainSemaphore(&lso_lock);
    AddHead((struct List *)&lso_objects, (struct Node *)&so->node);
    ReleaseSemaphore(&lso_lock);

    /* constructors */
    if (so->init)
    {
        LSO_DEBUG("  DT_INIT %p", so->init);
        so->init();
    }
    for (i = 0; i < (int)so->init_array_n; i++)
    {
        if (so->init_array[i] && (uintptr_t)so->init_array[i] != ~(uintptr_t)0)
        {
            LSO_DEBUG("  init_array[%d] %p", i, so->init_array[i]);
            so->init_array[i]();
        }
    }
    LSO_DEBUG("%s: loaded", path);
    return so;

fail:
    FreeVec(shstr);
    FreeVec(sh);
    FreeVec(ph);
    if (fd >= 0)
        close(fd);
    if (so->tls_modid && so->tls_modid == lso_next_tls_modid - 1)
        lso_next_tls_modid--;
    free_object(so);
    return NULL;
}

int linuxso_close(struct LinuxSO *so)
{
    int i;

    if (!so)
        return -1;
    if (--so->refcount > 0)
        return 0;

    for (i = (int)so->fini_array_n - 1; i >= 0; i--)
        if (so->fini_array[i] && (uintptr_t)so->fini_array[i] != ~(uintptr_t)0)
            so->fini_array[i]();
    if (so->fini)
        so->fini();
    run_atexit(so);

    ObtainSemaphore(&lso_lock);
    Remove((struct Node *)&so->node);
    ReleaseSemaphore(&lso_lock);

    if (so->eh_frame && so->eh_object)
        __deregister_frame_info(so->eh_frame);
    LSO_DEBUG("%s: unloaded", so->path);
    free_object(so);
    return 0;
}

/* /proc/self/maps as the guest expects it: one line per mapped segment. */
void lso_procfs_maps(char *buf, size_t len)
{
    struct LinuxSO *so;
    size_t used = 0;
    int i;

    buf[0] = 0;
    ObtainSemaphoreShared(&lso_lock);
    ForeachNode(&lso_objects, so)
    {
        for (i = 0; i < so->nseg && used < len; i++)
        {
            used += snprintf(buf + used, len - used, "%08lx-%08lx %c%c%cp %08lx 00:00 0 %s\n",
                             (unsigned long)so->seg[i].start, (unsigned long)so->seg[i].end,
                             (so->seg[i].flags & LSO_PF_R) ? 'r' : '-',
                             (so->seg[i].flags & LSO_PF_W) ? 'w' : '-',
                             (so->seg[i].flags & LSO_PF_X) ? 'x' : '-',
                             (unsigned long)so->seg[i].offset,
                             so->unixpath ? so->unixpath : "");
        }
    }
    ReleaseSemaphore(&lso_lock);
}
