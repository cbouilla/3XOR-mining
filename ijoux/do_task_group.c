#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <err.h>
#include <getopt.h>
#include <byteswap.h>

#include <mpi.h>

#include "common.h"

/* test

./do_task_group --partitioning-bits 0 --tg-per-job 1 --tg-per-job 1

./do_task_group --tg-per-job 1 --cpu-grid-size 1 --per-core-grid-size 8 --input-dir ../data/task_groups/ --task-grid-size 8 --job 0

*/

/* fonction externe, boite noire */
struct task_result_t * iterated_joux_task_(struct jtask_t *task, u32 task_index[2]);

#define CPU_VERBOSE 0

void *load_file_MPI(const char *filename, u64 * size, MPI_Comm comm)
{
	int rank;
	u64 *content = NULL;

	MPI_Comm_rank(comm, &rank);

	if (rank == 0)   // load the file
		content = load_file(filename, size);
	
	/* broadcast its size */
	MPI_Bcast(size, 1, MPI_UINT64_T, 0, comm);
	
	if (rank != 0) {
		content = aligned_alloc(64, *size * 8);
		if (content == NULL)
			err(1, "failed to allocate memory (non-root)");
	}

	/* broadcast its content */
	MPI_Bcast(content, *size, MPI_UINT64_T, 0, comm);

	return content;
}


struct tg_context_t {
	int task_grid_size;
	int cpu_grid_size;
	int per_core_grid_size;
	int tg_per_job;
	int cpu_i;
	int cpu_j;
	int rank;
	int comm_size;
	char *input_dir;
	void * data[3];
};


static void tg_task_base(struct tg_context_t *ctx, int tg_i, int tg_j, u32 base[3])
{
	base[0] = tg_i * ctx->cpu_grid_size + ctx->cpu_i;
	base[1] = tg_j * ctx->cpu_grid_size + ctx->cpu_j;
	base[2] = base[0] ^ base[1];
}


static void tg_task_idx(struct tg_context_t *ctx, int tg_i, int tg_j, int task_i, int task_j, u32 idx[3])
{
	tg_task_base(ctx, tg_i, tg_j, idx);
	idx[0] = idx[0] * ctx->per_core_grid_size + task_i;
	idx[1] = idx[1] * ctx->per_core_grid_size + task_j;
	idx[2] = idx[0] ^ idx[1];
}


struct option longopts[11] = {
	{"task-grid-size", required_argument, NULL, 'b'},
	{"tg-per-job", required_argument, NULL, 'g'},
	{"input-dir", required_argument, NULL, 'h'},
	{"tg-grid-size", required_argument, NULL, 't'},
	{"cpu-grid-size", required_argument, NULL, 'c'},
	{"per-core-grid-size", required_argument, NULL, 'p'},
	{"i", required_argument, NULL, 'i'},
	{"j", required_argument, NULL, 'j'},
	{"job", required_argument, NULL, 'o'},
	{NULL, 0, NULL, 0}
};


