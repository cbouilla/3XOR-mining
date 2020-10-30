#define _XOPEN_SOURCE 500
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <getopt.h>
#include <mpi.h>
#include "preprocessing.h"
#include "hasher.h"


struct option longopts[3] = {
        {"partitioning-bits", required_argument, NULL, 'b'},
        {"output-dir", required_argument, NULL, 'd'},
        {NULL, 0, NULL, 0}
};
int bits = -1;
char *output_dir = NULL;

i32 rank, size;

static const int READER_REQUEST_TAG = 0;
static const int NONCE_BLOCK_TAG = 1;
static const int HASH_BLOCK_TAG = 2;
static const int EOF_TAG = 3;
static const int KEY_TAG = 4;
u32 n_mapper, n_slots;

static const u32 READER_BUFFER_SIZE = 65536;
static const u32 WRITER_BUFFER_SIZE = 1024;

int main(int argc, char **argv)
{
        MPI_Init(&argc, &argv);
        MPI_Comm_size(MPI_COMM_WORLD, &size);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        struct preimage_t sample[2];
        MPI_Datatype PreimageStruct, PreimageType;
        MPI_Datatype type[2] = {MPI_UINT64_T, MPI_UINT32_T};
        int blocklen[2] = {1, 1};
        MPI_Aint disp[2];
        MPI_Aint base, sizeofentry;

        /* compute displacements of structure components */
        MPI_Get_address(&sample[0].counter, &disp[0]);
        MPI_Get_address(&sample[0].nonce, &disp[1]);
        MPI_Get_address(sample, &base);
        disp[0] -= base;
        disp[1] -= base;
        MPI_Type_create_struct(2, blocklen, disp, type, &PreimageStruct);
        MPI_Type_commit(&PreimageStruct);

        /* If compiler does padding in mysterious ways, the following may be safer */
        MPI_Get_address(sample + 1, &sizeofentry);
        sizeofentry -= base;
        MPI_Type_create_resized(PreimageStruct, 0, sizeofentry, &PreimageType);

        /* quick safety check */
        int x;
        MPI_Type_size(PreimageType, &x);
        if ((x != sizeof(struct preimage_t)) || (x != 12))
                errx(1, "data types size mismatch");


        struct dict_t sample2[2];
        MPI_Datatype type2[2] = {MPI_UINT64_T, PreimageType};
        MPI_Datatype DictStruct, DictType;

        /* compute displacements of structure components */
        MPI_Get_address(&sample2[0].hash, &disp[0]);
        MPI_Get_address(&sample2[0].preimage, &disp[1]);
        MPI_Get_address(sample2, &base);
        disp[0] -= base;
        disp[1] -= base;
        MPI_Type_create_struct(2, blocklen, disp, type2, &DictStruct);
        MPI_Type_commit(&DictStruct);

        MPI_Get_address(sample2 + 1, &sizeofentry);
        sizeofentry -= base;
        MPI_Type_create_resized(DictStruct, 0, sizeofentry, &DictType);




        signed char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'b':
                        bits = atoi(optarg);
                        break;
                case 'd':
                        output_dir = optarg;
                        break;
                default:
                        errx(1, "Unknown option\n");
                }
        }
        if (bits == -1) 
                errx(1, "missing required option --partitioning-bits");
        if (optind != argc - 1)
                errx(1, "missing (or extra) filenames");
        if (output_dir == NULL)
                errx(1, "missing required option --output-dir");
        char *in_filename = argv[optind];
        enum kind_t kind = file_get_kind(in_filename);

        n_mapper = size - 2;
        n_slots = 1 << bits;
        if (rank == 0 && n_mapper <= 0)
                errx(1, "not enough MPI processes. Need 3, have %d", size);

        if (rank == 0) {
                printf("Reader started. %d mappers.\n", n_mapper);
                u32 preimages_read = 0;
                double start = MPI_Wtime();
                double wait = 0;
                FILE *f = fopen(in_filename, "r");
                if (f == NULL)
                        err(1, "fopen on %s", in_filename);
                while (1) {
                        struct preimage_t buffer[READER_BUFFER_SIZE];
                        size_t n_items = fread(buffer, sizeof(struct preimage_t), READER_BUFFER_SIZE, f);
                        if (ferror(f))
                                err(1, "fread in reader");
                        if (n_items == 0 && feof(f))
                                break;

                        MPI_Status status;
                        double wait_start = MPI_Wtime();
                        MPI_Recv(NULL, 0, MPI_INT, MPI_ANY_SOURCE, READER_REQUEST_TAG, MPI_COMM_WORLD, &status);
                        wait += MPI_Wtime() - wait_start;
                        MPI_Send(buffer, n_items, PreimageType, status.MPI_SOURCE, NONCE_BLOCK_TAG, MPI_COMM_WORLD);
                        preimages_read += n_items;

                        double megabytes = preimages_read * 1.1444091796875e-05;
                        double rate = megabytes / (MPI_Wtime() - start);
                        printf("\rPreimages read: %d (%.1f Mb, %.1f Mb/s)", preimages_read, megabytes, rate);
                        fflush(stdout);



                }
                fclose(f);
                for (u32 i = 0; i < n_mapper; i++) {
                        MPI_Status status;
                        MPI_Recv(NULL, 0, MPI_INT, MPI_ANY_SOURCE, READER_REQUEST_TAG, MPI_COMM_WORLD, &status);
                        MPI_Send(NULL, 0, MPI_INT, status.MPI_SOURCE, EOF_TAG, MPI_COMM_WORLD);


                }
                printf("\nReader finished. %d preimages read, total wait = %.1f s\n", preimages_read, wait);


        } else if (rank == 1) {
                u32 n_eof = 0;
                FILE * f[n_slots];
                for (u32 i = 0; i < n_slots; i++) {
                        char out_filename[255];
                        char *input_base = basename(in_filename);
                        sprintf(out_filename, "%s/%03x/%s.unsorted", output_dir, i, input_base);
                        f[i] = fopen(out_filename, "w");
                        if (f[i] == NULL)
                                err(1, "[writer] Cannot open %s for writing", out_filename);
                }

                printf("Writer ready\n");
                while (n_eof < n_mapper) {
                        MPI_Status status;
                        u32 slot;
                        struct dict_t block[WRITER_BUFFER_SIZE];
                        MPI_Recv(&slot, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                        if (status.MPI_TAG == EOF_TAG) {
                                n_eof++;
                                continue;
                        }
                        MPI_Recv(block, WRITER_BUFFER_SIZE, DictType, status.MPI_SOURCE, 
                                                        HASH_BLOCK_TAG, MPI_COMM_WORLD, &status);
                        i32 n_entries;
                        MPI_Get_count(&status, DictType, &n_entries);

                        size_t tmp = fwrite(block, sizeof(struct dict_t), n_entries, f[slot]);
                        if (tmp != (size_t) n_entries)
                                err(1, "fwrite writer (file %03x): %zd vs %d", slot, tmp, n_entries);

                }
                for (u32 i = 0; i < n_slots; i++)
                        if (fclose(f[i]))
                                err(1, "fclose writer file %03x", i);


                printf("Writer done.\n");

        } else {
                int id = rank - 2;
                struct dict_t * output[n_slots];
                u32 output_size[n_slots];
                for (u32 i = 0; i < n_slots; i++) {
                        output_size[i] = 0;
                        output[i] = malloc(WRITER_BUFFER_SIZE * sizeof(struct dict_t));
                        if (output[i] == NULL)
                                err(1, "cannot alloc mapper output buffer");
                }


                u32 n_processed = 0, n_invalid = 0;
                while (1) {
                        struct preimage_t preimages[READER_BUFFER_SIZE];
                        MPI_Status status;
                        MPI_Send(NULL, 0, MPI_INT, 0, READER_REQUEST_TAG, MPI_COMM_WORLD);
                        MPI_Recv(preimages, READER_BUFFER_SIZE, PreimageType, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                        if (status.MPI_TAG == EOF_TAG)
                                break;
                        i32 n_preimages;
                        MPI_Get_count(&status, PreimageType, &n_preimages);


                        for (int i = 0; i < n_preimages; i++) {
                                u32 full_hash[8];
                                if (!compute_full_hash(kind, preimages + i, full_hash)) {
                                        n_invalid++;
                                        continue;
                                }

                                u64 x = extract_partial_hash(full_hash);
                                u32 slot = extract_partitioning_key(bits, full_hash);
                                output[slot][output_size[slot]].hash = x;
                                output[slot][output_size[slot]].preimage.counter = preimages[i].counter;
                                output[slot][output_size[slot]].preimage.nonce = preimages[i].nonce;
                                output_size[slot] += 1;

                                if (output_size[slot] == WRITER_BUFFER_SIZE) {
                                        MPI_Send(&slot, 1, MPI_INT, 1, KEY_TAG, MPI_COMM_WORLD);
                                        MPI_Send(output[slot], output_size[slot], DictType, 1, HASH_BLOCK_TAG, MPI_COMM_WORLD);
                                        n_processed += output_size[slot];
                                        output_size[slot] = 0;


                                }
                        }
                }
                for (u32 slot = 0; slot < n_slots; slot++) {
                        MPI_Send(&slot, 1, MPI_INT, 1, KEY_TAG, MPI_COMM_WORLD);
                        MPI_Send(output[slot], output_size[slot], DictType, 1, HASH_BLOCK_TAG, MPI_COMM_WORLD);
                        n_processed += output_size[slot];
                        output_size[slot] = 0;


                }
                MPI_Send(NULL, 0, MPI_INT, 1, EOF_TAG, MPI_COMM_WORLD);

                printf("Mapper %d finished. %d dictionnary entries transmitted. %d invalid.\n", id, n_processed, n_invalid);

        }


        MPI_Finalize();
        exit(EXIT_SUCCESS);
}


