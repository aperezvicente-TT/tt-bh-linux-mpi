// Compute-bound scaling: integrate 4/(1+x^2) over [0,1] with a fixed total step count.
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    long n = argc > 1 ? atol(argv[1]) : 400000000L;
    double h = 1.0 / (double)n, sum = 0.0;

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();
    for (long i = rank; i < n; i += size) {
        double x = h * ((double)i + 0.5);
        sum += 4.0 / (1.0 + x * x);
    }
    double pi = 0.0, local = h * sum;
    MPI_Reduce(&local, &pi, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    double t = MPI_Wtime() - t0;

    if (rank == 0) {
        printf("ranks=%2d  steps=%ld  pi=%.12f  error=%.2e  time=%.3f s\n", size, n, pi, pi - 3.141592653589793, t);
    }
    MPI_Finalize();
    return 0;
}
