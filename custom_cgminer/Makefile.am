
ACLOCAL_AMFLAGS = -I m4

SUBDIRS		= lib ccan

cgminer_CPPFLAGS = $(PTHREAD_FLAGS) -fno-strict-aliasing $(USBUTILS_CPPFLAGS)

bin_PROGRAMS	= cgminer

cgminer_LDFLAGS	= $(PTHREAD_FLAGS)
cgminer_LDADD	= $(DLOPEN_FLAGS) @LIBCURL_LIBS@ @PTHREAD_LIBS@ \
		  @NCURSES_LIBS@ @PDCURSES_LIBS@ @WS2_LIBS@ \
		  @MM_LIBS@ @RT_LIBS@ \
		  @MATH_LIBS@ lib/libgnu.a ccan/libccan.a -L/usr/local -lnanomsg

cgminer_CPPFLAGS += -I$(top_builddir)/lib -I$(top_srcdir)/lib -I/usr/local/include

# common sources
cgminer_SOURCES := cgminer.c

cgminer_SOURCES	+= elist.h miner.h compat.h bench_block.h	\
		   util.c util.h uthash.h logging.h		\
		   sha2.c sha2.h 
# api.c

cgminer_SOURCES	+= logging.c

# cgminer_SOURCES	+= klist.h klist.c

# cgminer_SOURCES	+= noncedup.c

cgminer_SOURCES += usbutils.c usbutils.h

cgminer_SOURCES += driver-bitmain.c driver-bitmain.h