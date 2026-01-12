typedef struct ppu {
	bool vblank_flag;
	bool nmi_enable;
	bool write_latch;
	byte vram_addr;
	word address;
	bool nametable_base;
	bool vram_increment;
	bool bg_pattern_table;
	bool sprite_pattern_table;
	byte nametable[4][1024];
	byte attribute_table[4][64];
} Famicom_ppu;
