#define _XOPEN_SOURCE 500   /* strdup */
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <assert.h>
#include <math.h>
#include <arpa/inet.h>
#include <byteswap.h>

#include <mpi.h>

#include "preprocessing.h"

#define VERBOSE 0
#define QUIET 1

double wtime()
{
	struct timeval ts;
	gettimeofday(&ts, NULL);
	return (double)ts.tv_sec + ts.tv_usec / 1e6;
}

#ifdef __bgq__
u64 __builtin_popcountll(u64 x)
{
	return __popcnt8(x);
}
#endif

bool big_endian()
{
	return (htonl(0x47) == 0x47);
}

u64 * load(const char *filename, u64 *size_)
{
	struct stat infos;
	if (stat(filename, &infos))
		err(1, "fstat failed on %s", filename);
	u64 size = infos.st_size;
	assert ((size % 8) == 0);
	u64 *content = malloc(size);
	if (content == NULL)
		err(1, "failed to allocate memory");
	FILE *f = fopen(filename, "r");
	if (f == NULL)
		err(1, "fopen failed (%s)", filename);
	u64 check = fread(content, 1, size, f);
	if (check != size)
		errx(1, "incomplete read %s", filename);
	fclose(f);
	*size_ = size / 8;
	if (big_endian()) {
		for (u32 i = 0; i < size / 8; i++)
			content[i] = bswap_64(content[i]);
	}
	return content;
}

static const u32 MAX_ISD_ITERATIONS = 100;

struct list_t {
	u64 x;
	struct list_t *next, *prev;
};

struct list_t *active, *reject;
u32 m;


static void list_insert(struct list_t *list, struct list_t *item)
{
	item->next = list->next;
	item->prev = list;
	item->next->prev = item;
	item->prev->next = item;        
}

static void list_remove(struct list_t *item)
{
	item->next->prev = item->prev;
	item->prev->next = item->next;
}

static void list_clear(struct list_t *item)
{
	assert(item->x == 0);
	item->next = item;
	item->prev = item;
}

static inline bool satisfy_equation(u64 x, u64 eq)
{
	return (__builtin_popcountll(x & eq) & 1) == 0;
}

void filter_active(u64 eq)
{
	struct list_t *item = active->next;
	while (item != active) {
		struct list_t *next = item->next;      /* we have to save this before modifying it */
		if (!satisfy_equation(item->x, eq)) {
			list_remove(item);
			list_insert(reject, item);
			m--;
		}
		item = next;    
	}
}

double H(double x) {
	if (x == 0)
		return 0;
	if (x == 1)
		return 0;
	return -(x * log(x) + (1 - x) * log(1 - x)) / M_LN2;
}

double H_inv(double y) {
	double a = 0;
	double b = 0.5;

	while (b - a > 1e-9) {
		const double x = (a + b) / 2;
		const double Hx = H(x);
		if (Hx < y)
			a = x;
		else
			b = x;
	}
	return (a + b) / 2;
}

double GV(double R) {
	return H_inv(1 - R);
}



static inline void swap(u64 *M, u32 i, u32 j)
{
	u64 tmp = M[i];
	M[i] = M[j];
	M[j] = tmp;
}

static const uint64_t M1_HI = 0xffffffff00000000;
static const uint64_t M1_LO = 0x00000000ffffffff;
static const uint64_t M2_HI = 0xffff0000ffff0000;
static const uint64_t M2_LO = 0x0000ffff0000ffff;
static const uint64_t M3_HI = 0xff00ff00ff00ff00;
static const uint64_t M3_LO = 0x00ff00ff00ff00ff;
static const uint64_t M4_HI = 0xf0f0f0f0f0f0f0f0;
static const uint64_t M4_LO = 0x0f0f0f0f0f0f0f0f;
static const uint64_t M5_HI = 0xcccccccccccccccc;
static const uint64_t M5_LO = 0x3333333333333333;
static const uint64_t M6_HI = 0xaaaaaaaaaaaaaaaa;
static const uint64_t M6_LO = 0x5555555555555555;

/* this code was written by Antoine Joux for his book 
  "algorithmic cryptanalysis" (cf. http://www.joux.biz). It
  was slighlty modified by C. Bouillaguet. Just like the original, it is licensed 
  under a Creative Commons Attribution-Noncommercial-Share Alike 3.0 Unported License. */
