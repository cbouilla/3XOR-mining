#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <err.h>
#include "hasher.h"

        
int cmp(const void *a_, const void *b_)
{
        const u64 *const a = (u64 *) a_;
        const u64 *const b = (u64 *) b_;
        return (*a > *b) - (*a < *b);
}

int main(int argc, char **argv)
{
        if (argc < 2)
                errx(1, "missing argument N");
        u32 avg_size = atoi(argv[1]);

        u32 N[3];
        u64 *H[3];
        srand48(1337);
        for (u32 k = 0; k < 3; k++) {
                N[k] = avg_size / 2 + (((u32) mrand48()) % avg_size);
                printf("H[%d] has size %d\n", k, N[k]);
                H[k] = malloc(N[k] * sizeof(u64));
                if (H[k] == NULL)
                        err(1, "cannot allocate hashes");
                struct preimage_t pre;
                pre.nonce = 0;
                pre.counter = 0;
                u32 ptr = 4;
                u32 hash[8];
                u64 *randomness = (u64 *) hash;
                for (u32 n = 0; n < N[k]; n++) {
                        if (ptr == 4) {
                                compute_full_hash(k, &pre, hash);
                                pre.counter++;
                                ptr = 0;
                        }
                        H[k][n] = randomness[ptr++];
                }
        }

        qsort(H[1], N[1], sizeof(u64), cmp);
        qsort(H[2], N[2], sizeof(u64), cmp);

        u32 b = ((u32) mrand48()) % N[1];
        u32 c = ((u32) mrand48()) % N[2];
        H[0][0] = H[1][0] ^ H[2][0];
        H[0][1] = H[1][N[1] - 1] ^ H[2][1];
        H[0][2] = H[1][b] ^ H[2][c];
        u64 x[3] = { H[0][0], H[0][1], H[0][2] };
        printf("%016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " = 0\n", 
                x[0], H[1][0], H[2][0]);
        printf("%016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " = 0\n", 
                x[1], H[1][N[1] - 1], H[2][N[2] - 1]);
        printf("%016" PRIx64 " ^ %016" PRIx64 " ^ %016" PRIx64 " = 0\n", 
                x[2], H[1][b], H[2][c]);

        qsort(H[0], N[0], sizeof(u64), cmp);

        u32 y[3];
        for (u32 k = 0; k < 3; k++) {
                for (u32 d = 0; d < N[0]; d++)
                        if (H[0][d] == x[k]) {
                                y[k] = d;
                                break;
                        }
        }
        printf("H[0][%08x] ^ H[1][%08x] ^ H[2][%08x] = 0\n", y[0], 0, 0);
        printf("H[0][%08x] ^ H[1][%08x] ^ H[2][%08x] = 0\n", y[1], N[1] - 1, N[2] - 1);
        printf("H[0][%08x] ^ H[1][%08x] ^ H[2][%08x] = 0\n", y[2], b, c);


        char *names[3] = {"foo.000", "bar.000", "foobar.000"};
        for (u32 k = 0; k < 3; k++) {
                FILE *f = fopen(names[k], "w");
                if (f == NULL)
                        err(1, "cannot open %s for writing", names[k]);
                size_t check = fwrite(H[k], sizeof(uint64_t), N[k], f);
                if (check != N[k])
                        errx(1, "incomplete write");
                fclose(f);
        }

        exit(EXIT_SUCCESS);
}


