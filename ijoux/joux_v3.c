#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <byteswap.h>
#include <strings.h>

#include <omp.h>
// #include <papi.h>

#include "common.h"
#include "datastructures.h"


struct side_t {
	u64 *L;			/* input list */
	u32 n;			/* size of L */

	/**** multi-threaded partitioning ****/
	u32 psize;		/* capacity of partitions */
	u32 tsize;		/* capacity of thread-private buckets */
	u64 *LM;		/* scratch space for the matrix product */
	u64 *scratch;		/* scratch space for partitioning */
	u32 *count;		/* counters for dispatching */
	u32 partition_size;	/* upper-bound on the actual number of items in
				   a partition */
};

struct context_t {
	/**** input ****/
	u64 *L[2];
	u32 n[2];

	/**** nicely presented input ****/
	struct side_t side[2];

	/**** tuning parameters ****/
	u32 T_gemm, T_part, T_subj;	/* number of threads */
	u32 p;			/* bits used in partitioning */

	/**** performance measurement ****/
	u64 volume, probes;
	u64 gemm_usec, part_usec, subj_usec, chck_usec;
	u32 bad_slice;

	/**** scratch space ****/
	u64 (*preselected[4])[3];

	/**** output ****/
	struct task_result_t *result;
};

struct slice_ctx_t {
	const struct slice_t *slice;
	u64 *H;
	struct task_result_t *result;
};
struct matmul_table_t {
	u64 tables[8][256] __attribute__ ((aligned(64)));
};

struct scattered_t {
	u64 **L;
	u32 *n;
};

static const u32 CACHE_LINE_SIZE = 64;

static u64 ROUND(u64 s)
{
	return CACHE_LINE_SIZE * ceil(((double)s) / CACHE_LINE_SIZE);
}

static u64 chernoff_bound(u64 N, u32 n_buckets)
{
	double mu = ((double)N) / n_buckets;
	double delta = sqrt(210 / mu);
	return ROUND(N * (1 + delta) / n_buckets);
}

static void prepare_side(struct context_t *self, u32 k, bool verbose)
{
	u32 T = self->T_part;
	u32 n = self->n[k];
	u32 fan_out = 1 << self->p;
	// TODO : verifier si l'alignement de tsize sur 64 n'est pas une connerie.
	u32 tsize = chernoff_bound(n, T * fan_out);
	u32 psize = tsize * T;
	u32 scratch_size = psize * fan_out;
	u32 partition_size = chernoff_bound(n, fan_out);

	u64 *scratch = aligned_alloc(CACHE_LINE_SIZE, sizeof(u64) * scratch_size);
	if (scratch == NULL)
		err(1, "failed to allocate scratch space");
	u64 *LM = aligned_alloc(CACHE_LINE_SIZE, sizeof(u64) * n);
	if (LM == NULL)
		err(1, "failed to allocate LM");
	u32 count_size = ROUND(sizeof(u32) * T * fan_out);
	u32 *count = aligned_alloc(CACHE_LINE_SIZE, count_size);
	if (count == NULL)
		err(1, "failed to allocate count");

	struct side_t *side = &self->side[k];
	side->L = self->L[k];
	side->n = n;
	side->tsize = tsize;
	side->psize = psize;
	side->scratch = scratch;
	side->LM = LM;
	side->count = count;
	side->partition_size = partition_size;

	if (verbose) {
		printf("side %d, n=%d, T=%d\n", k, n, T);
		printf("========================\n");
		double expansion = (100.0 * (scratch_size - n)) / n;
		printf
		    ("|scratch| = %d items (expansion = %.1f %%), tisze=%d, psize=%d, part_size=%d\n",
		     scratch_size, expansion, tsize, psize, partition_size);
		double st_part = (9.765625e-04 * n * 8) / fan_out;
		printf("Expected partition size = %.1f Kb\n", st_part);
	}
}


