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
       #include <search.h>

#include "preprocessing.h"

u64 * load(const char *filename, u64 *size_)
{
	struct stat infos;
	if (stat(filename, &infos))
		err(1, "fstat failed on %s", filename);
	u64 size = infos.st_size;
	assert ((size % 8) == 0);
	u64 *content = aligned_alloc(64, size);
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
	return content;
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

int cmp(const void *a_, const void *b_)
{
        u64 *a = (u64 *) a_;
        u64 *b = (u64 *) b_;
        // if (a < b)
        // 	return -1;
        // if (a > b)
        // 	return 1;
        // return 0;
        // printf("cmp %016" PRIx64 " vs %016" PRIx64 "\n", a, b);
        return (*a > *b) - (*a < *b);
}

static bool binary_search(u64 x, const u64 *L, u64 size)
{
	u64 mid = size / 2;
	if (x == L[mid])
		return true;
	if (size == 1)
		return false;
	if (x < L[mid])
		return binary_search(x, L, mid);
	return binary_search(x, L + mid, size - mid);
}

int main(int argc, char **argv)
{
	/* process command-line options */
	struct option longopts[4] = {
		{"hash", required_argument, NULL, 'h'},
		{"slice", required_argument, NULL, 's'},
		{"l", required_argument, NULL, 'l'},
		{NULL, 0, NULL, 0}
	};
	char *hash_filename = NULL;
	char *slice_filename = NULL;
	signed char ch;
	i64 l = -1;
	while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			hash_filename = optarg;
			break;
		case 's':
			slice_filename = optarg;
			break;
		case 'l':
			l = atoi(optarg);
			break;
		default:
			errx(1, "Unknown option\n");
		}
	}
	if (hash_filename == NULL)
		errx(1, "missing --hash");
	if (slice_filename == NULL)
		errx(1, "missing --slice");
	if (l < 0)
		errx(1, "missing --l");
	
	printf("Loading hashes...");
	fflush(stdout);
	u64 n_hashes;
	u64 *L = load(hash_filename, &n_hashes);
	printf("%" PRId64 "\n", n_hashes);

	printf("Sorting hashes\n");
	qsort(L, n_hashes, sizeof(*L), cmp);

	
	printf("Loading slices...");
	fflush(stdout);
	u64 slices_size;
	struct slice_t * slice = (struct slice_t *) load(slice_filename, &slices_size);
	u64 *slice_end = ((u64 *) slice) + slices_size;
	printf("%" PRId64 "\n", slices_size);

	u64 * M = malloc(n_hashes * sizeof(*M));
	u64 j = 0;

	/* process all slices: check M*Minv = I, copy vectors into M. */
	u64 i = 0;
	while ((u64 *) slice < slice_end) {
		assert(slice->l >= (u64) l);

		u64 mask = LEFT_MASK(slice->l);
		u64 n = slice->n;

		for (u64 k = 0; k < 64; k++) {
			assert(naive_gemv(slice->M[k], slice->Minv) == 1ull << k);
			assert(naive_gemv(slice->Minv[k], slice->M) == 1ull << k);
		}

		for (u64 k = 0; k < n; k++) {
			assert((slice->CM[k] & mask) == 0);
			u64 z = naive_gemv(slice->CM[k], slice->Minv);
			if (!binary_search(z, L, n_hashes)) {
				printf("Mismatch in slice #%" PRId64 ": item %" PRId64 " (%016" PRIx64 ") is not in L\n", i, k, z);
				exit(EXIT_FAILURE);
			}
			M[j++] = z;
		}

		/* advance to next slice */
		i++;
		u64 *ptr = (u64 *) slice + sizeof(struct slice_t)/8 + n;
		slice = (struct slice_t *) ptr;
	}

	assert(j == n_hashes);

	qsort(M, n_hashes, sizeof(u64), cmp);
	for (u64 i = 0; i < n_hashes; i++)
		if (L[i] != M[i]) {
			printf("Mismatch at offset #%" PRId64 ": L[] = %" PRIx64 " vs M[] = %016" PRIx64 "\n", i, L[i], M[i]);
			exit(EXIT_FAILURE);
		}

	printf("Average slice size: %.1f\n", (double) n_hashes / (double) i);
	exit(EXIT_SUCCESS);
}