/* process command-line arguments */
struct tg_context_t * setup(int argc, char **argv, int *i, int *j, int *job)
{
	/* MPI setup */
	int rank, world_size;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &world_size);

	/* setup and command-line options */

	/* A "task group" is done by process_grid_size ** 2 cores.
           Each core does per_core_grid_size ** 2 tasks.
           So, (process_grid_size * per_core_grid_size) ** 2 tasks are done in a group.

           There are 4 ** (partitioning_bits) tasks in total, so there is a 2D
           grid of size 2 ** (partitioning_bits) / (process_grid_size * per_core_grid_size)
        */
        struct tg_context_t *ctx = malloc(sizeof(*ctx));
	ctx->task_grid_size = -1;
	ctx->tg_per_job = -1;
	ctx->cpu_grid_size = -1;
	ctx->per_core_grid_size = -1;
        ctx->rank = rank;
        ctx->comm_size = world_size;
        ctx->input_dir = NULL;
	*i = -1;
	*j = -1;
	*job = -1;

        signed char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'g':
                        ctx->tg_per_job = atol(optarg);
                        break;
                case 'c':
                        ctx->cpu_grid_size = atol(optarg);
                        break;
                case 'p':
                        ctx->per_core_grid_size = atol(optarg);
                        break;
                case 'b':
                         ctx->task_grid_size = atol(optarg);
                         break;
                case 'h':
                        ctx->input_dir = optarg;
                        break;
                case 'i':
                        *i = atol(optarg);
                        break;
                case 'j':
                        *j = atol(optarg);
                        break;
                case 'o':
                        *job = atol(optarg);
                        break;
                default:
                        errx(1, "Unknown option\n");
                }
	}

	/* validation */
	if (ctx->cpu_grid_size < 0)
		errx(1, "missing --cpu-grid-size");
	if (ctx->per_core_grid_size < 0)
		errx(1, "missing --per-core-grid-size");
	if (ctx->input_dir == NULL)
		errx(1, "missing --input-dir");

	if (world_size != ctx->cpu_grid_size * ctx->cpu_grid_size)
		errx(2, "wrong communicator size (MPI says %d, I wanted %d)", world_size, ctx->cpu_grid_size * ctx->cpu_grid_size);

	if ((*i < 0) & (*job < 0))
		errx(3, "must provide either --job or --i/--j");
	if (!((*i >= 0) ^ (*job >= 0)))
		errx(3, "--i/--j and --job are mutually exclusive");
	if ((*i >= 0) ^ (*j >= 0))
		errx(3, "must give both --i and --j");
		
	if (*job >= 0) {
		if (ctx->tg_per_job < 0)
			errx(1, "missing --tg-per-job with --job");
		if (ctx->task_grid_size < 0)
			errx(1, "missing --task-grid-size with --job");
		if (ctx->task_grid_size < ctx->cpu_grid_size * ctx->per_core_grid_size)
			errx(1, "inconsistent parameters (task-grid-size too small)");
	}
	
	/* my own coordinates in the CPU grid */
	ctx->cpu_i = rank / ctx->cpu_grid_size;
	ctx->cpu_j = rank % ctx->cpu_grid_size;
	return ctx;
}


static struct jtask_t * load_tg_data(struct tg_context_t *ctx, int tg_i, int tg_j)
{
	char filename[255];
	u32 base[3];
	MPI_Comm comm_I, comm_J, comm_IJ;
	u64 devnull;

	double start = MPI_Wtime();
	struct jtask_t *all_tasks = malloc(ctx->per_core_grid_size * sizeof(*all_tasks));
	tg_task_base(ctx, tg_i, tg_j, base);

	/* A */
	sprintf(filename, "%s/foo.%03x", ctx->input_dir, base[0]);
	MPI_Comm_split(MPI_COMM_WORLD, base[0], 0, &comm_I);
	u64 *A = load_file_MPI(filename, &devnull, comm_I);
	if (A[0] != (u64) ctx->per_core_grid_size)
		errx(4, "wrong task-group size (foo)");
	for (u64 r = 0; r < A[0]; r++) {
		all_tasks[r].L[0] = A + A[r + 1];
		all_tasks[r].n[0] = A[r + 2] - A[r + 1];
	}
	ctx->data[0] = A;
	MPI_Comm_free(&comm_I);

	/* B */
	sprintf(filename, "%s/bar.%03x", ctx->input_dir, base[1]);
	MPI_Comm_split(MPI_COMM_WORLD, base[1], 0, &comm_J);	
	u64 *B = load_file_MPI(filename, &devnull, comm_J);
	if (B[0] != (u64) ctx->per_core_grid_size)
		errx(4, "wrong task-group size (bar)");
	for (u64 r = 0; r < B[0]; r++) {
		all_tasks[r].L[1] = B + B[r + 1];
		all_tasks[r].n[1] = B[r + 2] - B[r + 1];
	}
	ctx->data[1] = B;
	MPI_Comm_free(&comm_J);