static u64 naive_gemv(u64 x, const u64 * M)
{
	u64 y = 0;
	for (u32 i = 0; i < 64; i++) {
		u64 bit = (x >> i) & 1;
		u64 mask = (u64) (-((i64) bit));
		y ^= M[i] & mask;
	}
	return y;
}

static inline u64 gemv(u64 x, const struct matmul_table_t *M)
{
	u64 r = 0;
	r ^= M->tables[0][x & 0x00ff];
	r ^= M->tables[1][(x >> 8) & 0x00ff];
	r ^= M->tables[2][(x >> 16) & 0x00ff];
	r ^= M->tables[3][(x >> 24) & 0x00ff];
	r ^= M->tables[4][(x >> 32) & 0x00ff];
	r ^= M->tables[5][(x >> 40) & 0x00ff];
	r ^= M->tables[6][(x >> 48) & 0x00ff];
	r ^= M->tables[7][(x >> 56) & 0x00ff];
	return r;
}

static void matmul_init(const u64 * M, struct matmul_table_t *T)
{
	#pragma omp parallel for schedule(static)
	for (u32 i = 0; i < 8; i++) {
		u32 lo = i * 8;
		T->tables[i][0] = 0;
		u64 tmp = 0;
		for (u32 j = 1; j < 256; j++) {
			u32 k = ffs(j) - 1;
			tmp ^= M[lo + k];
			T->tables[i][j ^ (j >> 1)] = tmp;
		}
	}
}

static void gemm(const u64 * IN, u64 * OUT, u32 n, const struct matmul_table_t *M)
{
	#pragma omp for schedule(static)
	for (u32 i = 0; i < n; i++)
		OUT[i] = gemv(IN[i], M);
}

static void partition(u32 p, struct side_t *side)
{
	u32 tid = omp_get_thread_num();
	u32 fan_out = 1 << p;
	u32 *count = side->count + tid * fan_out;
	for (u32 i = 0; i < fan_out; i++)
		count[i] = side->psize * i + side->tsize * tid;
	const u64 *L = side->LM;
	const u32 n = side->n;
	u64 *scratch = side->scratch;
	u8 shift = 64 - p;

	#pragma omp for schedule(static)
	for (u32 i = 0; i < n; i++) {
		u64 x = L[i];
		u64 h = x >> shift;
		u32 idx = count[h]++;
		scratch[idx] = x;
	}
}

static u64 subjoin(struct slice_ctx_t *ctx, u32 T, struct scattered_t *partitions, u64 (*preselected)[3])
{
	static const u32 HASH_SIZE = 16384 / 4 / sizeof(u64);
	static const u64 HASH_MASK = 16384 / 4 / sizeof(u64) - 1;
	u8 l = ctx->slice->l;
	u8 shift = 64 - l;
	u64 H[HASH_SIZE];

	/* build phase */
	for (u32 i = 0; i < HASH_SIZE; i++)
		H[i] = 0;
	for (u32 t = 0; t < T; t++) {
		u64 *A = partitions[0].L[t];
		u32 nA = partitions[0].n[t];
		for (u32 i = 0; i < nA; i++) {
			u32 h = (A[i] >> shift) & HASH_MASK;
			while (H[h] != 0)
				h = (h + 1) & HASH_MASK;
			H[h] = A[i];
		}
	}

	/* probe phase */
	u64 emitted = 0;
	for (u32 t = 0; t < T; t++) {
		u64 *B = partitions[1].L[t];
		u32 nB = partitions[1].n[t];
		for (u32 i = 0; i < nB; i++) {
			u64 y = B[i];
			u32 h = (y >> shift) & HASH_MASK;
			u64 x = H[h];
			while (x != 0) {
				u64 z = x ^ y;
				if ((z >> shift) == 0) {
					preselected[emitted][0] = x;
					preselected[emitted][1] = y;
					preselected[emitted][2] = z; 
					emitted++;
				}
				h = (h + 1) & HASH_MASK;
				x = H[h];
			}
		}
	}
	return emitted;
}


