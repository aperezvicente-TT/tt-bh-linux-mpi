CFLAGS := -O2
all: hello_mpi mpi_pi pingpong
%: %.c
	mpicc $(CFLAGS) -o $@ $<
clean:
	rm -f hello_mpi mpi_pi pingpong
