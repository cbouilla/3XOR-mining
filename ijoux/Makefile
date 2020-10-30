# settings for gcc on a "normal" computer
CC = mpicc
CFLAGS = -std=c11 -g -Wall -Wextra -O3 -fopenmp
LDFLAGS = -fopenmp

# settings for xlc on turing
#CC = mpixlc_r
#CFLAGS = -qsmp=omp

LDLIBS = -lm

all: programs

common.o: common.h
joux_v3.o: common.h datastructures.h

programs: do_task do_task_group
do_task: common.o joux_v3.o
do_task_group: common.o joux_v3.o

clean:
	rm -rf *.o
	rm -rf do_task do_task_group
