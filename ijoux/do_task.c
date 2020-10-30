#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <getopt.h>
#include <string.h>
#include <math.h>

#include "common.h"

void usage()
{
	printf("--i=I --j=J             Solve task (i, j) given in HEXADECIMAL\n");
	printf("--n=N                   Solve the first N tasks\n");
	printf("--hash-dir=PATH         Location of hash files\n");
	printf("--slice-dir=PATH        Location of slice files\n\n");
}

void do_task(const char *hash_dir, const char  *slice_dir, u32 i, u32 j)
{
	printf("[%04x ; %04x ; %04x] ", i, j, i^j);
	fflush(stdout);
	u32 idx[3] = {i, j, i ^ j};
	struct jtask_t task;

	for (u32 k = 0;  k < 2; k++) {
		char filename[255];
		char *kind_name[3] = {"foo", "bar", "foobar"};
		sprintf(filename, "%s/%s.%03x", hash_dir, kind_name[k], idx[k]);
		task.L[k] = load_file(filename, &task.n[k]);
	}

	char filename[255];
	sprintf(filename, "%s/%03x", slice_dir, idx[2]);
	task.slices = load_file(filename, &task.slices_size);

	/* Now, random permutation is done during preprocessing
	#pragma omp parallel for schedule(static)
	for (u32 k = 0; k < 2; k++)
		for (u32 i = 0; i < task.n[k] - 1; i++) {
			u32 j = i + (task.L[k][i] % (task.n[k] - i));
			u64 x = task.L[k][i];
			task.L[k][i] = task.L[k][j];
			task.L[k][j] = x;
		}
	*/

	double start = wtime();
	struct task_result_t *result = iterated_joux_task(&task, idx);
	printf("%.2fs\n", wtime() - start);

	if (result->size > 0) {
		printf("#solutions = %d\n", result->size);
		for (u32 u = 0; u < result->size; u++) {
			struct solution_t * sol = &result->solutions[u];
			printf("%016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " == 0\n",
					sol->val[0], sol->val[1], sol->val[2]);
		}
	}

	result_free(result);
	free(task.L[0]);
	free(task.L[1]);
	free(task.slices);
}

int main(int argc, char **argv)
{	
	/* parse command-line options */
	struct option longopts[6] = {
		{"i", required_argument, NULL, 'i'},
		{"j", required_argument, NULL, 'j'},
		{"n", required_argument, NULL, 'n'},
		{"hash-dir", required_argument, NULL, 'h'},
		{"slice-dir", required_argument, NULL, 's'},
		{NULL, 0, NULL, 0}
	};
	u32 i = 0xffffffff;
	u32 j = 0xffffffff;
	u32 n = 0xffffffff;
	char *hash_dir = NULL;
	char *slice_dir = NULL;
	signed char ch;
	while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (ch) {
		case 'i':
			i = strtol(optarg, NULL, 16);
			break;
		case 'j':
			j = strtol(optarg, NULL, 16);
			break;
		case 'n':
			n = atoi(optarg);
			break;
		case 'h':
			hash_dir = optarg;
			break;
		case 's':
			slice_dir = optarg;
			break;
		default:
			errx(1, "Unknown option\n");
		}
	}
	if (n == 0xffffffff && i == 0xffffffff && j == 0xffffffff) {
		usage();
		errx(1, "missing either --n or --i/--j");
	}
	if (n != 0xffffffff && (i != 0xffffffff || j != 0xffffffff)) {
		usage();
		errx(1, "conflicting options.");
	}
	if ((i != 0xffffffff) ^ (j != 0xffffffff)) {
		usage();
		errx(1, "Both i and j must be given.");
	}
	if (hash_dir == NULL) {
		usage();
		errx(1, "missing option --hash-dir");
	}
	if (slice_dir == NULL) {
		usage();
		errx(1, "missing option --slice-dir");
	}

	double start = wtime();
	if (n == 0xffffffff) {
		do_task(hash_dir, slice_dir, i, j);
	} else {
		u32 grid_size = pow(2, ceil(log2(n) / 2));
		u32 k = 0;
		printf("Task grid is %d x %d\n", grid_size, grid_size);
		for (u32 i = 0; i < grid_size; i++)
			for (u32 j = 0; j < grid_size; j++) {
				if (k == n)
					break;
				do_task(hash_dir, slice_dir, i, j);
				k++;
			}
	}
	printf("FINISHED in %.1fs\n", wtime() - start);
}
