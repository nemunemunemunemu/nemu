typedef struct famicom_debug {
	char* rom_name;
	int rom_mapper;
	bool nmi;
	bool irq;
} Famicom_debug;

typedef struct famicom {
	Famicom_ppu* ppu;
	Famicom_debug debug;
	Cpu_6502* cpu;
	int cycles;
	int prg_size;
	int chr_size;
	byte* mem;
	byte* prg;
	byte* chr;
	byte oam[64][4];
} Famicom;

Famicom* famicom_create ();
void famicom_reset (Famicom* famicom);
void famicom_destroy (Famicom* famicom);
void famicom_step (Famicom* famicom);
int  famicom_load_rom (Famicom* famicom, FILE* rom);
byte mmap_famicom(Famicom* f, word addr, byte value, bool write);
byte mmap_famicom_read ( Famicom* famicom, word addr );
void mmap_famicom_write ( Famicom* famicom, word addr, byte value );
