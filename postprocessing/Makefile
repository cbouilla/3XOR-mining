CFLAGS = -std=c11 -g -Wall -Wextra -O3 -Werror -I../preprocessing -fopenmp
LDFLAGS = -fopenmp
LDLIBS = -lm

all: programs 

programs: post_processing

post_processing: ../preprocessing/hasher.o ../preprocessing/sha256.o


clean:
	rm -rf *.o 
	rm -rf post_processing
