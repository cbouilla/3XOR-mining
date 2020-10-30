#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <getopt.h>
#include <string.h>

#include "common.h"

#include <omp.h>
#include <pthread.h>
#include <papi.h>

/* in joux.c */
struct task_result_t * iterated_joux_task_v3(const char *hash_dir, const char *slice_dir, struct jtask_id_t *task, u32 p);

int main(int argc, char **argv)
{
        int rc = PAPI_library_init(PAPI_VER_CURRENT);
        if (rc != PAPI_VER_CURRENT) 
                errx(1, "PAPI version mismatch");
        rc = PAPI_thread_init(pthread_self);
        if (rc != PAPI_OK)
                errx(1, "PAPI_thread_init: %d (%s)\n", rc, PAPI_strerror(rc));
        #pragma omp parallel
        {
                int events[2] = {PAPI_TOT_CYC, PAPI_TOT_INS};
                rc = PAPI_start_counters(events, 2);
                if (rc < PAPI_OK)
                                errx(1, "PAPI_start_counters : tid=%d, rc = %d, %s", 
                                        omp_get_thread_num(), rc, PAPI_strerror(rc));
                printf("started on tid=%d\n", omp_get_thread_num());
        }

        struct option longopts[7] = {
                {"partitioning-bits", required_argument, NULL, 'k'},
                {"fine-task-size", required_argument, NULL, 'f'},
                {"hash-dir", required_argument, NULL, 'h'},
                {"slice-dir", required_argument, NULL, 's'},
                {"stage1-bits", required_argument, NULL, 'p'},
                {NULL, 0, NULL, 0}
        };
        u32 k = 0xffffffff;
        u32 k2 = 0;
        // u32 p = 0;
        // u32 q = 0;
        char *hash_dir = NULL;
        char *slice_dir = NULL;
        signed char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'k':
                        k = atoi(optarg);
                        break;
                case 'f':
                        k2 = atoi(optarg);
                        break;
                case 'h':
                        hash_dir = optarg;
                        break;
                case 's':
                        slice_dir = optarg;
                        break;
        //      case 'p':
        //              p = atoi(optarg);
        //              break;
        //      case 'q':
        //              q = atoi(optarg);
        //              break;
                default:
                        errx(1, "Unknown option\n");
                }
        }
        if (k == 0xffffffff)
                errx(1, "missing option --partitioning-bits");
        if (k2 == 0)
                errx(1, "missing option --fine-task-size");
        if (hash_dir == NULL)
                errx(1, "missing option --hash-dir");
        if (slice_dir == NULL)
                errx(1, "missing option --slice-dir");
        // if (p == 0)
        //      errx(1, "missing option --stage1-bits");
        // if (q == 0)
        //      errx(1, "missing option --stage2-bits");


        u32 problem_size = 1 << k;
        for (u32 i = 0; i < problem_size; i++)
                for (u32 j = 0; j < problem_size; j++)
                        for (u32 r = 0; r < k2; r++) {
                                struct jtask_id_t task;
                                task.k = k;
                                task.k2 = k2;
                                task.idx[0] = i;
                                task.idx[1] = j;
                                task.idx[2] = i ^ j;
                                task.r = r;
                                printf("task (%d, %d, %d) : ", i, j, r);
                                fflush(stdout);
                                struct task_result_t *solutions = 
                                  iterated_joux_task_v3(hash_dir, slice_dir, &task, 10);
                                printf("%d solutions\n", solutions->size);
                                for (u32 u = 0; u < solutions->size; u++)
                                        printf("%016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " == 0\n",
                                                        solutions->solutions[u].x, 
                                                        solutions->solutions[u].y, 
                                                        solutions->solutions[u].z);
                                result_free(solutions);
                        }


}



