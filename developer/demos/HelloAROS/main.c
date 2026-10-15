#include <stdio.h>
#include "greeting.h"

int main(void)
{
    printf("%s\n", greeting());
    printf("Built and linked natively on AROS: sum(1..100) = %d\n", sum_to(100));
    return sum_to(100) == 5050 ? 0 : 20;
}
