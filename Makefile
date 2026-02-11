include config.mk

all: mkbin audio graphics sst famicom apple1 cpu nemu link

mkbin:
	mkdir -p bin

audio:
	${CC} ${CFLAGS} -c src/audio.c -o bin/audio.o

graphics:
	${CC} ${CFLAGS} -c src/graphics.c -o bin/graphics.o

sst:
	${CC} ${CFLAGS} -c src/systems/sst.c -o bin/sst.o

famicom:
	${CC} ${CFLAGS} -c src/systems/famicom.c -o bin/famicom.o

apple1:
	${CC} ${CFLAGS} -c src/systems/apple1.c -o bin/apple1.o

cpu:
	${CC} ${CFLAGS} -c src/chips/6502.c -o bin/6502.o

nemu:
	${CC} ${CFLAGS} -c src/bitmath.c -o bin/bitmath.o
	${CC} ${CFLAGS} src/nemu.c -c -o bin/nemu.o

link:
	${CC} ${LDFLAGS} bin/*.o -o bin/nemu

run_sst:
	${CC} -O2 src/bitmath.c src/chips/6502.c src/systems/*.c src/cjson/cJSON.c src/run_sst.c -o bin/run_sst

.PHONY: clean
clean:
	rm -f bin/*
