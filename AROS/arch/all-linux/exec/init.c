/*
    Copyright (C) 1995-1997 AROS - The Amiga Replacement OS
    $Id$

    Desc: Linux init code for emulated (Unix) systems.
    Lang: english
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/execbase.h>

#include <proto/exec.h>

#if defined(__linux__) && defined(__mc68000__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <memory.h>	/* From $(TOP)/rom/exec */

#include <stdlib.h>
#include <stdio.h>
#include <sys/termios.h>

extern const struct Resident
    Expansion_resident,
    Exec_resident,
    Utility_resident,
    Aros_resident,
    BOOPSI_resident,
    HIDD_resident,
    UnixIO_resident,
    Graphics_resident,
    Timer_resident,
    Battclock_resident,
    Keymap_resident,
    Input_resident,
    Intuition_resident,
    Console_resident,
    Mathffp_resident,
    Mathieeesingbas_resident,
    Dos_resident,
    LDDemon_resident,
    emul_handler_resident,
    boot_resident;



/* This list MUST be in the correct order (priority). */
static const struct Resident *romtagList[] =
{
    &Expansion_resident,		    /* SingleTask,  110  */
    &Exec_resident,			    /* SingleTask,  105  */
    &Utility_resident,			    /* ColdStart,   103  */
    &Aros_resident,			    /* ColdStart,   102  */
    &Mathieeesingbas_resident,              /* ColdStart,   101  */
    &BOOPSI_resident,			    /* ColdStart,   95	 */
    &HIDD_resident,			    /* ColdStart,   92	 */
    &UnixIO_resident,			    /* ColdStart,   91	 */
    &Graphics_resident, 		    /* ColdStart,   65	 */
    &Timer_resident,			    /* ColdStart,   50	 */
    &Battclock_resident,		    /* ColdStart,   45	 */
    &Keymap_resident,			    /* ColdStart,   40	 */
    &Input_resident,			    /* ColdStart,   30	 */
    &Intuition_resident,		    /* ColdStart,   10	 */
    &Console_resident,			    /* ColdStart,   5	 */
    &emul_handler_resident,		    /* ColdStart,   0	 */
    &Mathffp_resident,			    /* ColdStart,  -120  */

    /*
	NOTE: You must not put anything between these two; the code which
	initialized boot_resident will directly call Dos_resident and
	anything between the two will be skipped.
    */
    &boot_resident,			    /* ColdStart,  -50	 */
    &Dos_resident,			    /* None,	   -120  */
    &LDDemon_resident,			    /* AfterDOS,   -125  */
    NULL
};

/* So we can examine the memory */
struct MemHeader *mh;
UBYTE *memory, *space;
int memSize = 2;

extern void InitCore(void);
extern struct ExecBase *PrepareExecBase(struct MemHeader *mh);

/*
    This is where AROS is first called by whatever system loaded it,
    either some kind of boot loader, or a "parent" operating system.

    For boot loaded $(ARCH), you don't need to define main() like this,
    you can have it anyway your bootloader likes.
*/

int main(int argc, char **argv)
{
    /*	Well, if you want you can take in command line arguments here,
	but that is not necessary, or perhaps rather complex...

	eg: say you wished to allow people to specify the root directory
	    arosshell --rootdir /usr/local/AROS --memsize 4

	For an example, you could ask how much memory you want for the
	system, chip/fast whatever...
    */

    struct ExecBase *SysBase;
    struct termios t;
    int psize = 0;

    /*
    First up, set up the memory.

    If your memory starts at 0 (I think Linux does, FreeBSD doesn't),
    then you can allocate 4K at that address, and do whatever you want
    to make that invalid to trap NULL dereference errors.

    */
#if defined(__linux__) && defined(__mc68000__)
    psize = getpagesize();
    space = mmap((APTR)0, (memSize << 20), PROT_READ|PROT_WRITE,
		 MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0);
    if (space != (UBYTE *)-1)
    {
	int size = psize/sizeof(ULONG);
	memory = (UBYTE *)((ULONG)space + psize);
	while(--size)
	    *((ULONG *)space)++ = 0xDEADBEEF;
    }
    else
    {
	perror("mmap");
	exit(20);
    }
#else
    /* We allocate memSize megabytes */
    memory = malloc((memSize << 20));
    if( !memory )
    {
	 /*fprintf(stderr, "Cannot allocate any memory!\n");*/
	 exit(20);
    }
#endif

    /* Prepare the first mem header */
    mh = (struct MemHeader *)memory;
    mh->mh_Node.ln_Type = NT_MEMORY;
    mh->mh_Node.ln_Name = "chip memory";
    mh->mh_Node.ln_Pri = -5;
    mh->mh_Attributes = MEMF_CHIP | MEMF_PUBLIC | MEMF_LOCAL |
			MEMF_24BITDMA | MEMF_KICK;
    mh->mh_First = (struct MemChunk *)((UBYTE *)mh+MEMHEADER_TOTAL);
    mh->mh_First->mc_Next = NULL;
    mh->mh_First->mc_Bytes = (memSize << 20) - MEMHEADER_TOTAL - psize;
    mh->mh_Lower = mh->mh_First;
    mh->mh_Upper = (APTR)(memory + (memSize << 20) - psize);
    mh->mh_Free = mh->mh_First->mc_Bytes;

    /*
	This will prepare enough of ExecBase to allow us to
	call functions, it will also set up the memory list.
    */
    SysBase = PrepareExecBase(mh);

    /* Ok, lets start up the kernel, we are probably using the UNIX
       kernel, or a variant of that (see config/unix).
    */
    InitCore();

    /* On Linux/m68k where we can run old Amiga binaries, we should
       put SysBase at location 4. On other systems, DON'T DO THIS.
    */
#if defined(__linux__) && defined(__mc68000__)
    *(APTR *)4 = SysBase;
    if (mprotect((APTR)0, psize, PROT_READ))
    {
	perror("mprotect");
	exit(10);
    }
#endif

    /* We might also be interested in using the BS key instead of the
       delete key, this will do that
    */
    tcgetattr(0, &t);
    t.c_cc[VERASE] = '\b';
#ifndef TCSASOFT
#   define TCSASOFT 0
#endif
    tcsetattr(0, TCSANOW|TCSASOFT, &t);

    /*	There is nothing more system dependant to set up,
	so lets start that ball rolling...

	The InitCode() call should never return in a working system.
    */
    SysBase->ResModules = romtagList;
    InitCode(RTF_SINGLETASK, 0);
    fprintf(stderr,"Returned from InitCode()\n");
    return 1;
}
