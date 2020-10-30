import sys
import array
import os.path

filenames = sys.argv[1:]
prefix = array.array('Q')
assert prefix.itemsize == 8

n = len(filenames)
prefix.append(n)

s = 2 + n
for f in filenames:
    l = os.path.getsize(f)
    assert (l % 8) == 0
    size = l // 8
    prefix.append(s)
    s += size
prefix.append(s)

sys.stdout.buffer.write(prefix.tobytes())

for f in filenames:
    with open(f, 'rb') as g:
        sys.stdout.buffer.write(g.read())
