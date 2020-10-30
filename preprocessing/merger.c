#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <err.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>

#include "preprocessing.h"

double wtime()
{
        struct timeval ts;
        gettimeofday(&ts, NULL);
        return (double)ts.tv_sec + ts.tv_usec / 1E6;
}


int main(int argc, char **argv)
{
        struct option longopts[2] = {
                {"output", required_argument, NULL, 'o'},
                {NULL, 0, NULL, 0}
        };
        char *out_filename = NULL;
        signed char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'o':
                        out_filename = optarg;
                        break;
                default:
                        errx(1, "Unknown option\n");
                }
        }
        if (out_filename == NULL)
                errx(1, "missing --output FILE");
        if (optind >= argc)
                errx(1, "missing input filenames");
        u32 P = argc - optind;
        char **in_filenames = argv + optind;


        FILE *f_in[P];
        for (u32 i = 0; i < P; i++) {
                f_in[i] = fopen(in_filenames[i], "r");
                if (f_in[i] == NULL)
                        err(1, "cannot open %s for reading", in_filenames[i]);
        }

        FILE *f_out = fopen(out_filename, "w");
        if (f_out == NULL)
                err(1, "cannot open %s for writing", out_filename);

        static const u64 SENTINEL = 0xffffffffffffffffull;
        static const u32 OUT_BUFFER_SIZE = 131072;
        static const u32 IN_BUFFER_SIZE = 52428;
        struct dict_t *buffer_in[P];
        u32 size_in[P], ptr_in[P];
        for (u32 i = 0; i < P; i++) {
                buffer_in[i] = malloc(IN_BUFFER_SIZE * sizeof(struct dict_t));
                if (buffer_in[i] == NULL)
                        err(1, "malloc failed (input buffer)");
                size_in[i] = 0;
                ptr_in[i] = 0;
        }
        u64 *buffer_out = malloc(OUT_BUFFER_SIZE * sizeof(*buffer_out));
        if (buffer_out == NULL)
                err(1, "malloc failed (output buffer)");
        u32 ptr_out = 0;
        u32 flushed = 0;
        double start = wtime();

        struct dict_t loser[P];
        u32 origin[P];
        for (u32 j = 0; j < P; j++) {
                loser[j].hash = 0;
                loser[j].preimage.counter = 0;
                loser[j].preimage.nonce = 0;
                origin[j] = j;
        }
        struct dict_t Q = {0, {0, 0}}, Q_prev = {0, {0, 0}};
        u32 i = 0, collisions = 0, duplicates = 0;

        while (Q.hash < SENTINEL) {
                if (ptr_out == OUT_BUFFER_SIZE) {
                        flushed += ptr_out;
                        size_t check_out = fwrite(buffer_out, sizeof(*buffer_out), ptr_out, f_out);
                        if (check_out != (size_t) ptr_out)
                                err(1, "fwrite inconsistency : %zd vs %d", check_out, ptr_out);
                        ptr_out = 0;
                        double mhash = flushed * 1e-6;
                        double rate = mhash / (wtime() - start);
                        printf("\rItem processed: %.1fM (%.1fM/s) ", mhash, rate);
                        printf("hash collisions=%d, duplicate=%d, ", collisions, duplicates - P);
                        printf("IN=%.1f Mb/s   OUT =%.1f Mb/s", 20*rate, 8*rate);
                        fflush(stdout);

                }
                if (Q.hash != Q_prev.hash) {
                        buffer_out[ptr_out++] = Q.hash;
                } else {
                        /* diagnose duplicate */
                        if ((Q.preimage.counter == Q_prev.preimage.counter) 
                                                && (Q.preimage.nonce == Q_prev.preimage.nonce))
                                duplicates++;
                        else
                                collisions++;
                }
                Q_prev = Q;


                if (ptr_in[i] == size_in[i]) {
                        size_in[i] = fread(buffer_in[i], sizeof(struct dict_t), IN_BUFFER_SIZE, f_in[i]);
                        if (ferror(f_in[i]))
                                err(1, "fread on %s", in_filenames[i]);
                        ptr_in[i] = 0;
                }

                if (size_in[i] == 0)
                        Q.hash = SENTINEL;
                else
                        Q = buffer_in[i][ptr_in[i]++];

                int T = (P + i) / 2;
                while (T >= 1) {
                        if (loser[T].hash < Q.hash) {
                                struct dict_t foo = loser[T];
                                int bar = origin[T];
                                loser[T] = Q;
                                origin[T] = i;
                                Q = foo;
                                i = bar;
                        }
                        T = T / 2;
                }

        }
        flushed += ptr_out;
        size_t check_out = fwrite(buffer_out, sizeof(*buffer_out), ptr_out, f_out);
        if (check_out != (size_t) ptr_out)
                err(1, "fwrite inconsistency : %zd vs %d", check_out, ptr_out);
        ptr_out = 0;
        double mhash = flushed * 1e-6;
        double rate = mhash / (wtime() - start);
        printf("\rItem processed: %.1fM (%.1fM/s) ", mhash, rate);
        printf("hash collisions=%d, duplicate=%d, ", collisions, duplicates - P);
        printf("IN=%.1f Mb/s   OUT =%.1f Mb/s", 20*rate, 8*rate);
        fflush(stdout);

        printf("\n");

        for (u32 i = 0; i < P; i++)
                if (fclose(f_in[i]))
                        err(1, "fclose on %s", in_filenames[i]);
        if (fclose(f_out))
                err(1, "fclose on %s", out_filename);


        exit(EXIT_SUCCESS);
}


