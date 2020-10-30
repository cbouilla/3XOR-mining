AntMiner S7
===========

Accessing the S7 is easy : ssh root@......, password = "admin". Accessing the web interface is also easy : root/root (because of the specific setup at ULille, this can be done by : ssh caza.lifl.fr -N -L 8000:mineur.lifl.fr:80, then target a web browser to local port 8000).

The AntMiner S7, a customized version of CGminer is installed. Fortunately, the code of this modified version is online in https://github.com/bitmaintech/cgminer, but beware! The code running on the S7 corresponds to the "bitmain_fan-ctrl" branch. It has code specific to the S7.

On the S7, a custom shell script monitors cgminer and restarts it if it stops. This script is also restarted if it exists!

When the S7 reboots, its state is restored to factory settings. This allows us to play without risk.

So, to **actually** stop CGminer on the S7, a possibility is to run the following script (scripts/relax.sh):

#!/bin/bash
rm /sbin/monitorcg
killall monitorcg
killall cgminer


Cross-compiling nanomsg
=======================

Add :

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armhf)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)

to CMakeList and compile.

Copy libnanomsg* to /usr/lib/ on the miner

	scp libnanomsg.so* mineur:/usr/lib


Cross-compiling CGminer
=======================

The trick is to build a modified version of (the modified version of) CGminer. Compiling on the S7 is a PITA, so we cross-compile locally.

Our version of CGminer is accessible at https://github.com/cbouilla/cgminer. We removed nearly everything not useful to us. To cross-compile :

#!/bin/bash
CFLAGS="-g -O2 -Wall -Wextra" ./configure --host=arm-linux-gnueabihf --enable-bitmain --without-curses --disable-libcurl --disable-udev
make clean
make




Running the modifier CGminer on the S7
======================================

Once nanomsg has been "installed" on the S7, the cross-compiled cgminer binary copied to /root, and the "normal" cgminer stopped, then we may safely run our own (script/run.sh):


+ Stop the previous one:

	rm /sbin/monitorcg
	killall monitorcg
	killall cgminer

+ copy new cgminer
+ run it inside a screen

	export TERM=vt100
	screen
	./cgminer --bitmain-dev /dev/bitmain-asic --bitmain-options 115200:32:8:5:700:0782:0706  --bitmain-hwerror --queue 8192

