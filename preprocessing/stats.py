import glob
import subprocess
import multiprocessing
import itertools
import os.path
import math

"""
This script runs all the preprocessing.
"""

KINDS = {'foo': 0, 'bar': 1, 'foobar': 2}
PREIMAGE_DIR = '../fooabar'
DICT_DIR = '../data/dict'
HASH_DIR = '../data/hash'

split_bits = 4

n_preimages = {}
n_dict = {}
n_hash = {}

def ensure_dirs():
    for i in range(1 << split_bits):
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
        print("Est # (64+k)-bit solutions = {:.1f}".format(n_entries / 2**(64 + split_bits)))
        print()


preimage_stats()
dict_stats()
hash_stats()
