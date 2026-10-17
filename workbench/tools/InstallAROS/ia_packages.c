/*
    Copyright (C) 2018-2026, The AROS Development Team. All rights reserved.
*/

#include <aros/debug.h>
#include <proto/dos.h>
#include <string.h>
#include <strings.h>

#include "ia_locale.h"
#include "ia_install.h"
#include "ia_packages.h"
#include "ia_diskio.h"

extern char *source_Path;

#define PACKAGEDIR_BASE "Prefs/Env-Archive/SYS/Packages"
#define PACKAGE_PATH_MAX 256

/* Drawers the other install steps copy (or deliberately leave to an option) */
static CONST_STRPTR installedDrawers[] = {
    "boot", "C", "Classes", "Devs", "Fonts", "L", "Libs", "Locale", "Prefs",
    "Rexxc", "S", "Storage", "System", "Tools", "Utilities", "WBStartup",
    "Applications", "Demos", "Extras", "Developer",
    NULL
};

void PACKAGES_InitSupport(struct List *PACKAGELIST)
{
    D(bug("[InstallAROS] %s()\n", __func__));

    NEWLIST(PACKAGELIST);
}

void PACKAGES_AddCoreSkipPaths(struct List *SkipList)
{
    ULONG skipPathLen = strlen(source_Path) + 100;
    TEXT skipPath[skipPathLen];

    D(bug("[InstallAROS] %s()\n", __func__));

    /* Developer is registered by the developer files step */
    strcpy(skipPath, source_Path);
    AddPart(skipPath, "Prefs/Env-Archive/SYS/Packages/Developer", skipPathLen);
    AddSkipListEntry(SkipList, skipPath);
}

static BOOL packageCopiedElsewhere(CONST_STRPTR relPath)
{
    ULONG i, len;

    for (i = 0; installedDrawers[i] != NULL; i++)
    {
        len = strlen(installedDrawers[i]);
        if (strncasecmp(relPath, installedDrawers[i], len) == 0
            && (relPath[len] == '\0' || relPath[len] == '/'))
            return TRUE;
    }
    return FALSE;
}

/* Packages register their install directory in ENVARC:SYS/Packages/<name>.
   Those that live in a drawer of their own on SYS: (not inside one the other
   steps copy) are installed here, with their drawer icon. */
void PACKAGES_DoInstall(Class * CLASS, Object * self, CONST_STRPTR destinationPath)
{
    ULONG pkgDirLen = strlen(source_Path) + strlen(PACKAGEDIR_BASE) + 2;
    TEXT pkgDir[pkgDirLen];
    struct FileInfoBlock *fib;
    BPTR lock;

    D(bug("[InstallAROS] %s('%s')\n", __func__, destinationPath));

    strcpy(pkgDir, source_Path);
    AddPart(pkgDir, PACKAGEDIR_BASE, pkgDirLen);

    if ((lock = Lock(pkgDir, SHARED_LOCK)) == BNULL)
        return;

    if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL)
    {
        if (Examine(lock, fib))
        {
            while (ExNext(lock, fib))
            {
                ULONG varFileLen = pkgDirLen + strlen(fib->fib_FileName) + 2;
                TEXT varFile[varFileLen];
                TEXT value[PACKAGE_PATH_MAX];
                CONST_STRPTR relPath;
                BPTR fh;
                LONG end;

                if (fib->fib_DirEntryType > 0
                    || strcmp(fib->fib_FileName, "Developer") == 0)
                    continue;

                strcpy(varFile, pkgDir);
                AddPart(varFile, fib->fib_FileName, varFileLen);
                if ((fh = Open(varFile, MODE_OLDFILE)) == BNULL)
                    continue;
                value[0] = '\0';
                FGets(fh, value, sizeof(value));
                Close(fh);

                for (end = strlen(value) - 1;
                    end >= 0 && (value[end] == '\n' || value[end] == ' ');
                    end--)
                    value[end] = '\0';
                while (end > 4 && value[end] == '/')
                    value[end--] = '\0';

                if (strncasecmp(value, "SYS:", 4) != 0 || value[4] == '\0')
                    continue;
                relPath = value + 4;
                if (packageCopiedElsewhere(relPath))
                    continue;

                D(bug("[InstallAROS] %s: package '%s' -> '%s'\n", __func__,
                    fib->fib_FileName, relPath));
                {
                    CONST_STRPTR package_dirs[] = { relPath, relPath, NULL };
                    CopyDirArray(CLASS, self, source_Path, destinationPath,
                        package_dirs, NULL);
                }
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
}

/* The Workbench backdrop list, without the Live CD's installer icon */
void PACKAGES_InstallBackdrop(CONST_STRPTR destinationPath)
{
    ULONG srcLen = strlen(source_Path) + strlen(".backdrop") + 2;
    ULONG dstLen = strlen(destinationPath) + strlen(".backdrop") + 2;
    TEXT srcFile[srcLen], dstFile[dstLen], line[PACKAGE_PATH_MAX];
    BPTR in, out;

    strcpy(srcFile, source_Path);
    AddPart(srcFile, ".backdrop", srcLen);
    strcpy(dstFile, destinationPath);
    AddPart(dstFile, ".backdrop", dstLen);

    if ((out = Lock(dstFile, SHARED_LOCK)) != BNULL)
    {
        UnLock(out);
        return;
    }
    if ((in = Open(srcFile, MODE_OLDFILE)) == BNULL)
        return;
    if ((out = Open(dstFile, MODE_NEWFILE)) != BNULL)
    {
        while (FGets(in, line, sizeof(line)))
        {
            if (strncasecmp(line, ":Tools/InstallAROS", 18) == 0)
                continue;
            FPuts(out, line);
        }
        Close(out);
    }
    Close(in);
}
