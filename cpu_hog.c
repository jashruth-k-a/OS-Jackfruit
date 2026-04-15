#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    printf("cpu_hog: spinning...\n");
    fflush(stdout);
    volatile unsigned long long x = 0;
    for (;;) {
        x++;
        if (x % (1ULL << 28) == 0) {
            printf("cpu_hog: tick %llu\n", x);
            fflush(stdout);
        }
    }
    return 0;
}
