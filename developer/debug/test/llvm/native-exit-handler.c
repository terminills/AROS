/* Check both main and atexit exit paths with the -nix startup wrapper.
 * Modes: 0 return 0; 1 exit(7); 2 _Exit(7); 3 atexit exit(7);
 *        4 atexit _Exit(7); 5 return 7.
 * Expected HANDLER output: every mode except 2; expected status: 0/7/7/7/7/7.
 */
#include <stdio.h>
#include <stdlib.h>

static int mode;

static void finish(void)
{
    puts("HANDLER");
    fflush(stdout);
    if (mode == 3)
        exit(7);
    if (mode == 4)
        _Exit(7);
}

int main(int argc, char **argv)
{
    mode = argc > 1 ? atoi(argv[1]) : 0;
    if (atexit(finish))
        return 99;
    puts("MAIN");
    fflush(stdout);
    if (mode == 1)
        exit(7);
    if (mode == 2)
        _Exit(7);
    return mode == 5 ? 7 : 0;
}
