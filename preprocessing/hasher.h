#include <string.h>
#include "preprocessing.h"

static inline void build_plaintext_block(int kind, struct preimage_t *preimage, char *buffer) {
        static const char *TEMPLATE[3] = {
        "FOO-0x0000000000000000                                                          ",
        "BAR-0x0000000000000000                                                          ",
        "FOOBAR-0x0000000000000000                                                       "
        };
        static const u8 NIBBLE[16] = {48, 49, 50, 51, 52, 53, 54, 55, 
                                        56, 57, 65, 66, 67, 68, 69, 70};
        memcpy(buffer, TEMPLATE[kind], 80);
                
        u64 counter = preimage->counter;
        int j = (kind == 2) ? 25 : 22;
        while (counter > 0) {
                u8 nibble = counter & 0x000f;
                counter >>= 4;
                buffer[j] = NIBBLE[nibble];
                j--;
        }

        u32 *block = (u32 *) buffer;
        block[19] = __builtin_bswap32(preimage->nonce);


}
extern bool compute_full_hash(int kind, struct preimage_t *preimage, u32 *hash);
extern void compute_middle_hash(int kind, struct preimage_t *preimage, u32 *hash);

static inline u64 extract_partial_hash(u32 *hash) {
        return (((u64) hash[5]) << 32) ^ hash[6] ^ (hash[4] & 0x80000000);

}
static inline u64 extract_partitioning_key(int k, u32 *hash) {
        return (hash[4] & 0x7fffffff) >> (31 - k);

}