static void checkup(struct slice_ctx_t *ctx, u32 size, u64 (*preselected)[3], u64 *H, bool bad_H)
{
	if (bad_H) {
		for (u32 i = 0; i < size; i++) {
			if (linear_lookup(H, preselected[i][2])) {
				struct solution_t solution;
				solution.val[0] = preselected[i][0];
				solution.val[1] = preselected[i][1];
				solution.val[2] = preselected[i][2];
				report_solution(ctx->result, &solution);
      			}
		}
	} else {
		for (u32 i = 0; i < size; i++) {
			if (cuckoo_lookup(H, preselected[i][2])) {
				struct solution_t solution;
				solution.val[0] = preselected[i][0];
				solution.val[1] = preselected[i][1];
				solution.val[2] = preselected[i][2];
				report_solution(ctx->result, &solution);
			}
		}
	}
}


static void process_slice(struct context_t *self, const struct slice_t *slice,
		      const u32 *task_index, bool verbose)
{
	(void) verbose;
	struct slice_ctx_t ctx = {.slice = slice };
	ctx.result = result_init();
	u64 H[512];
	bool bad_H = cuckoo_build(slice->CM, 0, slice->n, H);
	if (bad_H)
		self->bad_slice++;
	u32 fan_out = 1 << self->p;
	u64 volume = self->n[0] + self->n[1];
	self->volume += volume;
	if (slice->l - self->p < 9)
		printf("WARNING : l and p are too close (increase l)\n");
	struct matmul_table_t M;
	matmul_init(slice->M, &M);
	
	/************* phase 1: GEMM */

	long long gemm_start = usec();
	#pragma omp parallel num_threads(self->T_gemm)
	{
		gemm(self->L[0], self->side[0].LM, self->side[0].n, &M);
		gemm(self->L[1], self->side[1].LM, self->side[1].n, &M);
	}
	self->gemm_usec += usec() - gemm_start;

	/************* phase 2: partitioning */

	long long part_start = usec();
	#pragma omp parallel num_threads(self->T_part)
	{
		for (u32 k = 0; k < 2; k++)
			partition(self->p, &self->side[k]);
	}
	self->part_usec += usec() - part_start;
	
	/************* phase 3: subjoins */

	long long subj_start = usec();
	u32 n_preselected[4] = {0, 0, 0, 0};
	#pragma omp parallel num_threads(self->T_subj)
	{
		u32 probes = 0;
		
		int tid = omp_get_thread_num();
		u64 (*preselected)[3] = self->preselected[tid];
		#pragma omp for schedule(dynamic, 4)
		for (u32 i = 0; i < fan_out; i++) {
			u32 T = self->T_part;
			u64 *L[2][4];
			u32 n[2][4];
			struct scattered_t scattered[2];
			for (u32 k = 0; k < 2; k++) {
				scattered[k].L = L[k];
				scattered[k].n = n[k];
				struct side_t *side = &self->side[k];
				for (u32 t = 0; t < T; t++) {
					u32 lo = side->psize * i + side->tsize * t;
					u32 hi = side->count[t * fan_out + i];
					scattered[k].L[t] = side->scratch + lo;
					scattered[k].n[t] = hi - lo;
				}
			}
			
			u32 size = subjoin(&ctx, T, scattered, preselected + probes);
			probes += size;
		}
		n_preselected[tid] = probes;
	}
	self->subj_usec += usec() - subj_start;
	self->probes += n_preselected[0] + n_preselected[1] + n_preselected[2];

	/************* phase 4: intersection with CM */

	long long chck_start = usec();
	#pragma omp parallel num_threads(self->T_subj)
	{
		int tid = omp_get_thread_num();
		u64 (*preselected)[3] = self->preselected[tid];
		checkup(&ctx, n_preselected[tid], preselected, H, bad_H);
	}
	self->chck_usec += usec() - chck_start;

	/* lift solutions */
	struct solution_t * loc = ctx.result->solutions;
	u32 n_sols = ctx.result->size;
	for (u32 i = 0; i < n_sols; i++) {
		struct solution_t solution;
		for (u32 j = 0; j < 3; j++) {
			solution.val[j] = naive_gemv(loc[i].val[j], slice->Minv);
			solution.task_index[j] = task_index[j];
		}
		report_solution(self->result, &solution);
	}
	result_free(ctx.result);
}



