all: nemu
nemu:
	mkdir -p bin
	clang -g -O0 src/inprint/inprint2.c \
	             src/bitmath.c \
		     src/graphics.c \
		     src/chips/6502.c \
		     src/systems/famicom.c \
		     src/nemu.c \
	-I/usr/local/include -I/usr/local/include/SDL2 -I/usr/X11R6/include -D_REENTRANT -I/usr/X11R6/include -L/usr/local/lib -lSDL2 -L/usr/X11R6/lib -o bin/nemu
clean:
	rm bin/*
