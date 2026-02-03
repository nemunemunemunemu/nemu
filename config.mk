VERSION = 0.0.0
CC = cc

PREFIX = /usr/local

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
SDL3INC = /usr/local/include/SDL3
OPUSINC = /usr/local/include/opus
LIBEPOLLINC = /usr/local/include/libepoll-shim

INCS = -I/usr/local/include -I${SDL3INC} -I${X11INC} -I${LIBEPOLLINC}
LIBS = -L/usr/local/lib -lSDL3 -L${X11LIB} -lm
CFLAGS = -D_REENTRANT -O0 -g ${INCS}
LDFLAGS = ${LIBS}
