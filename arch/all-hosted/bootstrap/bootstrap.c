/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.
*/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

/* These macros are defined in both UNIX and AROS headers. Get rid of warnings. */
#undef __pure
#undef __const
#undef __pure2
#undef __deprecated

#include <aros/kernel.h>
#include <aros/multiboot.h>
#include <exec/execbase.h>
#include <utility/tagitem.h>

#include <elfloader.h>
#include <runtime.h>

#include "bootstrap.h"
#include "elf_io.h"
#include "filesystem.h"
#include "kickstart.h"
#include "log.h"
#include "memory.h"
#include "support.h"
#include "shutdown.h"

#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

#define D(x)

#define MEGABYTE      ((size_t)1024 * (size_t)1024)
#define LOWMEMSIZE    ((size_t)512 * MEGABYTE)

/* These macros are defined in both UNIX and AROS headers. Get rid of warnings. */
#undef __pure
#undef __const
#undef __pure2
#undef __deprecated

extern void *HostIFace;

/*
 * External libgcc routine which eventually clears instruction cache.
 * It is equivalent to gcc's __builtin___clear_cache(void *, void*)
 */
extern void __clear_cache(void *begin, void *end);

char bootstrapdir[PATH_MAX];
char *KernelArgs = NULL;
char *SystemVersion = NULL;
char buf[BUFFER_SIZE];

#if (__WORDSIZE != 64)
static struct mb_mmap MemoryMap[1] =
#else
static struct mb_mmap MemoryMap[2] =
#endif
{
    {
        sizeof(struct mb_mmap) - 4,
        0,
        0,
#if (__WORDSIZE != 64)
        0,
        0,
#endif
        MMAP_TYPE_RAM
    }
#if (__WORDSIZE == 64)
    , {
        sizeof(struct mb_mmap) - 4,
        0,
        0,
        MMAP_TYPE_RAM
    }
#endif
};

/* gdb can pick up kickstart segments from here */
static struct ELF_ModuleInfo *Debug_KickList = NULL;

static const char version[] = "$VER: AROS Hosted bootstrap v1.0 (" ADATE ")";

/* Kernel message */
static struct TagItem km[] =
{
    {KRN_KernelLowest , 0                },
    {KRN_KernelHighest, 0                },
    {KRN_KernelBss    , 0                },
    {KRN_BootLoader   , 0                },
    {KRN_DebugInfo    , 0                },
    {KRN_HostInterface, 0                },
    {KRN_MMAPAddress  , 0                },
    {KRN_MMAPLength   , sizeof(struct mb_mmap)},
    {KRN_CmdLine      , 0                },
    {TAG_DONE         , 0                }
};

/* Global SysBase */
struct ExecBase *SysBase;

static char *GetConfigArg(char *str, char *option)
{
    size_t l = strlen(option);

    /* First check option name */
    if (strncasecmp(str, option, l))
        return NULL;

    /* Skip option name */
    str += l;

    /* First character must be space */
    if (!isspace(*str++))
        return NULL;
    /* Skip the rest of spaces */
    while(isspace(*str))
        str++;

    return str;
}

static int ParseMemorySize(const char *value, size_t *size)
{
    const size_t max_megabytes = (size_t)-1 / MEGABYTE;
    const char *cursor;
    size_t parsed = 0;

    if (!value || !*value)
        return 0;

    cursor = value;
    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor == '+')
        cursor++;
    if (!isdigit((unsigned char)*cursor))
        return 0;

    while (isdigit((unsigned char)*cursor))
    {
        size_t digit = (size_t)(*cursor - '0');

        if ((parsed > max_megabytes / 10) ||
            ((parsed == max_megabytes / 10) &&
             (digit > max_megabytes % 10)))
            return 0;

        parsed = parsed * 10 + digit;
        cursor++;
    }

    while (isspace((unsigned char)*cursor))
        cursor++;
    if (*cursor || !parsed)
        return 0;

    *size = parsed * MEGABYTE;
    return 1;
}

char *join_string(int argc, char **argv)
{
    char *str, *s;
    int j;
    int x = 0;

    for (j = 0; j < argc; j++)
        x += (strlen(argv[j]) + 1);
    D(fprintf(stderr, "[Bootstrap] Allocating %u bytes for string\n", x));
    str = malloc(x);
    if (str) {
        s = str;
        for (j = 0; j < argc; j++) {
            strcpy(s, argv[j]);
            s += strlen(s);
            *s++ = ' ';
        }
        s[-1] = 0;
        D(fprintf(stderr, "[Bootstrap] Joined line: %s\n", str));
    }
    return str;
}

