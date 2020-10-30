#define _XOPEN_SOURCE 500   /* strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "preprocessing.h"

int cmp(const void *a_, const void *b_)
{
        struct dict_t *a = (struct dict_t *) a_;
        struct dict_t *b = (struct dict_t *) b_;
        return (a->hash > b->hash) - (a->hash < b->hash);
}

int main(int argc, char **argv)
{
        if (argc < 2)
                errx(1, "missing argument: FILENAME");
        char *in_filename = argv[1];

        struct stat infos;
        if (stat(in_filename, &infos))
                err(1, "fstat");
        struct dict_t *dictionnary = malloc(infos.st_size);
        if (dictionnary == NULL)
                err(1, "failed to allocate memory");
        FILE *f_in = fopen(in_filename, "r");
        if (f_in == NULL)
                err(1, "fopen failed");
        size_t check = fread(dictionnary, 1, infos.st_size, f_in);
        if ((check != (size_t) infos.st_size) || ferror(f_in))
                err(1, "fread : read %zd, expected %zd", check, infos.st_size);
        if (fclose(f_in))
                err(1, "fclose %s", in_filename);

        int n_entries = infos.st_size / sizeof(*dictionnary);
        qsort(dictionnary, n_entries, sizeof(*dictionnary), cmp);

        char *out_filename = strdup(in_filename);
        memcpy(out_filename + strlen(out_filename) - 8, "sorted", 7);
        FILE *f_out = fopen(out_filename, "w");
        if (f_out == NULL)
                err(1, "cannot create output file %s", out_filename);
        check = fwrite(dictionnary, sizeof(*dictionnary), n_entries, f_out);
        if (check != (size_t) n_entries)
                err(1, "fwrite inconsistensy %zd vs %d", check, n_entries);
        if (fclose(f_out))
                err(1, "fclose %s", out_filename);

        exit(EXIT_SUCCESS);
}


