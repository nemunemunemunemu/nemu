VERSION = 0.0.0
CC = cc

PREFIX = /usr/local

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
SDL2INC = /usr/local/include/SDL2
OPUSINC = /usr/local/include/opus

INCS = -I/usr/local/include -I${SDL2INC} -I${X11INC} -I${OPUSINC}
LIBS = -L/usr/local/lib -lSDL2 -lSDL2 -lSDL2_mixer -L${X11LIB}
CFLAGS = -D_REENTRANT -O0 -g ${INCS}
LDFLAGS = ${LIBS}