	/* C */
	sprintf(filename, "%s/foobar.%03x", ctx->input_dir, base[2]);
	MPI_Comm_split(MPI_COMM_WORLD, base[2], 0, &comm_IJ);
	u64 *C = load_file_MPI(filename, &devnull, comm_IJ);
	if (C[0] != (u64) ctx->per_core_grid_size)
		errx(4, "wrong task-group size (foobar)");
        for (u64 r = 0; r < C[0]; r++) {
	        all_tasks[r].slices = (struct slice_t *) (C + C[r + 1]);
	        all_tasks[r].slices_size = C[r + 2] - C[r + 1];
        }
        ctx->data[2] = C;
	MPI_Comm_free(&comm_IJ);

	double end_load = MPI_Wtime();
	if (ctx->rank == 0)
		printf("Total data load time %.fs\n", end_load - start);
	
	
	return all_tasks;
}


static struct task_result_t * tg_task_work(struct tg_context_t *ctx, int tg_i, int tg_j, struct jtask_t *all_tasks)
{
	struct task_result_t *all_solutions = result_init();
	double all_tasks_start = MPI_Wtime();
	for (int r = 0; r < ctx->per_core_grid_size; r++) {
	        for (int s = 0; s < ctx->per_core_grid_size; s++) {
			/* build task descriptor */
			struct jtask_t task;
			u32 task_index[3];
			tg_task_idx(ctx, tg_i, tg_j, r, s, task_index);
			task.L[0] = all_tasks[r].L[0];
			task.n[0] = all_tasks[r].n[0];
			task.L[1] = all_tasks[s].L[1];
			task.n[1] = all_tasks[s].n[1];
			task.slices = all_tasks[r ^ s].slices;
			task.slices_size = all_tasks[r ^ s].slices_size;

			double task_start = MPI_Wtime();	
			
			if (CPU_VERBOSE) {
				printf(" [%04x ; %04x ; %04x] : ", 
					task_index[0], task_index[1], task_index[2]);
			}


			struct task_result_t * result = iterated_joux_task(&task, task_index);
		
			if (CPU_VERBOSE)
				printf("%.1fs; %d solutions\n", MPI_Wtime() - task_start, result->size);
			
			/* copy task solutions into global all_solutions array */
			for (u32 u = 0; u < result->size; u++) {
				struct solution_t * solution = &result->solutions[u];
				report_solution(all_solutions, solution);		
			}
                        result_free(result);
		}
	}
	
	/* synchronisation */
	double barrier_start = MPI_Wtime();
	MPI_Barrier(MPI_COMM_WORLD);
	if (CPU_VERBOSE)
		printf("Waited in BARRIER: %.1fs\n", MPI_Wtime() - barrier_start);

	if (ctx->rank == 0)
		printf("All tasks done: %.1fs\n", MPI_Wtime() - all_tasks_start);

	return all_solutions;
}

void tg_solution_filename(int tg_i, int tg_j, char *filename)
{
	sprintf(filename, "solutions_%02x_%02x.bin", tg_i, tg_j);
}

