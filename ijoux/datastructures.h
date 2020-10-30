#include "../types.h"

/* functions for hash tables of 256 elements */

static const u32 CM_HASH_SIZE = 512; /* 4 Kbyte */
static const u64 CM_HASH_MASK = 511;

/* two hash functions especially optimized for BG/Q */
static inline u64 H0(u64 x)
{
    return x & CM_HASH_MASK;
}

static inline u64 H1(u64 x)
{
    return (x >> 16) & CM_HASH_MASK;
}

/* insert x into the H (hash table with linear probing) */
static inline void linear_insert(u64 *H, u64 x)
{
    u64 h = H0(x);
    while (H[h])
        h = (h + 1) & CM_HASH_MASK;
    H[h] = x;
}

/* Does x belongs to H? (hash table with linear probing) */
static inline bool linear_lookup(const u64 *H, const u64 x)
{
    u64 h = H0(x);
    while (1) {
        u64 probe = H[h];
        h = (h + 1) & CM_HASH_MASK;
        if (probe == x)
            return true;
        if (probe == 0)
            return false;
    }
}

/* Does x belongs to H? (hash table with linear probing) */
static inline bool cuckoo_lookup(const u64 *H, const u64 x)
{
    const u64 h1 = H0(x);
    const u64 h2 = H1(x);
    const u64 probe1 = H[h1];
    const u64 probe2 = H[h2];
    /* Goddamn xlc: DON'T USE  (probe1 == x) | (probe2 == x) */
    return (probe1 == x) || (probe2 == x);
}

/* try to insert x into H (cuckoo table). Returns TRUE in case of success */
static inline bool cuckoo_insert(u64 *H, u64 x)
{
    u64 h = H0(x);
    for (u64 loops = 0; loops < 2*CM_HASH_SIZE; loops++) {
        u64 y = x; x = H[h]; H[h] = y;
        u64 h0 = H0(x);
        u64 h1 = H1(x);
        if (x == 0)
            return true;
        h = (h0 == h) ? h1 : h0;
    }
    return false;
}

/* tries to build a cuckoo table; in case of failure, builds a linear table */
static bool cuckoo_build(const u64 *  L, u32 lo, u32 hi, u64 *H)
{
    bool fail = false;
    for (u32 i = 0; i < CM_HASH_SIZE; i++) 
        H[i] = 0;
    for (u32 i = lo; i < hi; i++) {
        /*if (L[i] == 0)
            errx(1, "cannot insert 0 in hash table"); */
        if (!cuckoo_insert(H, L[i])) { 
            fail = true;
            break;
        }
    }

    if (fail) {
            for (u32 i = 0; i < CM_HASH_SIZE; i++) 
                H[i] = 0;
        for (u32 i = lo; i < hi; i++) {
            /*if (L[i] == 0)
                errx(1, "cannot insert 0 in hash table");*/
            linear_insert(H, L[i]);
        }
    }
    return fail;
}