int bootstrap(int argc, char ** argv)
{
    int i = 1;
    size_t memoryBytes = (size_t)256 * MEGABYTE;
    size_t lowMemoryBytes;
#if (__WORDSIZE == 64)
    size_t highMemoryBytes;
#endif
    int def_memSize = 1;
    char *config = DefaultConfig;
    FILE *file;
    kernel_entry_fun_t kernel_entry;
    void *ro_addr, *rw_addr, *__bss_track;
    unsigned long ro_size, rw_size, bss_size;

    D(fprintf(stderr, "[Bootstrap] Started\n"));

    /*
     * This makes national characters to be output properly into
     * the debug log under Windows
     */
    setlocale(LC_ALL, "");
    if (!getcwd(bootstrapdir, sizeof(bootstrapdir)))
    {
        DisplayError("Failed to get current working directory!");
        return -1;
    }
    SaveArgs(argv);

    while (i < argc) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf ("Hosted AROS bootstrap\n"
                    "usage: %s [options] [kernel arguments]\n"
                    "Available options:\n"
                    " -h                 show this page\n"
                    " -m <size>          allocate <size> Megabytes of memory for AROS\n"
                    "                    (default is 256MB)\n"
                    " -c <file>          read configuration from <file>\n"
                    "                    (default is %s)\n"
                    " --help             same as '-h'\n"
                    " --memsize <size>   same as '-m <size>'\n"
                    " --config <file>    same as '-c'\n"
                    "\n"
                    "Please report bugs to the AROS development team. http://www.aros.org/\n", argv[0], DefaultConfig);
            return 0;
        } else if (!strcmp(argv[i], "--memsize") || !strcmp(argv[i], "-m")) {
            const char *value = (++i < argc) ? argv[i] : NULL;

            if (!ParseMemorySize(value, &memoryBytes))
            {
                DisplayError("Invalid AROS memory size: %s",
                             value ? value : "<missing>");
                return -1;
            }
            def_memSize = 0;
            i++;
        } else if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "-c")) {
            config = argv[++i];
            i++;
        } else
            break;
    }
    D(fprintf(stderr, "[Bootstrap] %u argument%s processed\n", i, i==1 ? "" : "s"));

    if (i < argc) {
        KernelArgs = join_string(argc - i, &argv[i]);
        D(fprintf(stderr, "[Bootstrap] Kernel arguments: %s\n", KernelArgs));
    }

    SystemVersion = getosversion(&version[6]);
    D(fprintf(stderr, "[Bootstrap] OS version: %s\n", SystemVersion));

    if (SetRootDirectory())
    {
        DisplayError("Failed to locate root directory!");
        return -1;
    }

    D(fprintf(stderr, "[Bootstrap] Opening configuration file %s\n", config));
    file = fopen(config, "r");
    if (!file)
    {
        DisplayError("Failed to load configuration file %s!", config);
        return -1;
    }

    /* Parse the configuration file */
    while (fgets(buf, sizeof(buf), file)) {
        char *c = strchr(buf, '\r');
      
        if (!c)
            c = strchr(buf, '\n');
        if (c)
            *c = 0;

        c = GetConfigArg(buf, "module");
        if (c) {
            AddKernelFile(c);
            continue;
        }

        /* Command line argument overrides this */
        if (def_memSize) {
            c = GetConfigArg(buf, "memory");
            if (c) {
                if (!ParseMemorySize(c, &memoryBytes))
                {
                    DisplayError("Invalid AROS memory size: %s", c);
                    fclose(file);
                    return -1;
                }
                continue;
            }
        }
        
        c = GetConfigArg(buf, "logfile");
        if (c)
        {
            i = SetLog(c);
            if (i)
            {
                DisplayError("Failed to redirect debug output to %s", c);
                return i;
            }
            fprintf(stderr, "----\n"); /* Separation marker */
        }

        c = GetConfigArg(buf, "arguments");
        if (c)
        {
            /* If we already have some arguments, append our line to them */
            if (KernelArgs)
            {
                char *parts[] = {KernelArgs, c};
                char *args = join_string(2, parts);
            
                if (args)
                {
                    free(KernelArgs);
                    KernelArgs = args;
                }
            }
            else
            {
                /* Otherwise just copy the line */
                i = strlen(c);
                if (i)
                {
                    i++;
                    KernelArgs = malloc(i);
                    memcpy(KernelArgs, c, i);
                }
            }
        }
    }
    fclose(file);

    lowMemoryBytes = memoryBytes;

    D(fprintf(stderr, "[Bootstrap] Allocating %zuMB of RAM for AROS\n",
              memoryBytes / MEGABYTE));

