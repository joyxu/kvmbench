#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

volatile uint32_t running = 0;
int opt_pinned = 1;

volatile struct {
    uint64_t result;
    int is_alu;
} *results;

static void *thread_alu(void *opaque)
{
    uint64_t idx = (unsigned long)opaque;
    uint64_t d = idx;
    uint64_t loops = 0;

    while (!running);

    while (1) {
        d = d * 11;
        asm("" : : "r"(d));
        loops++;
        if (!(loops & 0xffff)) {
            results[idx].result = loops;
        }
    }

    return (void*)d;
}

static void *thread_fpu(void *opaque)
{
    uint64_t idx = (unsigned long)opaque;
    double f = (double)idx;
    uint64_t loops = 0;

    while (!running);

    while (1) {
        int i;

        for (i = 0; i < 16; i++) {
            f = f * 1.01;
            asm("" : : "x"(f));
        }
        loops += 16;
        if (!(loops & 0xffff)) {
            results[idx].result = loops;
        }
    }

    return (void*)(unsigned long)f;
}

int pin_thread(pthread_t t, int core_id) {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return pthread_setaffinity_np(t, sizeof(cpu_set_t), &cpuset);
}

static void spawn_thread(pthread_t *pt, char *spawned, int is_alu, uintptr_t idx)
{
    results[idx].is_alu = is_alu;

    pthread_create(pt, NULL, is_alu ? thread_alu : thread_fpu, (void*)idx);
    if (opt_pinned) {
        pin_thread(*pt, idx);
    }
    *spawned = 1;
}

int main(int argc, char **argv)
{
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    int i;
    char thread_spawned[num_cores];
    pthread_t thread[num_cores];
    uint64_t result = 0;
    char thread_siblings[1024];
    char *tsp;
    int is_alu = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-P")) {
            opt_pinned = 0;
        } else {
            printf("Syntax: %s\n\n", argv[0]);
            printf("  -P    Don't pin the threads\n");
            printf("\n");
            exit(0);
        }
    }

    results = malloc(num_cores * sizeof(*results));
    memset((void*)results, 0, num_cores * sizeof(*results));
    memset(thread_spawned, 0, num_cores);

#if 1

    for (i = 0; i < num_cores; i++) {
        char path[512];
        int fd;
        int offset = 0;

        if (thread_spawned[i]) continue;

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings", i);

        memset(thread_siblings, 0, sizeof(thread_siblings));
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("Error opening %s\n", path);
            return 1;
        }
        read(fd, thread_siblings, sizeof(thread_siblings));
        /* Just hope we read everything */
        close(fd);

        tsp = thread_siblings + strlen(thread_siblings) - 9;
        while (1) {
            uint32_t cur_siblings;
            int j;

            if (sscanf(tsp, "%08x", &cur_siblings) != 1) {
                printf("Error reading siblings from %s\n", tsp);
                exit(1);
            }

            for (j = 0; j < 32; j++) {
                if (cur_siblings & (1U << j)) {
                    int cpu = j + offset;
                    spawn_thread(&thread[cpu], &thread_spawned[cpu], is_alu, cpu);
                    is_alu = (is_alu + 1) & 1;
                }
            }

            if (tsp == thread_siblings) {
                break;
            }

            tsp -= 9;
            offset += 32;
        }
    }
#else
    for (i = 0; i < num_cores; i++) {
        int thread0 = i;
        int thread1 = i + 1;

        if (thread_spawned[i]) continue;

        spawn_thread(&thread[thread0], &thread_spawned[thread0], 1, thread0);
        spawn_thread(&thread[thread1], &thread_spawned[thread1], 0, thread1);
    }
#endif

    if (opt_pinned) {
        printf("Running FPU test on CPUs ");
        for (i = 0; i < num_cores; i++) {
            if (!results[i].is_alu) {
                printf("%d ", i);
            }
        }
        printf("\n");

        printf("Running ALU test on CPUs ");
        for (i = 0; i < num_cores; i++) {
            if (results[i].is_alu) {
                printf("%d ", i);
            }
        }
        printf("\n");
    } else {
        printf("Running ALU and FPU tests without pinning ...\n");
    }

    running = 1;
    sleep(5);

    for (i = 0; i < num_cores; i++) {
        result += results[i].result;
    }

    printf("Total MOps: %ld\n", result / 1000000 / 5);

    return 0;
}
