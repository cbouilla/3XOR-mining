#include "sha256.h"
#include "hasher.h"

bool compute_full_hash(int kind, struct preimage_t *preimage, u32 *hash)
{
        u32 *block[20];
        build_plaintext_block(kind, preimage, (char *) block);
        u8 md[32];
        SHA256((u8 *) block, 80, md);
        SHA256((u8 *) md, 32, (u8 *) hash);

        return (hash[7] == 0x00000000) && ((hash[6] & 0x80000000) == 0x0000000000);
}

void compute_middle_hash(int kind, struct preimage_t *preimage, u32 *hash)
{
        u32 *block[20];
        build_plaintext_block(kind, preimage, (char *) block);
        SHA256((u8 *) block, 80, (u8 *) hash);
}
