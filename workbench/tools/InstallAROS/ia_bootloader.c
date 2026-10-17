/*
    Copyright (C) 2018-2026, The AROS Development Team. All rights reserved.
*/

#define INTUITION_NO_INLINE_STDARG

#include <aros/debug.h>

#include <libraries/mui.h>

#include <dos/dos.h>
#include <exec/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <clib/alib_protos.h>

#include <intuition/gadgetclass.h>
#include <intuition/icclass.h>
#include <gadgets/colorwheel.h>

#include <libraries/asl.h>
#include <libraries/expansionbase.h>

#include <devices/trackdisk.h>
#include <devices/scsidisk.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <proto/partition.h>
#include <proto/muimaster.h>
#include <proto/graphics.h>
#include <proto/utility.h>

#include "ia_locale.h"
#include "ia_install.h"
#include "ia_stage.h"
#include "ia_option.h"
#include "ia_stage_intern.h"
#include "ia_bootloader.h"
#include "ia_diskio.h"

extern char *source_Path;

extern Object   *cycle_drivetype;
extern Object   *optObjCycleGrub2Mode;
BOOL            gfx_font_exists;

LONG BootLoaderType;

#define GRUB_COPY_FILE_LOOP(files)                                              \
SET(data->gauge2, MUIA_Gauge_Current, 0);                                       \
                                                                                \
while (files[file_count] != NULL && data->inst_success == MUIV_Inst_InProgress) \
{                                                                               \
    ULONG newSrcLen = strlen(srcPath) + strlen(files[file_count]) + 2;        \
    ULONG newDstLen = strlen(dstPath) + strlen(files[file_count + 1]) + 2;    \
                                                                                \
    TEXT srcFile[newSrcLen];                                                    \
    TEXT dstFile[newDstLen];                                                    \
                                                                                \
    sprintf(srcFile, "%s", srcPath);                                            \
    sprintf(dstFile, "%s", dstPath);                                            \
    AddPart(srcFile, files[file_count], newSrcLen);                         \
    AddPart(dstFile, files[file_count + 1], newDstLen);                     \
                                                                                \
    DoMethod(self, MUIM_IC_CopyFile, srcFile, dstFile);                         \
                                                                                \
    file_count += 2;                                                            \
                                                                                \
    if (numgrubfiles > 0)                                                       \
    {                                                                           \
        SET(data->gauge2, MUIA_Gauge_Current,                                   \
            (LONG)((100.0 / numgrubfiles) * (file_count / 2.0)));               \
    }                                                                           \
}

/* Files to check for. Must be in the same order as BootLoaderTypes enum values */

struct BootLoaderInfo BootLoaderData[] = {
#if defined(INSTALL_BL_GRUB)
    {
        "grub",
        "SYS:",
        "boot/grub/stage1",
        "c/Install-grub",
        NULL
    },
#endif
#if defined(INSTALL_BL_GRUB2)
    {
        "grub",
        "SYS:",
        "boot/grub/" GRUBARCHDIR "/core.img",
        "c/Install-grub2",
        "boot/grub/fonts/unicode.pf2"
    },
#if (0)
    {
        "grub EFI",
        "SYS:",
        "EFI/BOOT/BOOTX64.EFI",
        NULL,
        NULL
    },
#endif
#endif
    {
        (CONST_STRPTR)-1,
        (CONST_STRPTR)-1,
        (CONST_STRPTR)-1,
        (CONST_STRPTR)-1
    }
};

#define BOOTLOADER_PATH_LEN 64  /* Must be large enough to contain any of the bootloader match strings */

BOOL _matchPath(CONST_STRPTR *match)
{
    ULONG matchSrcLen = strlen(source_Path) + BOOTLOADER_PATH_LEN;
    TEXT matchSrc[matchSrcLen];
    BPTR lock;

    strcpy(matchSrc, source_Path);
    AddPart(matchSrc, *match, matchSrcLen);

    D(bug("[InstallAROS] %s: trying match '%s'\n", __func__, matchSrc));

    lock = Lock(matchSrc, ACCESS_READ);
    if (lock != BNULL)
    {
        UnLock(lock);
        return TRUE;
    }
    *match = NULL;
    return FALSE;
}

