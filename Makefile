include config.mk

SRC =  src/inprint/inprint2.c src/bitmath.c src/graphics.c src/chips/6502.c src/systems/famicom.c src/systems/apple1.c src/systems/sst.c src/nemu.c
OBJ =  ${SRC:.c=.o}

all: nemu

.c.o:
	mkdir -p bin && cd bin && ${CC} -c ${CFLAGS} ../$<

nemu: ${OBJ}
	${CC} -o bin/$@ bin/*.o ${LDFLAGS}

run_sst:
	${CC} -o0 -g src/bitmath.c src/chips/6502.c src/systems/famicom.c src/systems/apple1.c src/systems/sst.c src/cjson/cJSON.c src/run_sst.c -o bin/run_sst

.PHONY: clean
clean:
	rm -f bin/*
