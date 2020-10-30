import subprocess

k = 8

for i in range(1 << k):
	hash_file = '../data/hash/foobar.{:03x}'.format(i)
	slice_file = '../data/slice_4/{:03x}'.format(i)
	cmds = ['./slice_checker', '--hash', hash_file, '--slice', slice_file, '--l', '19']
	subprocess.run(cmds).check_returncode()
