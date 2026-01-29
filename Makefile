include config.mk

SRC =  src/inprint/inprint2.c src/bitmath.c src/graphics.c src/chips/6502.c src/systems/famicom.c src/systems/apple1.c src/nemu.c
OBJ =  ${SRC:.c=.o}

all: nemu

.c.o:
	mkdir -p bin && cd bin && ${CC} -c ${CFLAGS} ../$<

nemu: ${OBJ}
	${CC} -o bin/$@ bin/*.o ${LDFLAGS}

.PHONY: clean
clean:
	rm -f bin/*
