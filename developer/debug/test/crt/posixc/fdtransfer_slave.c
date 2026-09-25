/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Helper for fdtransfer: imports the token given on the command line and
    writes "def" through its own descriptor. Returns 0 on success.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int __posixc_fd_import(void *token, int fdflags);

int main(int argc, char **argv)
{
    void *token;
    int fd;

    if (argc != 2 || sscanf(argv[1], "%p", &token) != 1)
        return 20;

    fd = __posixc_fd_import(token, 0);
    if (fd < 0)
        return 10;
    if (write(fd, "def", 3) != 3)
        return 10;
    if (close(fd) != 0)
        return 10;
    return 0;
}