void transpose_64(const u64 *M, u64 *T)
{
	/* to unroll manually */
	for (int l = 0; l < 32; l++) {
		T[l] = (M[l] & M1_LO) | ((M[l + 32] & M1_LO) << 32);
		T[l + 32] = ((M[l] & M1_HI) >> 32) | (M[l + 32] & M1_HI);
	}

	for (int l0 = 0; l0 < 64; l0 += 32)
		for (int l = l0; l < l0 + 16; l++) {
			uint64_t val1 = (T[l] & M2_LO) | ((T[l + 16] & M2_LO) << 16);
			uint64_t val2 = ((T[l] & M2_HI) >> 16) | (T[l + 16] & M2_HI);
			T[l] = val1;
			T[l + 16] = val2;
		}

	for (int l0 = 0; l0 < 64; l0 += 16)
		for (int l = l0; l < l0 + 8; l++) {
			uint64_t val1 = (T[l] & M3_LO) | ((T[l + 8] & M3_LO) << 8);
			uint64_t val2 = ((T[l] & M3_HI) >> 8) | (T[l + 8] & M3_HI);
			T[l] = val1;
			T[l + 8] = val2;
		}

	for (int l0 = 0; l0 < 64; l0 += 8)
		for (int l = l0; l < l0 + 4; l++) {
			uint64_t val1 = (T[l] & M4_LO) | ((T[l + 4] & M4_LO) << 4);
			uint64_t val2 = ((T[l] & M4_HI) >> 4) | (T[l + 4] & M4_HI);
			T[l] = val1;
			T[l + 4] = val2;
		}

	for (int l0 = 0; l0 < 64; l0 += 4)
		for (int l = l0; l < l0 + 2; l++) {
			uint64_t val1 = (T[l] & M5_LO) | ((T[l + 2] & M5_LO) << 2);
			uint64_t val2 = ((T[l] & M5_HI) >> 2) | (T[l + 2] & M5_HI);
			T[l] = val1;
			T[l + 2] = val2;
		}

	for (int l = 0; l < 64; l += 2) {
		uint64_t val1 = (T[l] & M6_LO) | ((T[l + 1] & M6_LO) << 1);
		uint64_t val2 = ((T[l] & M6_HI) >> 1) | (T[l + 1] & M6_HI);
		T[l] = val1;
		T[l + 1] = val2;
	}

}

void print_matrix(int n, int m, u64 *M)
{
	for (int i = 0; i < n; i++) {
		int weight = 0;
		printf("%4d: ", i);
		for (int j = 0; j < m; j++) {
			printf("%016" PRIx64 " ", M[i*m + j]);
			weight += __builtin_popcountll(M[i*m + j]);
		}
		printf("  | %d\n", weight);
	}
}

void transpose(const u64 *M, u32 w, u64 *T)
{
	for (u32 i = 0; i < w; i++) {
		u64 S[64];
		transpose_64(M + i*64, S);
		for (u32 j = 0; j < 64; j++)
			T[i + j * w] = S[j];
	}
}


void swap_columns(u64 *T, u32 w, u64 j, u64 l)
{
	i32 lw = l / 64;
	i32 lbit = l % 64;
	u64 *Tl = T + lw;
	for (u32 i = 0; i < 64; i++) {
		u64 a = T[i * w];
		u64 b = Tl[i * w];
		u64 delta = ((a >> j) ^ (b >> lbit)) & 1;
		T[i * w] ^= delta << j;
		Tl[i * w] ^= delta << lbit;
	}
}

/* The input rows are known to span a vector space of dimension less than d.
   There are 64 input rows and m input columns.
   Returns j such that columns [0:j] are echelonized.
   rows [j+1:64] are zero. */