struct task_result_t *iterated_joux_task(struct jtask_t *task, const u32 *task_index)
{
	static const bool task_verbose = false;
	static const bool slice_verbose = false;
	
	/* setup */
	static const u32 p = 10;	// hardcodé !
	struct task_result_t *result = result_init();
	double start = wtime();
	struct context_t self;
	for (u32 k = 0; k < 2; k++) {
		self.n[k] = task->n[k];
		self.L[k] = task->L[k];
	}
	self.T_gemm = 4;
	self.T_part = 2;
	self.T_subj = 3;
	self.p = p;
	self.result = result;
	for (u32 k = 0; k < 2; k++)
		prepare_side(&self, k, task_verbose);
	for (u32 t = 0; t < self.T_subj; t++)
		self.preselected[t] = malloc(task->n[0] * 24);
	self.volume = 0;
	self.gemm_usec = 0;
	self.part_usec = 0;
	self.subj_usec = 0;
	self.chck_usec = 0;
	self.bad_slice = 0;
	self.probes = 0;

	if (task_verbose) {
		/* task-level */
		printf("Task: |A|=%" PRId64 ",  |B|=%" PRId64 "\n", task->n[0], task->n[1]);
		double mbytes = 8 * (task->n[0] + task->n[1]) / 1048576.0;
		printf("Volume. Hash = %.1fMbyte + Slice = %.1fMbyte\n", 
			mbytes, task->slices_size / 1048576.0);
		printf("Partition size (A): %.0f elements (hash fill=%.0f%%)\n", 
			task->n[0] / 1024., task->n[0] / 5242.88);
	}
	
	/* process all slices */
	struct slice_t *slice = task->slices;
	u32 i = 0;
	u64 *end = ((u64 *) task->slices) + task->slices_size;
	while (((u64 *) slice) < end) {
		process_slice(&self, slice, task_index, slice_verbose);
		i++;
		
		u64 *ptr = ((u64 *) slice) + sizeof(*slice) / sizeof(*ptr) + slice->n;
		slice = (struct slice_t *) ptr;
	}

	/* cleanup */
	for (u32 k = 0; k < 2; k++) {
		free(self.side[k].scratch);
		free(self.side[k].LM);
		free(self.side[k].count);
	}
	for (u32 t = 0; t < self.T_subj; t++)
		free(self.preselected[t]);

	if (task_verbose) {
		double task_duration = wtime() - start;
		double Mvolume = self.volume * 9.5367431640625e-07;
		printf("Slices: %d (%d bad ones)\n", i, self.bad_slice);
		printf("Task duration: %.1f s\n", task_duration);
		printf("Total volume: %.1fMitem\n", Mvolume);
		printf("Breakdown:\n");
		printf("* GEMM:      \tT = %d, \ttime = %.2fs\trate = %.2fMitem/s\n",
		     self.T_gemm, 1e-6 * self.gemm_usec, self.volume / (self.gemm_usec / 1.048576));
		printf("* partition: \tT = %d, \ttime = %.2fs\trate = %.2fMitem/s\n",
		     self.T_part, 1e-6 * self.part_usec, self.volume / (self.part_usec / 1.048576));
		printf("* subjoin:   \tT = %d, \ttime = %.2fs\trate = %.2fMitem/s\n",
		     self.T_subj, 1e-6 * self.subj_usec, self.volume / (self.subj_usec / 1.048576));
		printf("         \tprobes = %.2fM\t\t%.2f x expected \n", 9.5367431640625e-07 * self.probes, self.probes / ((double) self.n[0] * self.n[1] * i / 524288.));
		printf("         \t\ttime = %.2fs\trate = %.2fMitem/s\n", 1e-6 * self.chck_usec, self.probes / (self.chck_usec / 1.048576));
	}
	return result;
}
