#define _XOPEN_SOURCE  500  /* for strdup */
#include <string.h>
#include <strings.h>        /* strncasecmp */
#include <stdlib.h>
#include <libgen.h>
#include <err.h>
#include "preprocessing.h"

enum kind_t file_get_kind(const char *filename)
{
        enum kind_t out = -1;
        char *filename_ = strdup(filename);
        char *base = basename(filename_);
        if (strncasecmp(base, "foobar", 6) == 0)
                out = FOOBAR;
        else if (strncasecmp(base, "foo", 3) == 0)
                out = FOO;
        else if (strncasecmp(base, "bar", 3) == 0)
                out = BAR;
        else
                errx(1, "cannot determine kind of file %s", filename);
        free(filename_);
        return out;
}

u32 file_get_partition(const char *filename)
{
        char *filename_ = strdup(filename);
        char *dir = dirname(filename_);
        char *base = basename(dir);
        u32 out = strtol(base, NULL, 16);
        free(filename_);
        return out;
}

