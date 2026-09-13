/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Does OpenDiskFont() give this system an OUTLINE font, and is it
          usable on a fixed cell grid?

    Written for consoleng.device's OSC 50 font switch, which silently gives up
    when OpenDiskFont() returns NULL or a font flagged FPF_PROPORTIONAL. On
    native pc-x86_64 every glyph above Latin-1 came back as reason code '2'
    (CONSOLENG_GLYPH_NOOTAG: the RastPort font is still a bitmap font), while
    the same stream on linux-hosted rendered DejaVu. The device cannot say
    WHICH of the two silent exits it took, so this asks diskfont the same
    question from a plain program and prints every field that decides it.

    What it found (2026-09-13): the Live CD's FONTS: is a multi-assign
    (RAM:Fonts + SYS:Fonts) and dos.library's Open() read files found in the
    second member through the first member's handler (fs_Open() claimed the
    handle on a rejected open), so freetype got 0 bytes of every .ttf.
    Fixed in rom/dos/{fs_driver,open}.c; the walk below is what exposed it.

    Usage: otagfontopen [fontname[.font]] [ysize]
           default DejaVuSansMono.font 16
*/

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <graphics/text.h>
#include <diskfont/diskfont.h>

#include <dos/exall.h>
#include <utility/tagitem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
    When OpenDiskFont() fails, replay the DOS calls diskfont makes to reach an
    outline font, one at a time, and print IoErr() after each. diskfont's own
    D(bug()) is compiled out and the failure surfaced with IoErr=211
    (ERROR_INVALID_LOCK) on the Live CD, which dos.library only raises for a
    NULL lock/filehandle - so SOME step below hands the next one a NULL.
*/
static void walk(const char *base)
{
    BPTR fontsdir, olddir, fh;
    char name[300];
    struct ExAllControl *eac;
    struct ExAllData *ead;
    LONG calls = 0, entries = 0, fontfiles = 0, seen = 0, more;
    UBYTE hdr[8];
    LONG got;

    fontsdir = Lock("FONTS:", ACCESS_READ);
    printf("OTAGFONTOPEN: walk Lock(\"FONTS:\") -> %p IoErr=%ld\n",
           BADDR(fontsdir), (long)IoErr());
    if (!fontsdir) return;

    /* GetFileList(): ExAll(dirlock, 1024 bytes, ED_DATE) until NO_MORE_ENTRIES */
    eac = AllocDosObject(DOS_EXALLCONTROL, NULL);
    ead = AllocMem(1024, MEMF_ANY);
    if (eac && ead)
    {
        eac->eac_LastKey = 0;
        do
        {
            struct ExAllData *it;
            more = ExAll(fontsdir, ead, 1024, ED_DATE, eac);
            calls++;
            if (!more && IoErr() != ERROR_NO_MORE_ENTRIES)
            {
                printf("OTAGFONTOPEN: walk ExAll call %ld FAILED IoErr=%ld "
                       "(diskfont drops the whole FONTS: dir on this)\n",
                       (long)calls, (long)IoErr());
                break;
            }
            for (it = eac->eac_Entries ? ead : NULL; it; it = it->ed_Next)
            {
                size_t l = strlen(it->ed_Name);
                entries++;
                if (l >= 5 && strcmp(it->ed_Name + l - 5, ".font") == 0)
                {
                    fontfiles++;
                    snprintf(name, sizeof(name), "%s.font", base);
                    if (strcmp(it->ed_Name, name) == 0)
                        seen = 1;
                }
            }
        } while (more);
        printf("OTAGFONTOPEN: walk ExAll: %ld calls, %ld entries, %ld .font, "
               "%s.font %s, final IoErr=%ld\n",
               (long)calls, (long)entries, (long)fontfiles, base,
               seen ? "SEEN" : "NOT SEEN", (long)IoErr());
    }
    if (ead) FreeMem(ead, 1024);
    if (eac) FreeDosObject(DOS_EXALLCONTROL, eac);

    /* DF_IteratorRememberOpen(): CurrentDir(DirLock) then relative opens */
    olddir = CurrentDir(fontsdir);

    snprintf(name, sizeof(name), "%s.font", base);
    fh = Open(name, MODE_OLDFILE);
    got = fh ? Read(fh, hdr, 4) : -1;
    printf("OTAGFONTOPEN: walk Open(\"%s\") -> %p read=%ld id=%02x%02x IoErr=%ld\n",
           name, BADDR(fh), (long)got, hdr[0], hdr[1], (long)IoErr());
    if (fh) Close(fh);

    /* OTAG_GetFile(): Open + ExamineFH + Read(whole) */
    snprintf(name, sizeof(name), "%s.otag", base);
    fh = Open(name, MODE_OLDFILE);
    if (fh)
    {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        BOOL ok = fib ? ExamineFH(fh, fib) : FALSE;
        LONG size = ok ? fib->fib_Size : -1;
        UBYTE *buf = (size > 0) ? AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR) : NULL;
        got = buf ? Read(fh, buf, size) : -1;
        printf("OTAGFONTOPEN: walk Open(\"%s\") -> %p ExamineFH=%d size=%ld "
               "read=%ld ident=%02x%02x%02x%02x IoErr=%ld\n",
               name, BADDR(fh), ok, (long)size, (long)got,
               buf ? buf[0] : 0, buf ? buf[1] : 0, buf ? buf[2] : 0,
               buf ? buf[3] : 0, (long)IoErr());
        if (buf) FreeVec(buf);
        if (fib) FreeDosObject(DOS_FIB, fib);
        Close(fh);
    }
    else
        printf("OTAGFONTOPEN: walk Open(\"%s\") FAILED IoErr=%ld\n",
               name, (long)IoErr());

    /* The engine then opens OT_FontFile from the otag (absolute path) */
    snprintf(name, sizeof(name), "Fonts:TrueType/%s.ttf", base);
    fh = Open(name, MODE_OLDFILE);
    got = fh ? Read(fh, hdr, 4) : -1;
    printf("OTAGFONTOPEN: walk Open(\"%s\") -> %p read=%ld IoErr=%ld\n",
           name, BADDR(fh), (long)got, (long)IoErr());
    if (fh) Close(fh);

    CurrentDir(olddir);
    UnLock(fontsdir);
}

