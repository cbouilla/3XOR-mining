#include <stdio.h>
#include <stdlib.h>
#include <err.h>

#include "preprocessing.h"

int main(int argc, char **argv)
{
        if (argc < 2)
                errx(1, "missing input filename");
        FILE *f = fopen(argv[1], "r");
        if (f == NULL)
                err(1, "cannot open %s for reading", argv[1]);
        static const u32 BUFFER_SIZE = 131072;
        u64 *buffer = malloc(BUFFER_SIZE * sizeof(*buffer));
        if (buffer == NULL)
                err(1, "cannot allocate buffer");


        u64 prev = 0;
        u32 processed = 0, size = 0;
        while (!feof(f)) {
                processed += size;
                size = fread(buffer, sizeof(*buffer), BUFFER_SIZE, f);
                if (ferror(f))
                        err(1, "fread failed");

                for (u32 i = 0; i < size; i++) {
                        if (prev >= buffer[i])
                                errx(1, "F[%d] (%016" PRIx64 ") >= F[%d] (%016" PRIx64 ")", 
                                          processed + (i-1), prev, processed + i, buffer[i]);
                        prev = buffer[i];
                }

        }
        fclose(f);
        exit(EXIT_SUCCESS);
}



