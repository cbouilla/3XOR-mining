import sys
import glob
import subprocess
import os
import os.path
import math
import argparse

parser = argparse.ArgumentParser(description="This script runs all the preprocessing")
parser.add_argument("--partitioning-bits", help="number of bits to split", type=int, required=True, dest="k")
parser.add_argument("--group", help="run the grouper", type=int, dest="group_size")
parser.add_argument("--dry-run", help="print commands but don't run them", action="store_true")
parser.add_argument("--verbose", help="display more information", action="store_true")

args = parser.parse_args()


HASH_DIR = '../data/hash'
SLICE_DIR = '../data/slice'
TG_DIR = '../data/task_groups'


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


if __name__ == '__main__':
    grouping_kind(HASH_DIR, 'foo')
    # grouping_kind(HASH_DIR, 'bar')
    grouping_kind(SLICE_DIR, 'foobar')