void BOOTLOADER_InitSupport(void)
{
    LONG i, lastmatch;
    BootLoaderType = lastmatch = BOOTLOADER_NONE;

    D(bug("[InstallAROS] %s()\n", __func__));

    for (i = 0; BootLoaderData[i].match != (CONST_STRPTR)-1; i++)
    {
        D(bug("[InstallAROS] %s: #%d = '%s'\n", __func__, i, BootLoaderData[i].path));

        if (BootLoaderData[i].match && _matchPath(&BootLoaderData[i].match))
        {
            D(bug("[InstallAROS] %s: bootloader files found\n", __func__));
            if (BootLoaderData[i].match2 && _matchPath(&BootLoaderData[i].match2))
            {
                D(bug("[InstallAROS] %s: bootloader installer found\n", __func__));
#if defined(INSTALL_BL_GRUB)
# if defined(GRUB) && (GRUB == 1)
                if (i == BOOTLOADER_GRUB1)
                    BootLoaderType = i;
#endif
#endif
#if defined(INSTALL_BL_GRUB2)
# if defined(GRUB) && (GRUB == 2)
                if (i == BOOTLOADER_GRUB2)
                    BootLoaderType = i;
#endif
#endif
                lastmatch = i;

                if (BootLoaderData[i].match3)
                    _matchPath(&BootLoaderData[i].match3);
            }
            else
                BootLoaderData[i].match = NULL;
        }
    }
#if defined(INSTALL_BL_GRUB2)
    if (BootLoaderData[BOOTLOADER_GRUB2].match3)
    {
        gfx_font_exists = TRUE;
        OPTOSET(optObjCycleGrub2Mode, MUIA_Cycle_Active, 1);
    }
    else
        gfx_font_exists = FALSE;
#endif
    if ((BootLoaderType == BOOTLOADER_NONE) && (lastmatch != BOOTLOADER_NONE))
        BootLoaderType = lastmatch;
}

void BOOTLOADER_AddCoreSkipPaths(struct List *SkipList)
{
    ULONG skipPathLen = strlen(source_Path) + BOOTLOADER_PATH_LEN;
    TEXT skipPath[skipPathLen];
#if defined(INSTALL_BL_GRUB) || defined(INSTALL_BL_GRUB2)
    BOOL grubSkipped = FALSE;
#endif

#if defined(INSTALL_BL_GRUB)
    strcpy(skipPath, source_Path);
    AddPart(skipPath, "boot/grub", skipPathLen);
    AddSkipListEntry(SkipList, skipPath);
    grubSkipped = TRUE;
#endif
#if defined(INSTALL_BL_GRUB2)
    if (!grubSkipped)
    {
        strcpy(skipPath, source_Path);
        AddPart(skipPath, "boot/grub", skipPathLen);
        AddSkipListEntry(SkipList, skipPath);
        grubSkipped = TRUE;
    }
#endif
}

BOOL BOOTLOADER_PartFixUp(struct InstallStage_DATA *data, IPTR systype)
{
    D(bug("[InstallAROS] %s()\n", __func__));

#if defined(INSTALL_BL_GRUB)
    /* Warn user about using non FFS-Intl filesystem for system
       partition with GRUB */
    if ((BootLoaderType == BOOTLOADER_GRUB1) && (systype != 0))
    {
        if (MUI_RequestA(data->installer, data->window, 0,
                _(MSG_WARNING),
                _(MSG_CONTINUECANCELPART),
                _(MSG_GRUBNONFFSWARNING), NULL) != 1)
            return FALSE;
    }
#endif
    return TRUE;
}

static BOOL IsESPType(UBYTE id, struct DosEnvec *de);

static LONG FindWindowsPartition(STRPTR device, LONG unit)
{
    IPTR active, id;
    struct PartitionType type;
    struct PartitionHandle *root, *partition;
    LONG partition_no = -1, i = 0;

    D(bug("[InstallAROS] %s()\n", __func__));

    if ((root = OpenRootPartition(device, unit)) != NULL)
    {
        if (OpenPartitionTable(root) == 0)
        {
            /* Look for an active partition with a Windows FS */
            ForeachNode(&root->table->list, partition)
            {
                GetPartitionAttrsTags
                    (partition,
                    PT_ACTIVE, (IPTR) & active,
                    PT_TYPE, (IPTR) & type, TAG_DONE);
                id = type.id[0];
                if (active && (id == 0x7 || id == 0xb || id == 0xc))
                {
                    struct DosEnvec de;

                    GetPartitionAttrsTags(partition, PT_DOSENVEC, (IPTR)&de, TAG_DONE);
                    if (!IsESPType(id, &de))
                        partition_no = i;
                }
                i++;
            }
            ClosePartitionTable(root);
        }
        CloseRootPartition(root);
    }

    return partition_no;
}

