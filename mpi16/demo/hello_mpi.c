// Every rank reports where it runs; rank 0 prints one line per rank.
#define _GNU_SOURCE
#include <mpi.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char host[32];
    int cpu;
    int hart;
} where_t;

// The guest's hart ids are 0-3 locally; the chip-wide X280 index is 4 * L2CPU + hart.
static int l2cpu_of(const char* host) {
    const char* dash = strrchr(host, '-');
    return dash ? dash[1] - '0' : -1;
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    where_t me = {0};
    gethostname(me.host, sizeof(me.host) - 1);
    me.cpu = sched_getcpu();
    int tile = l2cpu_of(me.host);
    me.hart = tile >= 0 ? 4 * tile + me.cpu : -1;

    where_t all[64];
    MPI_Gather(&me, sizeof(me), MPI_BYTE, all, sizeof(me), MPI_BYTE, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        printf("%d MPI ranks on Blackhole X280 cores\n\n", size);
        printf("rank  guest       L2CPU  local cpu  chip-wide X280\n");
        for (int r = 0; r < size; r++) {
            printf("%4d  %-10s  %5d  %9d  %14d\n", r, all[r].host, l2cpu_of(all[r].host), all[r].cpu, all[r].hart);
        }
    }
    MPI_Finalize();
    return 0;
}