void tg_gather_and_save(struct tg_context_t * ctx, int tg_i, int tg_j, struct task_result_t * all_solutions)
{
	/* gather solutions to node 0 */
	double transmission_start = MPI_Wtime();
	int *solutions_sizes = NULL;
	int *displacements = NULL;
	struct solution_t *solutions_recv = NULL;

	// MPI_Gather sur un tableau de taille 1 : all_solutions->size;  [1 x MPI_UINT32_T]
	if (ctx->rank == 0) 
		solutions_sizes = malloc(sizeof(u32) * ctx->comm_size);
	u32 u64_to_send = 6 * all_solutions->size;
	MPI_Gather(&u64_to_send, 1, MPI_UINT32_T, solutions_sizes, 1, MPI_UINT32_T, 0, MPI_COMM_WORLD);

	// MPI_Gatherv sur un tableau de taille 6 * all_solutions->size : all_solutions->solutions  [MPI_UINT64_T]
	int d = 0;
	if (ctx->rank == 0) {
		displacements = malloc(sizeof(int) * ctx->comm_size);
		for (int i = 0; i < ctx->comm_size; i++) {
			displacements[i] = d;
			d += solutions_sizes[i];
		}
		solutions_recv = malloc(sizeof(struct solution_t) * (d / 6));
		printf("#solutions = %d\n", d / 6);
	}
	MPI_Gatherv(all_solutions->solutions, u64_to_send, MPI_UINT64_T, solutions_recv, 
		solutions_sizes, displacements, MPI_UINT64_T, 0, MPI_COMM_WORLD);

	/* If I'm rank zero, I save in a file */
	if (ctx->rank == 0) {
		free(solutions_sizes);
		free(displacements);

		char filename[255];
		tg_solution_filename(tg_i, tg_j, filename);
		FILE *f_solutions = fopen(filename, "w");
		if (f_solutions == NULL)
                	err(1, "fopen failed (%s)", filename);
		int check = fwrite(solutions_recv, sizeof(struct solution_t), d / 6, f_solutions);
		if (check != d / 6)
	                errx(1, "incomplete write %s", filename);
        	fclose(f_solutions);

		if (ctx->rank == 0)
			printf("Gathering and saving: %.1fs\n", MPI_Wtime() - transmission_start);
		
		for (int i = 0; i < d / 6; i++) 
			printf("[%04" PRIx64 ";%04" PRIx64 ";%04" PRIx64 "] %016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " == 0\n", 
				solutions_recv[i].task_index[0], solutions_recv[i].task_index[1], solutions_recv[i].task_index[2],
				solutions_recv[i].val[0], solutions_recv[i].val[1], solutions_recv[i].val[2]);
		free(solutions_recv);
	}
}


void tg_cleanup(struct tg_context_t * ctx, struct task_result_t * all_solutions)
{
	result_free(all_solutions);
	for (int i = 0; i < 3; i++)
		free(ctx->data[i]);
}


void do_task_group(struct tg_context_t * ctx, int tg_i, int tg_j)
{
	/* check if checkpointed */
	char filename[255];
	tg_solution_filename(tg_i, tg_j, filename);
	FILE *f_solutions = fopen(filename, "r");
	if (f_solutions != NULL) {
		/* solution file exists. SKIP ! */		
		fclose(f_solutions);
		if (ctx->rank == 0)
			printf("SKIPPING task goup (%d, %d)\n", tg_i, tg_j);
		return;
	}

	if (ctx->rank == 0)
		printf("Doing task goup (%d, %d)\n", tg_i, tg_j);

	struct jtask_t *all_tasks = load_tg_data(ctx, tg_i, tg_j);
	struct task_result_t * all_solutions = tg_task_work(ctx, tg_i, tg_j, all_tasks);
	tg_gather_and_save(ctx, tg_i, tg_j, all_solutions);
	tg_cleanup(ctx, all_solutions);
}


void do_job(struct tg_context_t * ctx, int job) 
{
	int tg_grid_size = ctx->task_grid_size / ctx->cpu_grid_size / ctx->per_core_grid_size;
	int tg_from = job * ctx->tg_per_job;
	int tg_to = (job + 1) * ctx->tg_per_job;
	int njobs = tg_grid_size * tg_grid_size / ctx->tg_per_job;

	if (job >= njobs)
		errx(3, "invalid job number");

	if (ctx->rank == 0) {
		printf("* Task grid       is [%d x %d]\n", ctx->task_grid_size, ctx->task_grid_size);
		printf("* Task Group grid is [%d x %d]\n", tg_grid_size, tg_grid_size);
		printf("* #jobs           is %d\n", njobs);
	}

	for (int k = tg_from; k < tg_to; k++) {
		int i = k / tg_grid_size;
		int j = k % tg_grid_size;

		do_task_group(ctx, i, j);
	}
}


int main(int argc, char **argv)
{
	MPI_Init(&argc, &argv);
	int i, j, job;
	struct tg_context_t * ctx = setup(argc, argv, &i, &j, &job);
	
	if (job < 0) {
		/* do single task group */
		do_task_group(ctx, i, j);
	} else {
		do_job(ctx, job);
	}
	
	MPI_Finalize();
}