u32 echelonize(u64 *T, u32 m, u32 w, u32 d, u64 *E)
{
	/* E is the change of basis matrix */
	for (u32 i = 0; i < 64; i++)
		E[i] = 1ull << i;     /* E == identity */

	// printf("Echelonize with m=%d, d=%d\n", m, d);

	u32 n_random_trials = 6;
	for (u32 j = 0; j < d; j++) {
		/* eliminate the j-th column */
		u32 l = j + 1;

		// printf("j = %d\n", j);

		/* search a row with a non-zero coeff ---> it will be the pivot */
		i32 i = -1;
		u64 mask = 1ull << j;
		while (1) {
			for (i32 k = j; k < 64; k++) {
				// printf("Examining row %d\n", k);
				if (((T[k * w] & mask) != 0)) {
					i = k;
					// printf("found i = %d. %016" PRIx64 "\n", i, T[i * w]);
					break;
				}
			}
			if (i >= 0)
				break;    /* found pivot */


			/* pivot not found. This means that the d first columns
			   are linearly dependent. We swap the j-th column with the o-th. */
			i32 o;
			if ((n_random_trials > 0) && (j + 1 < m)) {
				o = (j + 1) + (lrand48() % (m - (j + 1)));
				n_random_trials--;
			} else {
				if (l >= m)
					break;
				o = l;
				l++;
			}
			swap_columns(T, w, j, o);
		}
		if (i < 0)
			return j;

		/* permute the rows so that the pivot is on the diagonal */
		if (j != (u32) i) {
			swap(E, i, j);
			for (u32 k = 0; k < w; k++)
				swap(T, i * w + k, j * w + k);
		}

		/* use the pivot to eliminate everything else on the column */
		// trick to avoid testing if k != j
		assert((T[j * w] & mask) != 0);
		u64 old = T[j * w];
		T[j * w] ^= mask;
		assert((T[j * w] & mask) == 0);
		for (u32 k = 0; k < 64; k++) {
			if ((T[k * w] & mask) != 0) {
				E[k] ^= E[j];         /* record the operation */
				T[k * w] ^= old;
				for (u32 l = 1; l < w; l++)
					T[k * w + l] ^= T[j * w + l];
			}
		}
		T[j * w] ^= mask;
		assert((T[j * w] & mask) != 0);
	}
	return d;
}

/* If the columns of M span a vector space of dimension less than 64 - k, then
   find new equations that are satisfied by all vectors in M. Returns the total
   number of equations.
*/
u32 check_rank_defect(const u64 *M, u32 m, u64 *equations, int k) {
	u32 w = ceil(m / 64.);
	u32 rows = 64 * w;
	u64 E[64];
	u64 T[rows];
	u32 d = 64 - k;
	transpose(M, w, T);

	u32 j = echelonize(T, m, w, d, E);
	if (j == d)
		return k;

	/* not enough pivots found: rank defect */
	for (u32 r = j; r < d; r++) {
		for (u32 s = 0; s < w; s++)
			if (T[r * w + s] != 0) {
				printf("T[%d] != 0\n", r);
				assert(0);
			}
		equations[k] = E[r];
		k++;
	}
	printf("---> Rank defect; %d free equations\n", d - j);
	return k;
}

