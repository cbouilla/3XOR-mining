#include <stdio.h>
#include "../types.h"
#include "../preprocessing/preprocessing.h"


struct jtask_t {
	u64 *L[2];
	u64 n[2];
	struct slice_t *slices;
	u64 slices_size;        /* in u64, not in bytes */
};

struct solution_t {
	u64 val[3];
	u64 task_index[3];
};

struct task_result_t {
	u32 size;
	u32 capacity;
	struct solution_t *solutions;
};

#define MAX(x, y) (((x) < (y)) ? (y) : (x))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

// static inline u64 LEFT_MASK(u32 n)
// {
//      return ~((1ull << (64 - n)) - 1);
// }

static inline u64 RIGHT_MASK(u32 n)
{
	return (1ull << n) - 1;
}

double wtime();
u64 cycles();
long long usec();

bool big_endian();
void *aligned_alloc(size_t alignment, size_t size);
void *load_file(const char *filename, u64 * size_);
struct task_result_t *result_init();
void report_solution(struct task_result_t *result, const struct solution_t *solution);
void result_free(struct task_result_t *result);

/* the task processing function -- in joux_v3.c */
struct task_result_t *iterated_joux_task(struct jtask_t *task, const u32 *task_index);
