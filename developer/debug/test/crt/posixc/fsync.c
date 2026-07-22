/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <aros/debug.h>
#include "test.h"

static const char test_dir[] = "T:__fsync_dir__";
static const char test_file[] = "T:__fsync_dir__/file";
static int fd = -1;

int main(void)
{
    bug("[FDATASYNC-TEST] 01 mkdir begin\n");
    TEST((mkdir(test_dir, 0700) == 0));
    bug("[FDATASYNC-TEST] 01 mkdir done\n");

    bug("[FDATASYNC-TEST] 02 directory open begin\n");
    fd = open(test_dir, O_RDONLY);
    TEST((fd >= 0));
    bug("[FDATASYNC-TEST] 02 directory open done fd=%d\n", fd);
    bug("[FDATASYNC-TEST] 03 directory fsync begin\n");
    TEST((fsync(fd) == 0));
    bug("[FDATASYNC-TEST] 03 directory fsync done\n");
    bug("[FDATASYNC-TEST] 04 directory fdatasync begin\n");
    TEST((fdatasync(fd) == 0));
    bug("[FDATASYNC-TEST] 04 directory fdatasync done\n");
    bug("[FDATASYNC-TEST] 05 directory close begin\n");
    TEST((close(fd) == 0));
    fd = -1;
    bug("[FDATASYNC-TEST] 05 directory close done\n");

    bug("[FDATASYNC-TEST] 06 writable open begin\n");
    fd = open(test_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    TEST((fd >= 0));
    bug("[FDATASYNC-TEST] 06 writable open done fd=%d\n", fd);
    bug("[FDATASYNC-TEST] 07 write begin\n");
    TEST((write(fd, "x", 1) == 1));
    bug("[FDATASYNC-TEST] 07 write done\n");
    bug("[FDATASYNC-TEST] 08 writable fsync begin\n");
    TEST((fsync(fd) == 0));
    bug("[FDATASYNC-TEST] 08 writable fsync done\n");
    bug("[FDATASYNC-TEST] 09 writable fdatasync begin\n");
    TEST((fdatasync(fd) == 0));
    bug("[FDATASYNC-TEST] 09 writable fdatasync done\n");
    bug("[FDATASYNC-TEST] 10 writable close begin\n");
    TEST((close(fd) == 0));
    fd = -1;
    bug("[FDATASYNC-TEST] 10 writable close done\n");

    bug("[FDATASYNC-TEST] 11 readonly open begin\n");
    fd = open(test_file, O_RDONLY);
    TEST((fd >= 0));
    bug("[FDATASYNC-TEST] 11 readonly open done fd=%d\n", fd);
    errno = 0;
    bug("[FDATASYNC-TEST] 12 readonly fsync begin\n");
    TEST((fsync(fd) == -1));
    TEST((errno == EBADF));
    bug("[FDATASYNC-TEST] 12 readonly fsync done errno=%d\n", errno);
    errno = 0;
    bug("[FDATASYNC-TEST] 13 readonly fdatasync begin\n");
    TEST((fdatasync(fd) == -1));
    TEST((errno == EBADF));
    bug("[FDATASYNC-TEST] 13 readonly fdatasync done errno=%d\n", errno);

    bug("[FDATASYNC-TEST] 14 cleanup begin\n");
    cleanup();
    bug("[FDATASYNC-TEST] 14 cleanup done; PASS\n");
    return OK;
}

void cleanup(void)
{
    if (fd >= 0)
        close(fd);
    unlink(test_file);
    rmdir(test_dir);
}
