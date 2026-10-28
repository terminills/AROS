/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <sys/ioctl.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <proto/dos.h>

static int checkconsole(const char *path, int raw)
{
    struct winsize w = {0, 0, 7, 7};
    struct termios t;
    struct FileHandle *h;
    long fh = 0;
    char typed[8] = "";
    int fd = open(path, O_RDWR);

    if (fd < 0)
    {
        printf("FAIL: open %s errno=%d\n", path, errno);
        return 1;
    }
    if (raw)
    {
        tcgetattr(fd, &t);
        t.c_lflag &= ~ICANON;
        tcsetattr(fd, TCSANOW, &t);
    }
    /* Input waiting before the query, as if typed, must still be read after it */
    __get_default_file(fd, &fh);
    h = BADDR((BPTR)fh);
    if (!DoPkt(h->fh_Type, ACTION_STACK, (SIPTR)h->fh_Arg1, (SIPTR)"keep", 4, 0, 0))
    {
        printf("FAIL: %s ACTION_STACK error %ld\n", path, (long)IoErr());
        close(fd);
        return 1;
    }
    if (ioctl(fd, TIOCGWINSZ, &w) != 0 || w.ws_row == 0 || w.ws_col == 0 ||
        w.ws_xpixel != 0 || w.ws_ypixel != 0)
    {
        printf("FAIL: %s TIOCGWINSZ errno=%d rows %d cols %d\n", path, errno, w.ws_row, w.ws_col);
        close(fd);
        return 1;
    }
    tcgetattr(fd, &t);
    if (!(t.c_lflag & ICANON) != !!raw)
    {
        printf("FAIL: %s mode changed by TIOCGWINSZ\n", path);
        close(fd);
        return 1;
    }
    if (read(fd, typed, sizeof(typed) - 1) != 4 || strcmp(typed, "keep"))
    {
        printf("FAIL: %s input before TIOCGWINSZ read back as \"%s\"\n", path, typed);
        close(fd);
        return 1;
    }
    printf("PASS: %s %s: %d rows, %d columns\n", path, raw ? "raw" : "cooked", w.ws_row, w.ws_col);
    close(fd);
    return 0;
}

int main(void)
{
    struct winsize w = {1, 2, 3, 4};
    int fd = -1, failed = 0, error = 0;
#define CHECK(x) do { if (!(x)) { failed = __LINE__; error = errno; goto done; } } while (0)
    CHECK(ioctl(-1, TIOCGWINSZ, &w) == -1 && errno == EBADF);
    fd = open("RAM:ioctl-tty-test.tmp", O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(ioctl(fd, TIOCGWINSZ, &w) == -1 && errno == ENOTTY);
    CHECK(ioctl(fd, 123, &w) == -1 && errno == ENOTTY);
    CHECK(w.ws_row == 1 && w.ws_col == 2 && w.ws_xpixel == 3 && w.ws_ypixel == 4);
    CHECK(ioctl(fd, TIOCGWINSZ, NULL) == -1 && errno == EFAULT);
    failed = checkconsole("CON:0/0/640/200/ioctl_tty/CLOSE", 0) ? -1 : 0;
    if (checkconsole("CON:0/0/320/120/ioctl_tty raw/CLOSE", 1))
        failed = -1;
done:
    if (fd >= 0)
    {
        if (close(fd)) { failed = __LINE__; error = errno; }
        if (unlink("RAM:ioctl-tty-test.tmp")) { failed = __LINE__; error = errno; }
    }
    if (failed) printf("IOCTL TTY FAIL line=%d errno=%d\n", failed, error);
    else puts("IOCTL TTY PASS");
    return failed ? 20 : 0;
}
