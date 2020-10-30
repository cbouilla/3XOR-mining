import sys
import glob
import subprocess
import os
import os.path
import math

import argparse
parser = argparse.ArgumentParser(description="This script runs all the preprocessing")

parser.add_argument("--partitioning-bits", help="number of bits to split", type=int, required=True, dest="k")
parser.add_argument("--stats", help="display stats", action="store_true")
parser.add_argument("--dry-run", help="print commands but don't run them", action="store_true")
parser.add_argument("--verbose", help="display more information", action="store_true")
parser.add_argument("--cores", help="number of cores of this machine", type=int, default=1)
parser.add_argument("--check", help="run check programs", action="store_true")
parser.add_argument("--slice", help="run the slicer [for joux's algo]", action="store_true")
parser.add_argument("--group", help="run the grouper", type=int, dest="group_size")
args = parser.parse_args()

print("Running with k = {}".format(args.k))
print()


L1_cache_size = 16384

KINDS = {'foo': 0, 'bar': 1, 'foobar': 2}
PREIMAGE_DIR = '../data/preimages'
PREIMAGE_DIR = '../foobar'
DICT_DIR = '../data/dict'
HASH_DIR = '../data/hash'
SLICE_DIR = '../data/slice_alt'
TG_DIR = '../data/task_groups'

SPLITTER = './splitter'
DICT_CHECKER = './dict_checker'
SORTER = './sorter'
MERGER = './merger'
HASH_CHECKER = './hash_checker'
SLICER = './slicer'

SLICE_L = 19

do_split = False
check_split = False
#do_sort = False
#check_sort = False
do_merge = False
check_merge = False

n_preimages = {}
n_dict = {}
n_hash = {}

def ensure_dirs():
    """
    Make sure that the directories for incomming dicts exist.
    """
    for i in range(1 << args.k):
        d = '{}/{:03x}'.format(DICT_DIR, i)
        if not os.path.exists(d):
            os.mkdir(d)

def preimage_stats(verbose=True):
    for kind, kind_id in KINDS.items():
        total = 0
        for file in glob.glob('{}/{}.*'.format(PREIMAGE_DIR, kind)):
            total += os.path.getsize(file)
        n_preimages[kind] = total // 12
    if verbose:
        print('Preimage stats')
        print('==============')
        for kind in KINDS:
            total = n_preimages[kind]
            print("|{:^6}| = {} (2^{:.2f}, {:.1f}M preimages)".format(kind, total, math.log(total, 2), total / 1024**2))
        print()

def dict_stats(verbose=True):
    for kind in KINDS:
        total = 0
        for file in glob.glob('{}/*/{}.*.unsorted'.format(DICT_DIR, kind)):
            total += os.path.getsize(file)
        n_dict[kind] = total // 20
    if verbose:
        print('Dictionary stats')
        print('================')
        for kind in KINDS:
            total = n_dict[kind]
            pre = n_preimages[kind]
            dup = 100 * (pre - total) / pre
            print("|{:^6}| = {} (2^{:.2f}, {:.1f}M dict entries) [invalid rate={:.3f}%]".format(kind, total, math.log(total, 2), total / 1024**2, dup))
        print()

def hash_stats(verbose=True):
    for kind, kind_id in KINDS.items():
        total = 0
        for file in glob.glob('{}/{}.*'.format(HASH_DIR, kind)):
            total += os.path.getsize(file)
        n_hash[kind] = total // 8
    if verbose:
        print('Hash files stats')
        print('================')
        for kind in KINDS:
            total = n_hash[kind]
            d = n_dict[kind]
            dup = 100 * (d - total) / d
            print("|{:^6}| = {} (2^{:.2f}, {:.1f}M hashes) [duplicate rate={:.3f}%]".format(kind, total, math.log(total, 2), total / 1024**2, dup))
        n_entries = 1
        for kind in KINDS:
        	n_entries *= n_hash[kind]
        sizes = [n_hash[kind] * 8 / (1 << args.k)/ 1024**2 for kind in KINDS]
        print()
        print("task size = {:.1f}Mbyte x {:.1f}Mbyte x {:.1f}Mbyte".format(*sizes))
        print("Est # (64+k)-bit solutions = {:.1f}".format(n_entries / 2**(64 + args.k)))
        print()


def splitting():
    """
    split all preimage files using [[split_bits]]
    """
    mpi_n_process = 2 + 2 * args.cores
    for kind, kind_id in KINDS.items():
        files = sorted(glob.glob('{}/{}.*'.format(PREIMAGE_DIR, kind)))
        for preimage in files:
            split = '{}/{:03x}/{}.unsorted'.format(DICT_DIR, (1 << args.k) - 1, os.path.basename(preimage))
            if os.path.exists(split):
                if args.verbose:
                    print("# Skiping {} [already split]".format(preimage))
                continue
            mpi_n_process = 2 + 2 * args.cores
            cmd = ['mpirun', '-np', mpi_n_process, SPLITTER, 
                   '--partitioning-bits', args.k, '--output-dir', DICT_DIR, preimage]
            cmdline = list(map(str, cmd))
            if args.verbose:
                print(" ".join(cmdline))
            if not args.dry_run:
                subprocess.run(cmdline).check_returncode()