void BOOTLOADER_DoInstall(Class * CLASS, Object * self)
{
    struct InstallStage_DATA *data = INST_DATA(CLASS, self);
    ULONG srcLen =
        strlen(source_Path) + BOOTLOADER_PATH_LEN + 8;
    ULONG dstLen =
        strlen(data->install_SysTarget) + BOOTLOADER_PATH_LEN + 8;
    int numgrubfiles = 0, file_count = 0;
    TEXT srcPath[srcLen];
    TEXT dstPath[dstLen];
    TEXT tmp[256];
    IPTR option = 0;
    LONG part_no;

    /* Installing Bootloader */
    D(bug("[InstallAROS] %s()\n", __func__));
    SET(data->label, MUIA_Text_Contents, __(MSG_INSTALLINGBOOT));
    SET(data->pageheader, MUIA_Text_Contents, __(MSG_BOOTLOADER));
    SET(data->label, MUIA_Text_Contents, __(MSG_COPYBOOT));

    strcpy(srcPath, source_Path);
    AddPart(srcPath, "boot", srcLen);
    sprintf(dstPath, "%s:boot", data->install_SysTarget);

    D(
        bug("[InstallAROS] %s: Boot Device = %s/%d\n", __func__, data->bl_TargetDevice, data->bl_TargetUnit);
        bug("[InstallAROS] %s: Source Path = '%s'\n", __func__, srcPath);
        bug("[InstallAROS] %s: Dest Path = '%s'\n", __func__, dstPath);
    )

    switch (BootLoaderType)
    {
#if defined(INSTALL_BL_GRUB2)
    case BOOTLOADER_GRUB2:
        {
            CONST_STRPTR grub2_dirs[] = {
                BootLoaderData[BOOTLOADER_GRUB2].path, BootLoaderData[BOOTLOADER_GRUB2].path,
                NULL
            };
            CopyDirArray(CLASS, self, srcPath, dstPath, grub2_dirs, NULL);
            AddPart(srcPath, BootLoaderData[BOOTLOADER_GRUB2].path, srcLen);
            AddPart(dstPath, BootLoaderData[BOOTLOADER_GRUB2].path, dstLen);

            /* Warning: do not modify srcPath or dstPath beyond this point */

            /* Grub 2 text/gfx mode */
            GET(data->instc_options_grub->gopt_grub2mode, MUIA_Cycle_Active,
                &option);

            /* Rename _unicode.pf2 <-> unicode.pf2 if necessary */
            {
                ULONG newDstLen =
                    strlen(dstPath) + strlen("_unicode.pf2") + 8;
                TEXT srcFile[newDstLen];
                TEXT dstFile[newDstLen];

                sprintf(srcFile, "%s/fonts", dstPath);
                sprintf(dstFile, "%s/fonts", dstPath);
                AddPart(srcFile, "_unicode.pf2", newDstLen);
                AddPart(dstFile, "unicode.pf2", newDstLen);

                if (option == 1 && !gfx_font_exists)
                    Rename(srcFile, dstFile);
                else if (option == 0 && gfx_font_exists)
                    Rename(dstFile, srcFile);
            }

            /* Add entry to boot MS Windows if present */
            if ((part_no =
                    FindWindowsPartition(data->bl_TargetDevice, data->bl_TargetUnit)) != -1
                && XGET(cycle_drivetype, MUIA_Cycle_Active) != 2)
            {
                TEXT cmd[256];
                sprintf(tmp, "%s", dstPath);
                AddPart(tmp, "grub.cfg", 256);

                /* Make sure the config file is writeable */
                sprintf(cmd, "Protect ADD FLAGS=W ALL QUIET %s",
                    tmp);
                D(bug
                        ("[InstallAROS] %s: calling '%s'\n", __func__, cmd));
                LONG success = Execute(cmd, NULL, NULL);
                if (success)
                {
                    BPTR menu_file = Open(tmp, MODE_READWRITE);

                    if (menu_file != NULL)
                    {
                        Seek(menu_file, 0, OFFSET_END);
                        FPrintf(menu_file, "\nmenuentry \"Microsoft Windows\" {\n    chainloader (hd%ld,%ld)+1\n}\n\n", 0, part_no + 1);    /* GRUB2 counts partitions from 1 */
                        Close(menu_file);
                    }
                    D(bug
                        ("[InstallAROS] %s: Windows partition found. Adding Windows option to GRUB2 menu.\n", __func__));
                }
            }

            sprintf(tmp,
                "C:Install-grub2 DEVICE \"%s\" UNIT %d GRUB \"%s\"",
                data->bl_TargetDevice, data->bl_TargetUnit, dstPath);

            break;
        }
#endif
#if defined(INSTALL_BL_GRUB)
    case BOOTLOADER_GRUB1:

        CreateDir(dstPath);

        numgrubfiles = 3;

        TEXT *grub_files[] = {
            "stage1", "stage1",
            "stage2_hdisk", "stage2",
            "menu.lst.DH0", "menu.lst",
            NULL
        };

        GRUB_COPY_FILE_LOOP(grub_files);

        /* Add entry to boot MS Windows if present */
        if ((part_no =
                FindWindowsPartition(data->bl_TargetDevice, data->bl_TargetUnit)) != -1)
        {
            sprintf(tmp, "%s", dstPath);
            AddPart(tmp, "menu.lst", 256);

            BPTR menu_file = Open(tmp, MODE_READWRITE);
            if (menu_file != NULL)
            {
                Seek(menu_file, 0, OFFSET_END);
                FPrintf(menu_file,
                    "\ntitle Microsoft Windows\nrootnoverify (hd%ld,%ld)\nchainloader +1\n",
                    0, part_no);
                Close(menu_file);
            }
            D(bug
                ("[InstallAROS] %s: Windows partition found. Adding Windows option to GRUB menu.\n", __func__));
        }

        sprintf(tmp,
            "C:Install-grub DEVICE \"%s\" UNIT %d GRUB \"%s\" FORCELBA",
            data->bl_TargetDevice, data->bl_TargetUnit, dstPath);

        break;
#endif
    default:
        /* TODO: support more bootloaders */
        break;
    }

    D(bug("[InstallAROS] %s: executing '%s'\n", __func__, tmp));
    Execute(tmp, NULL, NULL);
    D(bug("[InstallAROS] %s: finished\n", __func__));

    SET(data->gauge2, MUIA_Gauge_Current, 100);
}

