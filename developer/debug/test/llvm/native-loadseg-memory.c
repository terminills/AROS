#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load only: this separates loader retention from program initialization. */
int main(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[2], "valid") && strcmp(argv[2], "invalid")))
    {
        fprintf(stderr, "usage: %s file valid|invalid repetitions max-total-loss\n", argv[0]);
        return 2;
    }
    int valid = !strcmp(argv[2], "valid");
    int repetitions = atoi(argv[3]);
    long long limit = strtoll(argv[4], NULL, 10);
    if (repetitions < 1 || repetitions > 100)
        return 2;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("LOADSEG-MEMORY mode=%s repetitions=%d\n", argv[2], repetitions);
    unsigned long long initial = AvailMem(MEMF_ANY);
    for (int i = 0; i < repetitions; ++i)
    {
        unsigned long long before = AvailMem(MEMF_ANY);
        BPTR segment = LoadSeg((CONST_STRPTR)argv[1]);
        LONG error = IoErr();
        if (segment)
            UnLoadSeg(segment);
        unsigned long long after = AvailMem(MEMF_ANY);
        printf("iteration=%d loaded=%d ioerr=%ld retained=%lld\n",
               i, segment != 0, (long)error,
               (long long)before - (long long)after);
        if ((segment != 0) != valid)
            return 3;
    }
    long long retained = (long long)initial - (long long)AvailMem(MEMF_ANY);
    printf("LOADSEG-MEMORY-TOTAL retained=%lld limit=%lld\n", retained, limit);
    if (limit >= 0 && retained > limit)
        return 4;
    puts("LOADSEG-MEMORY-PASS");
    return 0;
}