static u64 myrand()
{
	u64 a = lrand48(); // 31 bits
	u64 b = lrand48(); // 31 bits
	u64 c = lrand48(); // 31 bits
	return a + (b << 31) + (c << 62);
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

bool invert(const u64 *M_, u64 *Minv)
{
	u64 M[64];
	for (u64 i = 0; i < 64; i++) {
		M[i] = M_[i];
		Minv[i] = 1ull << i;
	}
	for (u64 j = 0; j < 64; j++) {
		/* search pivot */
		i32 i = -1;
		u64 mask = 1ull << j;
		for (i32 k = j; k < 64; k++)
			if (((M[k] & mask) != 0)) {
				i = k;
				break;
			}
		if (i < 0)
			return false;
		swap(M, i, j);
		swap(Minv, i, j);

		/* use the pivot to eliminate everything else on the column */
		for (u32 k = 0; k < 64; k++)
			if ((k != j) & ((M[k] & mask) != 0)) {
				M[k] ^= M[j];
				Minv[k] ^= Minv[j];
			}
	}
	return true;
}


int slice_it(u32 l, u64 *equations, double T)
{
	if (!QUIET)
		printf("--> ");

	double timeouts[l];
	for (u32 i = 0; i < l; i++) {
		timeouts[i] = 1;
	}
	double total = 0;
	for (u32 i = 0; i < l; i++)
		total += timeouts[i];
	for (u32 i = 0; i < l; i++)
		timeouts[i] *= T / total;

	u32 k = 0;
	while (k < l) {
		/* copy the active vectors into M, and pad with zeros to reach a multiple of 64 */
		u32 w = ceil(m / 64.);
		u32 rows = 64 * w;
		u64 *M = malloc(rows * sizeof(*M));
		if (M == NULL)
			err(1, "cannot allocate scratch space");
		u32 i = 0;
		for (struct list_t *item = active->next; item != active; item = item->next)
			M[i++] = item->x;
		while (i < rows)
			M[i++] = 0x0000000;

		k = check_rank_defect(M, m, equations, k);
		if (k >= l)
			break;

		u32 d = 64 - k;
		double R = ((double) d) / m;
		double expected_w = ceil(m * GV(R));

		assert(m >= d); /* we should not be worse than naive linear algebra */

		if (VERBOSE)
			printf("length=%d, dimension=%d, Rate = %.3f, GV bound: %.0f\n", m, d, R, expected_w);
		else if (!QUIET)
			printf("%d ", m);

		/* setup low-weight search */
		u32 best_weight = m;
		u64 best_equation = 0;
		u32 n_iterations = MAX_ISD_ITERATIONS * 150000 / m;

		if (VERBOSE)
			printf("Going for %d iterations\n", n_iterations);

		u64 T[rows];
		u64 E[64];
		double start = wtime();
		double stop = start + timeouts[k];
		u32 it = 0;
		while (it == 0 || wtime() < stop) {
			/* this is one iteration of the Lee-Brickell algorithm */
			it++;

			/* random permutation of the rows */
			for (u32 i = 0; i < d; i++) {
				u32 j = i + (lrand48() % (m - i));
				swap(M, i, j);
			}

			/* transpose the matrix, in order to access the columns efficiently */
			transpose(M, w, T);

			u32 j = echelonize(T, m, w, d, E);
			assert(j == d);

			/* look for a low-weight row */
			for (u32 i = 0; i < 64 - k; i++) {
				u32 weight = 0;
				for (u32 l = 0; l < w; l++)
					weight += __builtin_popcountll(T[i * w + l]);
				if (weight < best_weight) {
					if (VERBOSE)
						printf("\rw = %d (%d iterations)", weight, it);
					best_weight = weight;
					best_equation = E[i];
				}
			}
			if (best_weight < expected_w) {
				// printf("\nweight small enough; early abort\n");
				break;
			}
		}
		filter_active(best_equation);
		equations[k++] = best_equation;
		if (VERBOSE) {
			printf("\nBest weight=%d, equation=%" PRIx64 "\n", best_weight, best_equation);
			printf("Done an ISD pass. I now have %d equations and %d active vectors\n", k, m);
		}
		free(M);
	}
	assert(k >= l);
	if (VERBOSE)
		printf("Finished: I now have %d equations and %d active vectors\n", k, m);
	else if (!QUIET)
		printf("%d\n", m);
	return k;
}

void usage()
{
	printf("Slice a single hash file:\n");
	printf("	./slicer [--l INT] [--output OUT] INPUT\n");
	printf("Slice all hash files:\n");
	printf("	./slicer [--l INT] [--output-dir PATH] [--input-dir PATH] [--partitioning-bits INT]\n");
	printf("\n\nThe --l parameter default to 19\n");
	exit(EXIT_FAILURE);
}


int main(int argc, char **argv)
{
	int rank, size;

	/* process command-line options */
	struct option longopts[7] = {
		{"output", required_argument, NULL, 't'},
		{"output-dir", required_argument, NULL, 'o'},
		{"input-dir", required_argument, NULL, 'i'},
		{"partitioning-bits", required_argument, NULL, 'b'},
		{"l", required_argument, NULL, 'l'},
		{"time-control", required_argument, NULL, 'c'},
		{NULL, 0, NULL, 0}
	};
	char *target = NULL;
	char *output_dir = NULL;
	char *input_dir = NULL;
	char *in_filename = NULL;
	int partitioning_bits = -1;
	i32 l = -1;
	double remaining_time = -1;
	bool multi_mode = false;

	signed char ch;
	while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (ch) {
		case 't':
			target = optarg;
			break;
		case 'o':
			output_dir = optarg;
			multi_mode = true;
			break;
		case 'i':
			input_dir = optarg;
			multi_mode = true;
			break;
		case 'l':
			l = atoi(optarg);
			break;
		case 'b':
			partitioning_bits = atoi(optarg);
			multi_mode = true;
			break;
		case 'c':
			remaining_time = atof(optarg);   // in HOURS
			break;
		default:
			errx(1, "Unknown option\n");
		}
	}

	/* default settings */
	double end_time = INFINITY;
	if (l < 0)
		l = 19;
	if (remaining_time >= 0)
		end_time = wtime() + 3600 * remaining_time;

	if (!multi_mode) {
		if (optind >= argc)
			errx(1, "missing input file");
		if (target == NULL)
			errx(1, "missing --output");
		in_filename = argv[optind];
	} else {
		if (output_dir == NULL)
			errx(1, "missing --output-dir");
		if (input_dir == NULL)
			errx(1, "missing --input-dir");
		if (partitioning_bits < 0)
			errx(1, "missing --partitioning-bits");

		MPI_Init(&argc, &argv);
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		if (size != (1 << partitioning_bits))
			errx(1, "bad number of processes (need %d)", 1 << partitioning_bits);

		in_filename = malloc(255);
		target = malloc(255);

		sprintf(in_filename, "%s/foobar.%03x", input_dir, rank);
		sprintf(target, "%s/%03x", output_dir, rank);
		if (in_filename == NULL || target == NULL)
			err(1, "cannot allocate filenames");
	}

	u64 n;
	u64 *L = load(in_filename, &n);

	FILE *f_out = fopen(target, "w");
	if (f_out == NULL)
		err(1, "cannot open %s\n", target);

	/* setup doubly-linked lists with dummy node */
	struct list_t *items = malloc(n * sizeof(*items));
	if ((items == NULL))
		err(1, "cannot allocate linked lists");
	struct list_t active_header, reject_header;
	active = &active_header;
	reject = &reject_header;
	active->x = 0x00000000;
	reject->x = 0x00000000;
	list_clear(active);
	list_clear(reject);

	/* setup: all vectors are "active" */
	for (u32 i = 0; i < n; i++) {
		items[i].x = L[i];
		list_insert(active, &items[i]);
	}
	free(L);
	m = n;         // count of active vectors

	u64 output_size = sizeof(struct slice_t) / 8 * (1 + n / (64 - l)) + n;
	u64 *out_space = malloc(output_size * sizeof(u64));
	output_size = 0;
	if (out_space == NULL)
		err(1, "cannot allocate output");

	if (!QUIET && multi_mode)
		printf("process %d, starting.\n", rank);

	/* slice until the input list is empty */
	int slices_done = 0;
	int vectors_done = 0;
	while (n > 0) {
		/* time control */
		double avg_slice_size = (double) vectors_done / (double) slices_done;
		double nb_slices = ceil(n / (64 - l));
		double remaining_time = end_time - wtime();
		/* 1s default time per slice if nothing specified */
		double slice_time = (remaining_time < INFINITY) ? remaining_time / nb_slices : 1.0;

		if (!QUIET)
			printf ("Starting slice %d, timeout=%.2fs. Less than %.0f slices remain. %.1fs remain. %.2fms/vector remain. Avg slice size: %.1f\n",
				slices_done, slice_time, nb_slices, remaining_time, 1000. * remaining_time / n, avg_slice_size);

		/* compute the equations */
		u64 equations[64];
		u32 k = slice_it(l, equations, slice_time);

		if (VERBOSE)
			for (u32 i = 0; i < k; i++)
				printf("eq[%d] = %016" PRIx64 "\n", i, equations[i]);

		/* compute the slice and save it on disk */

		/* pad equations to obtain an invertible 64x64 matrix */
		struct slice_t *slice = (struct slice_t *) (out_space + output_size);
		output_size += sizeof(*slice) / sizeof(u64) + m;

		slice->n = m;
		slice->l = k;
		bool ok = false;
		int n_trials = 0;
		while (!ok) {
			u64 T[64];
			for (u64 i = 0; i < 64 - k; i++)
				T[i] = myrand();
			for (u32 i = 0; i < k; i++)
				T[64 - k + i] = equations[i];

			transpose_64(T, slice->M);
			ok = invert(slice->M, slice->Minv);
			n_trials++;

			if (n_trials > 1000) {
				for (u32 i = 0; i < k; i++)
					printf("eq[%d] = %016" PRIx64 "\n", i, equations[i]);
				errx(3, "the impossible happened");
			}
		}

		u32 i = 0;
		for (struct list_t *item = active->next; item != active; item = item->next)
			slice->CM[i++] = naive_gemv(item->x, slice->M);

		/* The active (=good) vectors are discarded. The rejected vectors become active again for the next pass. */
		struct list_t *tmp = active;
		active = reject;
		reject = tmp;
		list_clear(reject);
		vectors_done += m;
		slices_done++;
		n -= m;
		m = n;
	}

	if (!QUIET && multi_mode)
		printf("Process %d, writing\n", rank);

	/* the slice files must be little-endian. If I'm big-endian (=turing), I swap */
	if (big_endian()) {
		for (u32 i = 0; i < output_size; i++)
			out_space[i] = bswap_64(out_space[i]);
	}

	size_t check = fwrite(out_space, 8, output_size, f_out);
	if (check != output_size)
		err(1, "fwrite inconsistensy %zd vs %d", check, size);
	int rc = fclose(f_out);
	if (rc != 0)
		err(1, "cannot close output");

	if (multi_mode) {
		if (!QUIET)
			printf("Process %d, over and out\n", rank);
		MPI_Finalize();
	}
	exit(EXIT_SUCCESS);
}
