/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: CrashReporter annotations — how a running program hands
          CrashReporter the state it wants in its crash report (the page a
          browser was showing, the file an editor had open, ...).  See
          DESIGN.md §6 and SoftwareCenter PROTOCOL.md §9.5.

          The program keeps a CrashAnnotationBlock in MEMF_PUBLIC memory
          and registers it with AddPort(): the embedded MsgPort, named
          CRA_PORTNAME, is only ever used as the registry entry (no
          messages are exchanged).  Entries are fixed-size key/value
          strings the program rewrites in place at any time, from any of
          its tasks, without locking: a torn value is worth more than a
          lock a crashing task might hold.  When one of the program's
          tasks (the registering task or, via ETask et_Parent, any task it
          created) reaches Alert(), CrashReporter copies the live entries
          into the record and they appear in the report as an
          [Annotations] section, "key: value" per line.

          The layout is the contract; a program that does not build
          against this header (Chromium mirrors it) must reproduce it
          exactly, and only ever append fields in later versions.

    AI_PHASE: CRASH_REPORTER
    AI_STATUS: IMPLEMENTED
    AI_COMPLEXITY: LOW
*/

#ifndef CRASHREPORTER_ANNOTATIONS_H
#define CRASHREPORTER_ANNOTATIONS_H

#include <exec/types.h>
#include <exec/ports.h>

#define CRA_PORTNAME    "CrashReporter.annotations"
#define CRA_MAGIC       0x43524131          /* 'CRA1'                        */
#define CRA_VERSION     1

#define CRA_KEYSIZE     40                  /* incl. NUL                      */
#define CRA_VALUESIZE   128                 /* incl. NUL                      */
#define CRA_PRODUCTLEN  96

struct CrashAnnotationEntry
{
    TEXT    cae_Key[CRA_KEYSIZE];           /* "" = free slot                 */
    TEXT    cae_Value[CRA_VALUESIZE];
};

struct CrashAnnotationBlock
{
    struct MsgPort  cab_Port;               /* ln_Name = CRA_PORTNAME, PA_IGNORE */
    ULONG           cab_Magic;              /* CRA_MAGIC                      */
    ULONG           cab_Version;            /* CRA_VERSION                    */
    ULONG           cab_Size;               /* whole block, bytes             */
    ULONG           cab_Entries;            /* slots in cab_Entry[]           */
    ULONG           cab_KeySize;            /* CRA_KEYSIZE                    */
    ULONG           cab_ValueSize;          /* CRA_VALUESIZE                  */
    struct Task    *cab_Owner;              /* task that registered the block */
    APTR            cab_Reserved;
    TEXT            cab_Product[CRA_PRODUCTLEN]; /* "Chromium 124.0.6367.0"   */
    struct CrashAnnotationEntry cab_Entry[0];
};

#define CRA_BLOCKSIZE(n) (sizeof(struct CrashAnnotationBlock) + (n) * sizeof(struct CrashAnnotationEntry))

#endif /* CRASHREPORTER_ANNOTATIONS_H */
