/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test.h"

static const char test_dir[] = "T:__fsync_dir__";
static const char test_file[] = "T:__fsync_dir__/file";
static int fd = -1;

int main(void)
{
    TEST((mkdir(test_dir, 0700) == 0));

    fd = open(test_dir, O_RDONLY);
    TEST((fd >= 0));
    TEST((fsync(fd) == 0));
    TEST((close(fd) == 0));
    fd = -1;

    fd = open(test_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    TEST((fd >= 0));
    TEST((write(fd, "x", 1) == 1));
    TEST((fsync(fd) == 0));
    TEST((close(fd) == 0));
    fd = -1;

    fd = open(test_file, O_RDONLY);
    TEST((fd >= 0));
    errno = 0;
    TEST((fsync(fd) == -1));
    TEST((errno == EBADF));

    cleanup();
    return OK;
}

void cleanup(void)
{
    if (fd >= 0)
        close(fd);
    unlink(test_file);
    rmdir(test_dir);
}