#if (__WORDSIZE == 64)
    if (memoryBytes > LOWMEMSIZE)
    {
        highMemoryBytes = memoryBytes - LOWMEMSIZE;
        MemoryMap[1].len = highMemoryBytes;
        MemoryMap[1].addr = (IPTR)AllocateRAM(highMemoryBytes);
        if (!MemoryMap[1].addr) {
            int err = errno;
            DisplayError("[Bootstrap] Failed to allocate %zuMB of 'High' RAM for AROS: %08x %s\n",
                         highMemoryBytes / MEGABYTE,
                         err, strerror(err));
            return -1;
        }
        fprintf(stderr, "[Bootstrap] RAM memory block allocated: %p - %p (%zu bytes)\n",
              (void *)(IPTR)MemoryMap[1].addr,
              (char *)(IPTR)MemoryMap[1].addr + highMemoryBytes,
              highMemoryBytes);

        lowMemoryBytes = LOWMEMSIZE;
        km[7].ti_Data += sizeof(struct mb_mmap);
    }
#endif
    MemoryMap[0].len = lowMemoryBytes;
    MemoryMap[0].addr = (IPTR)AllocateRAM32(lowMemoryBytes);

    if (!MemoryMap[0].addr) {
        int err = errno;
        DisplayError("[Bootstrap] Failed to allocate %zuMB of RAM for AROS: %08x %s\n",
                     lowMemoryBytes / MEGABYTE,
                     err, strerror(err));
        return -1;
    }
    fprintf(stderr, "[Bootstrap] RAM memory block allocated: %p - %p (%zu bytes)\n",
              (void *)(IPTR)MemoryMap[0].addr,
              (char *)(IPTR)MemoryMap[0].addr + lowMemoryBytes,
              lowMemoryBytes);

    if (!GetKernelSize(FirstELF, &ro_size, &rw_size, &bss_size))
        return -1;

    ro_addr = AllocateRO(ro_size);
    D(fprintf(stderr, "[Bootstrap] Kickstart ROM area: %p\n", ro_addr));
    if (!ro_addr)
    {
        DisplayError("Failed to allocate %u bytes for the kickstart!", ro_size);
        return -1;
    }

    rw_addr = AllocateRW(rw_size);
    D(fprintf(stderr, "[Bootstrap] Kickstart data area: %p\n", rw_addr));
    if (!rw_addr)
    {
        DisplayError("Failed to allocate %u bytes for the kickstart!", rw_size);
        return -1;
    }

    __bss_track = AllocateRO(bss_size);
    D(fprintf(stderr, "[Bootstrap] Kickstart BSS array: %p\n", __bss_track));
    if (!__bss_track)
    {
        DisplayError("Failed to allocate %u bytes for the kickstart!", bss_size);
        return -1;
    }

    if (!LoadKernel(FirstELF, ro_addr, rw_addr, __bss_track, (uintptr_t)&SysBase, NULL, &kernel_entry, &Debug_KickList))
        return -1;

    D(fprintf(stderr, "[Bootstrap] Read-only %p - %p, Read-write %p - %p, Entry %p, Debug info %p\n",
             ro_addr, ro_addr + ro_size - 1, rw_addr, rw_addr + rw_size - 1, kernel_entry, Debug_KickList));

    FreeKernelList();

    D(fprintf(stderr, "[Bootstrap] Sealing kernel code memory\n"));
    if (SetRO(ro_addr, ro_size))
    {
        DisplayError("Failed to set kernel memory permissions");
        return -1;
    }


    km[0].ti_Data = (IPTR)ro_addr;
    km[1].ti_Data = (IPTR)ro_addr + ro_size - 1;
    km[2].ti_Data = (IPTR)__bss_track;
    km[3].ti_Data = (IPTR)SystemVersion;
    km[4].ti_Data = (IPTR)Debug_KickList;
    km[5].ti_Data = (IPTR)HostIFace;
    km[6].ti_Data = (IPTR)&MemoryMap;
    /* km[7] is statically filled in */
    if (KernelArgs)
        km[8].ti_Data = (IPTR)KernelArgs;
    else
        km[8].ti_Tag = TAG_DONE;

    /* Flush instruction cache */
    __clear_cache((char *)ro_addr, (char *)ro_addr + ro_size);
    
#ifdef DEBUG_CODE
    /* This is a quickly hacked up sanity test which was used during iOS port development */
    if (ro_size > 256)
        ro_size = 256;
    
    fprintf(stderr, "[Bootstrap] Dumping %u bytes of code at %p\n", ro_size, ro_addr);
    for (i = 0; i < ro_size;)
    {
        if (i % 8 == 0)
            fprintf(stderr, "%p ", ro_addr + i);
        fprintf(stderr, "%02X ", ((unsigned char *)ro_addr)[i++]);
        if (i % 8 == 0)
            fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
#endif

    i = kick(kernel_entry, km);

    return i;
}
