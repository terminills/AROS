/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    __posixc_fd_export/__posixc_fd_import/__posixc_fd_release: an open file
    passed to a separately started program, which gets a descriptor of its
    own for the same open file.
*/

#include <proto/dos.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "test.h"

void *__posixc_fd_export(int file_descriptor);
int __posixc_fd_import(void *token, int fdflags);
void __posixc_fd_release(void *token);

#define TESTFILE "RAM:fdtransfer.t"

int main(void)
{
    char cmd[384], buf[16];
    void *token;
    int fd;

    fd = open(TESTFILE, O_CREAT | O_TRUNC | O_RDWR, 0644);
    TEST((fd >= 0));
    TEST((write(fd, "abc", 3) == 3));

    /* The slave imports the token into its own table and appends "def".
       Both share one open file, so it writes at our offset (3). */
    token = __posixc_fd_export(fd);
    TEST((token != NULL));
    /* PROGDIR: in a SystemTags() command is the new shell's, not ours, so
       name the slave by our own program directory. */
    {
        char dir[256];
        TEST((NameFromLock(GetProgramDir(), (STRPTR)dir, sizeof(dir))));
        TEST((AddPart((STRPTR)dir, (CONST_STRPTR)"fdtransfer_slave", sizeof(dir))));
        snprintf(cmd, sizeof(cmd), "\"%s\" %p", dir, token);
    }
    TEST((SystemTags((CONST_STRPTR)cmd, TAG_DONE) == 0));

    /* Our descriptor is unaffected by the slave closing its own. */
    TEST((lseek(fd, 0, SEEK_CUR) == 6));
    TEST((lseek(fd, 0, SEEK_SET) == 0));
    memset(buf, 0, sizeof(buf));
    TEST((read(fd, buf, sizeof(buf) - 1) == 6));
    TEST((strcmp(buf, "abcdef") == 0));

    /* A token that is never imported is released without closing the file
       under the descriptor that is still open. */
    token = __posixc_fd_export(fd);
    TEST((token != NULL));
    __posixc_fd_release(token);
    TEST((lseek(fd, 0, SEEK_SET) == 0));
    TEST((read(fd, buf, 3) == 3));

    /* Importing in the same process gives a second descriptor on the file. */
    token = __posixc_fd_export(fd);
    {
        int fd2 = __posixc_fd_import(token, 0);
        TEST((fd2 >= 0 && fd2 != fd));
        TEST((close(fd2) == 0));
    }
    TEST((lseek(fd, 0, SEEK_SET) == 0));

    errno = 0;
    TEST((__posixc_fd_export(-1) == NULL && errno == EBADF));
    errno = 0;
    TEST((__posixc_fd_export(999) == NULL && errno == EBADF));

    TEST((close(fd) == 0));
    unlink(TESTFILE);
    printf("fdtransfer: all tests passed\n");
    return OK;
}

void cleanup(void)
{
    unlink(TESTFILE);
}
