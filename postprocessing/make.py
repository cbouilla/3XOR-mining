import subprocess
import os.path
import glob

#subprocess.run('rsync ryuf001@turing.idris.fr:foobar/ijoux/*.bin ../production/', shell=True).check_returncode()

for sol_file in glob.glob('../production/*.bin'):
    base = os.path.basename(sol_file)
    target = base[:-3] + 'txt'
    #if os.path.exists(target):
    #    print("Skipping {}".format(sol_file))
    #    continue
    print("Processing {}".format(sol_file))
    cmds = './post_processing --dict-dir ../data/dict {ins} | tee {out}'.format(ins=sol_file, out=target)
    subprocess.run(cmds, shell=True).check_returncode()