def check_dict():
    """
    Verify all dictionnaries.
    """
    jobs = []
    for i in range(1 << args.k):
        for file in sorted(glob.glob('{}/{:03x}/*'.format(DICT_DIR, i))):
            cmds = [DICT_CHECKER, '--partitioning-bits', str(args.k), file]
            if args.verbose:
                print(" ".join(cmds))
            if not args.dry_run:
                subprocess.run(cmds).check_returncode()
    


def merging():
    """
    merge all (sorted) dictionnaries.
    """
    for kind in KINDS:
        for i in range(1 << args.k):
            input_files = sorted(glob.glob('{}/{:03x}/{}.*.sorted'.format(DICT_DIR, i, kind)))
            if not input_files:
                continue
            output_file = '{}/{}.{:03x}'.format(HASH_DIR, kind, i)
            if os.path.exists(output_file):
                output_mtime = os.stat(output_file).st_mtime
                input_mtime = 0
                for f in input_files:
                    input_mtime = max(input_mtime, os.stat(f).st_mtime)
                if input_mtime < output_mtime:
                    if args.verbose:
                        print('# skipping {} [already merged]'.format(output_file))
                    continue
            cmds = [MERGER, '--output', output_file] + input_files
            if args.verbose:
                print(" ".join(cmds))
            if not args.dry_run:
                subprocess.run(cmds, stdout=subprocess.DEVNULL).check_returncode()

def check_hash():
    """
    Verify all dictionnaries.
    """
    for kind, kind_id in KINDS.items():
        for file in sorted(glob.glob('{}/{}.*'.format(HASH_DIR, kind))):
            cmds = [HASH_CHECKER, file]
            if args.verbose:
                print(" ".join(cmds))
            if not args.dry_run:
                subprocess.run(cmds).check_returncode()



def slicing():
    """
    compute the slice for foobar hash files.
    """
    for i in range(1 << args.k):
        input_file = '{}/foobar.{:03x}'.format(HASH_DIR, i)
        output_file = '{}/{:03x}'.format(SLICE_DIR, i)
        if not os.path.exists(input_file):
            continue
        if os.path.exists(output_file):
            input_mtime = os.stat(input_file).st_mtime
            output_mtime = os.stat(output_file).st_mtime
            if input_mtime < output_mtime:
                if args.verbose:
                    print('# skipping {} [already sliced]'.format(output_file))
                continue
        cmds = [SLICER, '--l', str(SLICE_L), '--target-dir', SLICE_DIR, input_file]
        if args.verbose:
            print(" ".join(cmds))
        if not args.dry_run:
            subprocess.run(cmds, stdout=subprocess.DEVNULL).check_returncode()


def grouping_kind(path, kind):
    assert ((1 << args.k) % args.group_size) == 0
    for i in range((1 << args.k) // args.group_size):
        output_file = '{}/{}.{:03x}'.format(TG_DIR, kind, i)
        input_files = []
        for j in range(args.group_size):
            if kind == 'foobar':
                input_files.append('{}/{:03x}'.format(path, i * args.group_size + j))
            else:
                input_files.append('{}/{}.{:03x}'.format(path, kind, i * args.group_size + j))


        if os.path.exists(output_file):
            output_mtime = os.stat(output_file).st_mtime
            input_mtime = 0
            for f in input_files:
                input_mtime = max(input_mtime, os.stat(f).st_mtime)
            if input_mtime < output_mtime:
                if args.verbose:
                    print('# skipping {} [already grouped]'.format(output_file))
                continue

        cmds = 'python3 grouper.py {ins} > {out}'.format(ins=' '.join(input_files), out=output_file)
        if args.verbose:
            print(cmds)
        if not args.dry_run:
            subprocess.run(cmds, shell=True).check_returncode()

def grouping():
    grouping_kind(HASH_DIR, 'foo')
    grouping_kind(HASH_DIR, 'bar')
    grouping_kind(SLICE_DIR, 'foobar')


if args.stats:
    preimage_stats()
    dict_stats()
    hash_stats()
    sys.exit()

ensure_dirs()

print("1. Splitting (preimage --> dictionaries), [split_bits={}]".format(args.k))
splitting()
if args.check:
    print("X. Checking (unsorted) dictionaries")
    check_dict()

#print("2. Sorting (dictionaries -> dictionaries)")
#sorting()
#if args.check:
#    print("Y. Checking (sorted) dictionaries")
#    check_dict()

print("3. Merging (dictionaries -> hash files)")
merging()
if args.check:
    print("Z. Checking hashes")
    check_hash()

if args.slice:
    print("4. Slicing (hash files -> slice files)")
    slicing()

print("5. grouping (hash & slice files -> task group files)")
grouping()
