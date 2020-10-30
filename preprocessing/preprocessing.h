#ifndef _PREPROCESSING_H
#define _PREPROCESSING_H
#include "../types.h"

enum kind_t { 
        FOO, 
        BAR, 
        FOOBAR
};

struct preimage_t {
        i64 counter;
        u32 nonce;
} __attribute__((packed));

struct dict_t {
        u64 hash;
        struct preimage_t preimage;
} __attribute__((packed));

struct slice_t {
        u64 M[64];
        u64 Minv[64];
        u64 n;
        u64 l;
        u64 CM[];
}  __attribute__((packed));

static inline u64 LEFT_MASK(u8 n)
{
        return ~((1ull << (64 - n)) - 1);
}


enum kind_t file_get_kind(const char *filename);
u32 file_get_partition(const char *filename);
#endif

