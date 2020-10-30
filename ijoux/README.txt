To try the iterated Joux algorithm :

1) Generate an instance of a given size N with known solutions

	# preprocessing/forger N

N = 128000 is reasonable. The 3 files:
- bar.000  
- foo.000
- foobar.000 
are created in the current directory.

1.5) Generate the slices (in the current directory).

	# slicer foobar.000 --target-dir . --l 19


2) Solve it

	# ./joux_standalone --partitioning-bits 0 --hash-dir HASHPATH --slice-dir SLICEPATH

Set HASHPATH must be the path where the 3 hashfiles are located.
This should report the solutions announced by the forger.


Production data
================

Experiments show that the best size for hash files is between 128*1024 and
256*1024 elements (better smaller than larger), and it seems that the best
value of l is the smallest admissible one, namely l=19.

partitioning-bits=15

The computation is split into a 2D grid of 32768 x 32768 taks (this is imposed
by the task processing function). Each task takes roughly 30s. This should
make 9 million hours. Because individual tasks are too short, we will group
them into TASK GROUPs. The intention is that a task group is a sizable unit
of work.

One viable option is to use u^2 cores and have each core do v^2 tasks. 

u = 64 ---> 4096 cores, 1/4 rack
u = 128 ---> 16384 cores, 1 rack
u = 256 ---> 65536 cores, 4 rack

v = 8  --->   64 tasks / core ---> 32 minutes + overhead.  Data ==  32Mo / core
v = 16 --->  256 tasks / core ---> 128 minutes + overhead. Data ==  64Mo / core
v = 32 ---> 1024 tasks / core ---> 8.5 hours + overhead.   Data == 128Mo / core

u = 64,  v = 8  ---> MRt1
u = 64,  v = 32 ---> MRt3

u = 128, v =  8 ---> 1Rt1
u = 128, v = 16 ---> 1Rt2            
u = 128, v = 32 ---> 1Rt3          

u = 256, v = 8   ---> 4Rt1           xxxxxxxxxxxxxx
u = 256, v = 32  ---> 4Rt3


Each task group corresponds to 2^22 tasks (a slice of 2048 * 2048 of the whole grid). 
Thus, the computation is split into a 2D grid of 16x16 task groups. Each task group represents 70'000 CPU-hours.

A JOB is composed of several task groups. Checkpointing is done after each task group.

To keep 4R busy for about 20 hours, a JOB is made of 32 TASK GROUPs. 
Thus, there will be 8 JOBs, and each job is 4Rt3, runs for 17h.

I/O and logistics
=================

Maybe group input for a task group into a single file ? 2048 x 16Mb file for each list.

--------------------------------------------------------------------------------------------

bar.18 vérollé à 100%
bar.19 OK
bar.20 OK
21 OK
22 OK

/!\  a priori aucun autre bar vérolé, et foo OK (sauf le premier mais on le savait).

collision entre des bars de date proche...