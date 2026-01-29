typedef struct apple1 {
	Cpu_6502* cpu;
	byte* mem;
	byte rom[0x100];
} Apple1;

Apple1* apple1_create();
void apple1_reset(Apple1* apple1);
void apple1_destroy(Apple1* apple1);
void apple1_step (Apple1* apple1);
byte mmap_apple1(Apple1* apple1, word addr, byte value, bool write);
