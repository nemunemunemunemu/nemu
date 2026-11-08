enum ppu_mirroring {
	mirror_horizontal,
	mirror_vertical,
	mirror_single_lower,
	mirror_single_upper,
	mirror_four_screen,
};

typedef struct ppu {
	byte ctrl;
	byte mask;
	byte status;
	byte oam_address;
	byte oam[256];
	byte palettes[0x20];
	byte vram[0x1000]; // 2KB internal + 2KB for four screen carts
	// loopy registers
	word v;
	word t;
	byte fine_x;
	bool write_latch;
	byte ppudata_buffer;
	byte open_bus;
	// timing
	int dot;
	int scanline;
	bool odd_frame;
	long frame;
	bool nmi_pending;
	bool frame_complete;
	// background pipeline
	byte nt_latch;
	byte at_latch;
	byte bg_lo_latch;
	byte bg_hi_latch;
	word bg_shift_lo;
	word bg_shift_hi;
	word at_shift_lo;
	word at_shift_hi;
	// sprites on the current scanline, in oam order
	int sprite_count;
	bool sprite_zero_on_line;
	byte sprite_x[8];
	byte sprite_attr[8];
	byte sprite_lo[8];
	byte sprite_hi[8];
	// palette indices of the last finished frame
	byte framebuffer[240][256];
} Famicom_ppu;

struct famicom;
void ppu_reset(Famicom_ppu* ppu, bool warm);
void ppu_step(struct famicom* f);
byte ppu_read(struct famicom* f, word addr);
byte ppu_peek(struct famicom* f, word addr);
void ppu_write(struct famicom* f, word addr, byte value);
byte ppu_register_read(struct famicom* f, word addr);
void ppu_register_write(struct famicom* f, word addr, byte value);
