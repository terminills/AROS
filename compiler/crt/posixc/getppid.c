/*
    Copyright (C) 2004-2026, The AROS Development Team. All rights reserved.
*/

#include <sys/types.h>

#include <exec/tasks.h>
#include <proto/exec.h>

#include <assert.h>

#include "__vfork.h"
#include "__posixc_intbase.h"

/*****************************************************************************

    NAME */
#include <unistd.h>

        pid_t getppid(

/*  SYNOPSIS */
        void)

/*  FUNCTION
        Returns the Parent process ID of the calling processes.

    INPUTS

    RESULT

    NOTES

    EXAMPLE

    BUGS

    SEE ALSO

    INTERNALS

******************************************************************************/
{
  struct vfork_data *udata = __posixc_pretending();
  struct Task *ParentTask;
  struct ETask *eThisTask;
  struct ETask *et;

  if (udata)
    eThisTask = GetETask(udata->child);
  else
    eThisTask = GetETask(FindTask(NULL));
  assert(eThisTask);
  ParentTask = (struct Task *)eThisTask->et_Parent;
  if(!ParentTask)
    return (pid_t) 1;
  et = GetETask(ParentTask);
  assert(et);
  return (pid_t) et->et_UniqueID;
}

