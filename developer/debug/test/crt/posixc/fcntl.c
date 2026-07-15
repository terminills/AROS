/*
    Copyright (C) 1995-2014, The AROS Development Team. All rights reserved.
*/

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "test.h"

int fd = -1;
int fd2 = -1;

int main()
{
    struct flock lock;

    fd = mkstemp("T:lseekXXXXXX");
    TEST((fd != -1));

    /* Check if F_GETFD and F_SETFD is working */
    TEST((fcntl(fd, F_GETFD) == 0));
    TEST((fcntl(fd, F_SETFD, FD_CLOEXEC) == 0));
    TEST((fcntl(fd, F_GETFD) == FD_CLOEXEC));
    TEST((fcntl(fd, F_SETFD, 0) == 0));
    TEST((fcntl(fd, F_GETFD) == 0));
    
    int fd2 = dup(fd);
    TEST((fd2 != -1));

    /* Check if descriptor flags are independent for duped descriptors */
    TEST((fcntl(fd2, F_GETFD) == 0));
    TEST((fcntl(fd2, F_SETFD, FD_CLOEXEC) == 0));
    TEST((fcntl(fd2, F_GETFD) == FD_CLOEXEC));
    TEST((fcntl(fd, F_GETFD) == 0));
    TEST((fcntl(fd, F_SETFD, FD_CLOEXEC) == 0));
    TEST((fcntl(fd2, F_SETFD, 0) == 0));
    TEST((fcntl(fd, F_GETFD) == FD_CLOEXEC));

    /* Exercise DOS record locking and the caller-coordinated fallback used
       when a filesystem handler does not implement ACTION_LOCK_RECORD. */
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    lock.l_pid = 0;
    TEST((fcntl(fd, F_SETLK, &lock) == 0));

    lock.l_type = F_UNLCK;
    TEST((fcntl(fd, F_SETLK, &lock) == 0));

    lock.l_type = F_WRLCK;
    TEST((fcntl(fd, F_SETLKW, &lock) == 0));

    lock.l_type = F_UNLCK;
    TEST((fcntl(fd, F_SETLK, &lock) == 0));
    
    cleanup();
    return OK;
}

void cleanup()
{
    if(fd != -1)
        close(fd);
    if(fd2 != -1)
        close(fd2);
}
