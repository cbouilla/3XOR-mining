CFLAGS="-g -O2 -Wall -Wextra" ./configure --host=arm-linux-gnueabihf --enable-bitmain --without-curses --disable-libcurl --disable-udev
make clean
make