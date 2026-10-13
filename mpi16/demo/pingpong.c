// Ping-pong between rank 0 and every other rank: same-island peers use shared memory,
// other-island peers go over lan0 (X280 -> host -> X280).
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static double pingpong(int rank, int peer, char* buf, int bytes, int iters) {
    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();
    for (int i = 0; i < iters; i++) {
        if (rank == 0) {
            MPI_Send(buf, bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD);
            MPI_Recv(buf, bytes, MPI_BYTE, peer, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else if (rank == peer) {
            MPI_Recv(buf, bytes, MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(buf, bytes, MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        }
    }
    return (MPI_Wtime() - t0) / iters / 2.0;  // one-way
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char host[32] = {0}, peer_host[32];
    gethostname(host, sizeof(host) - 1);
    const int big = 1 << 20;
    char* buf = calloc(1, big);

    if (rank == 0) {
        printf("one-way, rank 0 on %s\n\n", host);
        printf("peer  guest       path            8 B latency   1 MiB bandwidth\n");
    }
    for (int peer = 1; peer < size; peer++) {
        if (rank == peer) {
            MPI_Send(host, sizeof(host), MPI_CHAR, 0, 1, MPI_COMM_WORLD);
        } else if (rank == 0) {
            MPI_Recv(peer_host, sizeof(peer_host), MPI_CHAR, peer, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
        pingpong(rank, peer, buf, 8, 10);  // warm up the connection
        double lat = pingpong(rank, peer, buf, 8, 200);
        double t = pingpong(rank, peer, buf, big, 10);
        if (rank == 0) {
            printf("%4d  %-10s  %-14s  %8.1f us  %10.1f MB/s\n", peer, peer_host,
                   strcmp(host, peer_host) == 0 ? "shared memory" : "lan0 via host", lat * 1e6, big / t / 1e6);
        }
    }
    free(buf);
    MPI_Finalize();
    return 0;
}
