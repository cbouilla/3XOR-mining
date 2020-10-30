#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <err.h>

#include "preprocessing.h"
#include "hasher.h"

int main(int argc, char **argv)
{
        struct option longopts[2] = {
                {"partitioning-bits", required_argument, NULL, 'b'},
                {NULL, 0, NULL, 0}
        };
        int bits = -1;
        signed char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'b':
                        bits = atoi(optarg);
                        break;
                default:
                        errx(1, "Unknown option\n");
                }
        }
        if (bits == -1) 
                errx(1, "missing required option --partitioning-bits");
        if (optind != argc - 1)
                errx(1, "missing (or extra) filenames");
        char *in_filename = argv[optind];
        enum kind_t kind = file_get_kind(in_filename);
        u32 partition = file_get_partition(in_filename);
        printf("Input has kind %d and partition key %03x.\n", kind, partition);

        FILE *f = fopen(in_filename, "r");
        if (f == NULL)
                err(1, "cannot open %s for reading", in_filename);
        static const u32 BUFFER_SIZE = 131072;
        struct dict_t *buffer = malloc(BUFFER_SIZE * sizeof(*buffer));
        if (buffer == NULL)
                err(1, "cannot allocate buffer");


        struct dict_t prev = {0, {0, 0}};
        u32 processed = 0, size = 0;
        bool in_order = true;
        u32 duplicates = 0, collisions = 0;
        while (!feof(f)) {
                processed += size;
                size = fread(buffer, sizeof(*buffer), BUFFER_SIZE, f);
                if (ferror(f))
                        err(1, "fread failed");

                for (u32 i = 0; i < size; i++) {
                        u32 full_hash[8];
                        if (!compute_full_hash(kind, &buffer[i].preimage, full_hash))
                                errx(1, "invalid preimage %d", processed + i);
                        if (buffer[i].hash != extract_partial_hash(full_hash))
                                errx(1, "partial hash mismatch %d", processed + i);
                        if (partition != extract_partitioning_key(bits, full_hash))
                                errx(1, "partitioning key changed");
                        if (prev.preimage.counter == buffer[i].preimage.counter 
                                  && prev.preimage.nonce == buffer[i].preimage.nonce)
                                duplicates++;
                        if (prev.hash == buffer[i].hash)
                                collisions++;
                        if (prev.hash > buffer[i].hash)
                                in_order = false;
                        prev = buffer[i];
                }

        }
        processed += size;
        printf("%d items processed.\n", processed);
        printf("In order: %d\n", in_order);
        printf("Hash collisions: %d\n", collisions - duplicates);
        printf("Duplicate preimages: %d\n", duplicates);
        fclose(f);
        exit(EXIT_SUCCESS);


}



