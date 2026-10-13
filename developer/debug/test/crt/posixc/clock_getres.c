#include <errno.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    const clockid_t clocks[] = { CLOCK_REALTIME, CLOCK_MONOTONIC };
    struct timespec res;
    unsigned int i;

    for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i)
    {
        if (clock_getres(clocks[i], &res) != 0 ||
            res.tv_sec != 0 || res.tv_nsec != 1000 ||
            clock_getres(clocks[i], NULL) != 0)
            return 1;
    }
    res.tv_sec = 123;
    res.tv_nsec = 456;
    errno = 0;
    if (clock_getres((clockid_t)-1, &res) != -1 || errno != EINVAL ||
        res.tv_sec != 123 || res.tv_nsec != 456)
        return 2;
    errno = 0;
    if (clock_getres((clockid_t)-1, NULL) != -1 || errno != EINVAL)
        return 3;
    puts("CLOCK-GETRES-PASS");
    return 0;
}
