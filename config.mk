VERSION = 0.0.0
CC = cc

PREFIX = /usr/local

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
SDL2INC = /usr/local/include/SDL2

INCS = -I/usr/local/include -I${SDL2INC} -I${X11INC}
LIBS = -L/usr/local/lib -lSDL2 -L${X11LIB}
CFLAGS = -D_REENTRANT -O0 -g ${INCS}
LDFLAGS = ${LIBS}
