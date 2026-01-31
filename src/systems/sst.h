//psuedo system for running single step tests
typedef struct sst {
	Cpu_6502* cpu;
	char ram[0x10000];
} Sst;
byte mmap_sst(Sst* s, word addr, byte value, bool write);