/* UEFI ****************************************************************/

#define EFI_LOADER      "efi/BOOT/BOOTX64.EFI"
#if !defined(ID_FAT32_DISK)
#define ID_FAT32_DISK   AROS_MAKE_ID('F','A','T',2)
#endif

extern BOOL FormatPartition(CONST_STRPTR device, CONST_STRPTR name, ULONG dostype);
#define EFI_ESP_MAXMB   512

static BOOL IsESPType(UBYTE id, struct DosEnvec *de)
{
    UQUAD blocks = (UQUAD)(de->de_HighCyl - de->de_LowCyl + 1)
        * de->de_Surfaces * de->de_BlocksPerTrack;
    UQUAD mb = (blocks * (de->de_SizeBlock << 2)) >> 20;

    return id == 0xef || ((id == 0x0b || id == 0x0c) && mb <= EFI_ESP_MAXMB);
}

/* The DOS device mounted on the ESP of device/unit: name into devName */
static BOOL FindESPDevice(STRPTR device, LONG unit, STRPTR devName, ULONG len)
{
    struct PartitionHandle *root, *partition;
    struct PartitionType type;
    struct DosEnvec esp = { 0 };
    struct DosList *dl;
    BOOL found = FALSE;

    if ((root = OpenRootPartition(device, unit)) == NULL)
        return FALSE;
    if (OpenPartitionTable(root) == 0)
    {
        ForeachNode(&root->table->list, partition)
        {
            struct DosEnvec de;

            GetPartitionAttrsTags(partition,
                PT_TYPE, (IPTR)&type, PT_DOSENVEC, (IPTR)&de, TAG_DONE);
            if (IsESPType(type.id[0], &de))
            {
                esp = de;
                found = TRUE;
                break;
            }
        }
        ClosePartitionTable(root);
    }
    CloseRootPartition(root);
    if (!found)
        return FALSE;

    found = FALSE;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)) != NULL)
    {
        struct FileSysStartupMsg *fssm =
            (struct FileSysStartupMsg *)BADDR(dl->dol_misc.dol_handler.dol_Startup);
        struct DosEnvec *de;

        if ((IPTR)fssm < 0x400 || fssm->fssm_Unit != unit
            || strcmp(AROS_BSTR_ADDR(fssm->fssm_Device), device) != 0)
            continue;
        de = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
        if (de->de_LowCyl == esp.de_LowCyl && de->de_HighCyl == esp.de_HighCyl)
        {
            snprintf(devName, len, "%s:", AROS_BSTR_ADDR(dl->dol_Name));
            found = TRUE;
            break;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return found;
}

/* Empty, or ours from an earlier install: never another system's loader */
static BOOL ESPIsUsable(CONST_STRPTR espRoot)
{
    TEXT path[128];
    struct InfoData *info;
    BOOL usable = FALSE;
    BPTR lock;

    if ((lock = Lock(espRoot, SHARED_LOCK)) == BNULL)
        return FALSE;
    if ((info = AllocMem(sizeof(*info), MEMF_ANY)) != NULL)
    {
        if (Info(lock, info) && info->id_DiskType != ID_NOT_REALLY_DOS
            && info->id_DiskType != ID_UNREADABLE_DISK)
        {
            BPTR efi, ours;

            snprintf(path, sizeof(path), "%sefi", espRoot);
            if ((efi = Lock(path, SHARED_LOCK)) == BNULL)
                usable = TRUE;
            else
            {
                UnLock(efi);
                snprintf(path, sizeof(path), "%sefi/BOOT/grub", espRoot);
                if ((ours = Lock(path, SHARED_LOCK)) != BNULL)
                {
                    UnLock(ours);
                    usable = TRUE;
                }
            }
        }
        FreeMem(info, sizeof(*info));
    }
    UnLock(lock);
    return usable;
}

static BOOL ESPIsFormatted(CONST_STRPTR espRoot)
{
    struct InfoData *info;
    BOOL formatted = FALSE;
    BPTR lock;

    if ((lock = Lock(espRoot, SHARED_LOCK)) == BNULL)
        return FALSE;
    if ((info = AllocMem(sizeof(*info), MEMF_ANY)) != NULL)
    {
        formatted = Info(lock, info) && info->id_DiskType != ID_NOT_REALLY_DOS
            && info->id_DiskType != ID_UNREADABLE_DISK;
        FreeMem(info, sizeof(*info));
    }
    UnLock(lock);
    return formatted;
}

/* UEFI firmware loads efi/BOOT/BOOTX64.EFI from the ESP. GRUB's root there is
   the ESP itself, so it also needs the kernel and boot packages beside it. */
void BOOTLOADER_InstallEFI(Class * CLASS, Object * self)
{
    struct InstallStage_DATA *data = INST_DATA(CLASS, self);
    ULONG srcLen = strlen(source_Path) + 32;
    TEXT src[srcLen], espRoot[64], dst[160];
    struct FileInfoBlock *fib;
    BPTR lock;

    strcpy(src, source_Path);
    AddPart(src, EFI_LOADER, srcLen);
    if ((lock = Lock(src, SHARED_LOCK)) == BNULL)
        return;
    UnLock(lock);

    if (!FindESPDevice(data->bl_TargetDevice, data->bl_TargetUnit,
        espRoot, sizeof(espRoot)))
    {
        D(bug("[InstallAROS] %s: no ESP on %s unit %ld\n", __func__,
            data->bl_TargetDevice, data->bl_TargetUnit));
        return;
    }
    D(bug("[InstallAROS] %s: ESP is %s\n", __func__, espRoot));

    if (!ESPIsFormatted(espRoot))
    {
        TEXT dev[64];

        strcpy(dev, espRoot);
        if (!FormatPartition(dev, "EFI", ID_FAT32_DISK))
        {
            D(bug("[InstallAROS] %s: formatting %s failed\n", __func__, espRoot));
            return;
        }
    }
    if (!ESPIsUsable(espRoot))
    {
        D(bug("[InstallAROS] %s: %s holds another loader - left alone\n",
            __func__, espRoot));
        return;
    }

    {
        CONST_STRPTR efi_dirs[] = {
            "efi/BOOT", "efi/BOOT",
            "boot/pc-smp", "boot/pc-smp",
            NULL
        };
        CopyDirArray(CLASS, self, source_Path, espRoot, efi_dirs, NULL);
    }

    /* boot/#?.pkg.xz */
    strcpy(src, source_Path);
    AddPart(src, "boot", srcLen);
    if ((lock = Lock(src, SHARED_LOCK)) != BNULL)
    {
        if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL)
        {
            if (Examine(lock, fib))
            {
                while (ExNext(lock, fib))
                {
                    ULONG n = strlen(fib->fib_FileName);
                    TEXT file[srcLen + n + 2];

                    if (fib->fib_DirEntryType > 0 || n < 7
                        || strcmp(fib->fib_FileName + n - 7, ".pkg.xz") != 0)
                        continue;
                    strcpy(file, src);
                    AddPart(file, fib->fib_FileName, sizeof(file));
                    snprintf(dst, sizeof(dst), "%sboot/%s", espRoot, fib->fib_FileName);
                    DoMethod(self, MUIM_IC_CopyFile, file, dst);
                }
            }
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(lock);
    }
}
