#include "greeting.h"
const char *greeting(void) { return "Hello from native AROS!"; }
int sum_to(int n)
{
    int total = 0;
    for (int i = 1; i <= n; ++i) total += i;
    return total;
}