int main(int argc, char **argv)
{
    struct Library *DiskfontBase;
    struct TextAttr ta;
    struct TextFont *tf;
    char fullname[256];
    const char *name = (argc > 1) ? argv[1] : "DejaVuSansMono";
    LONG ysize = (argc > 2) ? atol(argv[2]) : 16;
    size_t n = strlen(name);
    int rc = RETURN_FAIL;

    strncpy(fullname, name, sizeof(fullname) - 8);
    fullname[sizeof(fullname) - 8] = '\0';
    if (n < 5 || strcasecmp(fullname + n - 5, ".font") != 0)
        strcat(fullname, ".font");

    DiskfontBase = OpenLibrary("diskfont.library", 0);
    if (!DiskfontBase)
    {
        printf("OTAGFONTOPEN: FAIL diskfont.library would not open\n");
        return RETURN_FAIL;
    }

    ta.ta_Name  = fullname;
    ta.ta_YSize = ysize;
    ta.ta_Style = FS_NORMAL;
    ta.ta_Flags = 0;

    tf = OpenDiskFont(&ta);
    printf("OTAGFONTOPEN: OpenDiskFont(\"%s\", %ld) -> %p  IoErr=%ld\n",
           fullname, (long)ysize, tf, (long)IoErr());

    if (tf)
    {
        printf("OTAGFONTOPEN: tf_YSize=%u tf_XSize=%u tf_Baseline=%u "
               "tf_Flags=0x%02x%s%s%s tf_Style=0x%02x tf_LoChar=%u tf_HiChar=%u\n",
               tf->tf_YSize, tf->tf_XSize, tf->tf_Baseline,
               tf->tf_Flags,
               (tf->tf_Flags & FPF_PROPORTIONAL) ? " PROPORTIONAL" : "",
               (tf->tf_Flags & FPF_DISKFONT)     ? " DISKFONT"     : "",
               (tf->tf_Flags & FPF_ROMFONT)      ? " ROMFONT"      : "",
               tf->tf_Style, tf->tf_LoChar, tf->tf_HiChar);

        /* Same verdict consoleng's C_SET_FONT reaches, spelled out. */
        if (tf->tf_Flags & FPF_PROPORTIONAL)
            printf("OTAGFONTOPEN: FAIL font is FPF_PROPORTIONAL - consoleng "
                   "would close it and stay on the bitmap font\n");
        else
        {
            printf("OTAGFONTOPEN: PASS fixed-width outline font usable on a "
                   "cell grid\n");
            rc = RETURN_OK;
        }
        CloseFont(tf);
    }
    else
    {
        BPTR lock;
        char path[300];

        printf("OTAGFONTOPEN: FAIL OpenDiskFont returned NULL - consoleng "
               "would stay on the bitmap font\n");

        /* Is it the font files, or the engine behind them? */
        strcpy(fullname + strlen(fullname) - 5, ".otag");
        snprintf(path, sizeof(path), "FONTS:%s", fullname);
        lock = Lock(path, ACCESS_READ);
        printf("OTAGFONTOPEN:   %s %s\n", path, lock ? "found" : "NOT FOUND");
        if (lock) UnLock(lock);

        {
            struct Library *ft = OpenLibrary("freetype2.library", 0);
            printf("OTAGFONTOPEN:   freetype2.library %s\n",
                   ft ? "opens" : "does NOT open");
            if (ft) CloseLibrary(ft);
        }

        /* Same font by full path: diskfont then takes its DF_FILEDATA route
           (Lock + ParentDir) and never scans FONTS: with ExAll. If THIS
           works, the directory scan is what fails on this volume. */
        strcpy(fullname + strlen(fullname) - 5, ".font");
        snprintf(path, sizeof(path), "FONTS:%s", fullname);
        ta.ta_Name = path;
        tf = OpenDiskFont(&ta);
        printf("OTAGFONTOPEN: OpenDiskFont(\"%s\", %ld) -> %p  IoErr=%ld%s\n",
               path, (long)ysize, tf, (long)IoErr(),
               tf ? "  (full path WORKS: FONTS: scan is the failing half)" : "");
        if (tf) CloseFont(tf);

        fullname[strlen(fullname) - 5] = '\0';
        walk(fullname);
    }

    CloseLibrary(DiskfontBase);
    return rc;
}
