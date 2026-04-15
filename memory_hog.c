#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int   chunk_mib  = (argc >= 2) ? atoi(argv[1]) : 16;
    int   sleep_ms   = (argc >= 3) ? atoi(argv[2]) : 500;
    int   total_mib  = 0;
    char *ptrs[1024];
    int   n = 0;

    printf("memory_hog: allocating %d MiB every %d ms\n", chunk_mib, sleep_ms);
    fflush(stdout);

    while (n < 1024) {
        size_t sz = (size_t)chunk_mib << 20;
        ptrs[n] = malloc(sz);
        if (!ptrs[n]) break;
        memset(ptrs[n], 0xAA, sz);   /* touch pages so they count as RSS */
        total_mib += chunk_mib;
        printf("memory_hog: total RSS ~%d MiB\n", total_mib);
        fflush(stdout);
        n++;
        usleep((useconds_t)sleep_ms * 1000);
    }
    printf("memory_hog: done (holding memory)\n");
    pause();
    return 0;
}

