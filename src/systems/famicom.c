#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include "../types.h"
#include "../bitmath.h"
#include "system.h"
#include "../chips/2C02.h"
#include "../chips/6502.h"
#include "famicom.h"
#define SET_BIT(b,i) (b | 1 << i)
#define CLEAR_BIT(b,i) (b & ~(1 << i))
#define GET_BIT(b,i) (b>>i) & 1;

const int memsize_famicom = 0x0800;

Famicom* famicom_create ()
{
	Famicom* famicom = malloc(sizeof(Famicom));
	famicom->mem = malloc( sizeof(byte) * memsize_famicom );
	famicom->cpu = (Cpu_6502*) malloc(sizeof(Cpu_6502));
	famicom->ppu = (Famicom_ppu*) malloc(sizeof(Famicom_ppu));
	if (famicom->mem == NULL) {
		free(famicom);
		printf("couldn't allocate memory\n");
		return NULL;
	}
	if (famicom->cpu == NULL) {
		free(famicom->mem);
		free(famicom);
		printf("couldn't allocate memory\n");
		return NULL;
	}
	if (famicom->ppu == NULL) {
		free(famicom->mem);
		free(famicom->cpu);
		free(famicom);
		printf("couldn't allocate memory\n");
		return NULL;
	}
	famicom->cpu->running = false;
	return famicom;
}

void famicom_reset_controller(Famicom* f)
{
	f->controller_p1.right = false;
	f->controller_p1.left = false;
	f->controller_p1.up = false;
	f->controller_p1.down = false;
	f->controller_p1.select = false;
	f->controller_p1.start = false;
	f->controller_p1.button_b = false;
	f->controller_p1.button_a = false;
}


void famicom_reset (Famicom* famicom, bool warm)
{
	System system; system.s = famicom_system; system.h = famicom;
	if (!warm) {
		memset( famicom->mem, 0, sizeof(byte) * memsize_famicom );
		memset( famicom->ppu->nametable, 0, sizeof(byte) * sizeof(famicom->ppu->nametable) );
		memset( famicom->ppu->attribute_table, 0, sizeof(byte) * sizeof(famicom->ppu->attribute_table) );
		memset( famicom->ppu->oam, 0, sizeof(byte) * sizeof(famicom->ppu->oam) );
		memset( famicom->ppu->palettes, 0, sizeof(byte) * sizeof(famicom->ppu->palettes) );
	}
	famicom->cycles = 0;
	famicom->ppu->vblank_flag = false;
	famicom->ppu->nmi_enable = false;
	famicom->ppu->write_latch = false;
	famicom->ppu->vram_addr = 0;
	famicom->ppu->vram_increment =false;
	famicom->ppu->sprite_pattern_table = false;
	famicom->ppu->bg_pattern_table = false;
	famicom->ppu->address = 0;
	famicom->ppu->nametable_base = 0;
	famicom->ppu->scroll_x = 0;
	famicom->ppu->scroll_y = 0;
	famicom->ppu->nmi_hit = false;
	famicom->apu.pulse1_timer = 0;
	famicom->apu.pulse2_timer = 0;
	famicom->apu.tri_timer = 0;
	famicom_reset_controller(famicom);
	cpu_reset(famicom->cpu, system);
}


void famicom_destroy (Famicom* famicom)
{
	free(famicom->mem);
	free(famicom->prg);
	free(famicom->chr);
	free(famicom->chr_window);
	free(famicom->ppu);
	free(famicom->cpu);
	free(famicom);
}

int famicom_load_rom (Famicom* famicom, FILE* rom)
{
	byte header[16];
	fread(header, sizeof(byte), 16, rom);
	if (header[0] == 'N' && header[1] == 'E' && header[2] == 'S' && header[3] == 0x1A) {
		int prg_size = header[4] * 16384;
		int chr_size = header[5] * 8192;
		famicom->prg_size = prg_size;
		famicom->chr_size = chr_size;
		byte mapper_lo = (header[6] & 0xF0) >> 4;
		byte mapper_hi = header[7] & 0xF0;
		int mapper = (mapper_hi | mapper_lo);
		famicom->loaded_rom.mapper = mapper;
		int mirroring = header[6] & 0x01;
		famicom->loaded_rom.mirroring = mirroring;
		printf("NES rom, mapper: %d, \nprg size: %d, chr size: %d, mirroring: %d\n", mapper, prg_size, chr_size, mirroring);
		switch (mapper) {
		case 0:
		case 3:
			famicom->prg = (byte*)malloc(sizeof(byte) * prg_size);
			if (!famicom->prg) {
				printf("error allocating prg\n");
				fclose(rom);
				return 1;
			}
			fseek(rom, 16, SEEK_SET);
			fread(famicom->prg, sizeof(byte), prg_size, rom);
			famicom->chr = (byte*)malloc(sizeof(byte) * chr_size);
			if (famicom->chr == NULL) {
				free(famicom->prg);
				printf("error allocating chr\n");
				fclose(rom);
				return 1;
			}
			fseek(rom, 16 + prg_size, SEEK_SET);
			fread(famicom->chr, sizeof(byte), chr_size, rom);
			famicom->chr_window = (byte*) malloc(sizeof(byte) * 8192);
			memcpy(famicom->chr_window, famicom->chr, 8192);
			break;
		default:
			printf("unsupported mapper\n");
			fclose(rom);
			return 1;
		}
		fclose(rom);
		return 0;
	} else {
		printf("not an NES rom\n");
	}
	fclose(rom); // not an NES rom
	return 1;
}

const word ppu_addr_start = 0x2000;
const word apu_addr_start = 0x4000;
const word unmapped_addr_start = 0x4020;

enum ppu_mmapped_register {
	PPUCTRL,
	PPUMASK,
	PPUSTATUS,
	OAMADDR,
	OAMDATA,
	PPUSCROLL,
	PPUADDR,
	PPUDATA,
};

void oamdma(Famicom* f, byte value);
byte mmap_famicom(Famicom* f, word addr, byte value, bool write)
{
	byte ppustatus = 0;
	 // zero page
	if (addr < ppu_addr_start) {
		if (write) {
			f->mem[addr % 0x800] = value;
			return 0;
		} else {
			return f->mem[addr % 0x800];
		}
	// PPU registers
	} else if (addr < apu_addr_start) {
		byte ppu_reg = get_lower_byte(addr) % 8;
		switch (ppu_reg) {
		case PPUCTRL:
			if (write) {
				f->ppu->vram_increment = GET_BIT(value, 2);
				f->ppu->sprite_pattern_table = GET_BIT(value, 3);
				f->ppu->bg_pattern_table = GET_BIT(value, 4);
				f->ppu->nmi_enable = value & 0x80;
				f->ppu->nametable_base = value & 0x03;
				return 0;
			}
			break;
		case PPUMASK:
			return 0;
		case PPUSTATUS:
			ppustatus = set_bit(ppustatus, 7, f->ppu->vblank_flag);
			f->ppu->vblank_flag = false;
			f->ppu->write_latch = false;
			return ppustatus;
		case OAMADDR:
			if (write) {
				f->ppu->oam_address = value;
			}
			return 0;
		case OAMDATA:
			if (write) {
			}
			return 0;
		case PPUSCROLL:
			if (write) {
				if (f->ppu->write_latch) {
					f->ppu->scroll_y = value;
				} else {
					f->ppu->scroll_x = value;
				}
			}
			f->ppu->write_latch = !f->ppu->write_latch;
			return 0;
		case PPUADDR:
			if (write) {
				if (f->ppu->write_latch) {
					f->ppu->address = bytes_to_word(f->ppu->vram_addr, value) % 0x4000;
				} else {
					f->ppu->vram_addr = value;
				}
			}
			f->ppu->write_latch = !f->ppu->write_latch;
			return 0;
		case PPUDATA:
			if (write) {
				word address = f->ppu->address;
                                if (0x1FFF < address && address < 0x23C0) {
					f->ppu->nametable[0][address - 0x2000] = value;
				} else if (0x23BF < address && address < 0x2400) {
					f->ppu->attribute_table[0][address - 0x23C0] = value;
				} else if (0x23FF < address && address < 0x27C0) {
					f->ppu->nametable[1][address - 0x2400] = value;
				} else if (0x27BF < address && address < 0x2800) {
					f->ppu->attribute_table[1][address - 0x27C0] = value;
				} else if (0x2800 <= address && address <= 0x2BBF) {
					f->ppu->nametable[2][address - 0x2800] = value;
				} else if (0x2BC0 <= address && address <= 0x2BFF) {
					f->ppu->attribute_table[2][address - 0x2BC0] = value;
				} else if (0x3F00 < address && address < 0x3F20 ) {
					f->ppu->palettes[address - 0x3F00] = value;
				}
				if (f->ppu->vram_increment) {
					f->ppu->address += 32;
				} else {
					f->ppu->address += 1;
				}
				f->ppu->address = f->ppu->address % 0x4000;
			}
			return 0;
		}
	// APU & OAMDMA
	} else if (addr < unmapped_addr_start) {
		switch (addr) {
		case 0x4014:
			if (write) {
				oamdma(f, value);
			}
			return 0;
		case 0x4002:
			if (write) {
				f->apu.pulse1_timer = value;
			}
			return 0;
		case 0x4003:
			if (write) {
				f->apu.pulse1_timer = ((f->apu.pulse1_timer << 3) | value);
			}
			return 0;
		case 0x4006:
			if (write) {
				f->apu.pulse2_timer = value;
			}
			return 0;
		case 0x4007:
			if (write) {
				f->apu.pulse2_timer = ((f->apu.pulse2_timer << 3) | value);
			}
			return 0;
		case 0x4008:
			if (write) {
				f->apu.tri_timer = value;
			}
			return 0;
		case 0x400B:
			if (write) {
				f->apu.tri_timer = ((f->apu.tri_timer << 3) | value);
			}
			return 0;
		case 0x4016:
			if (write) {
				if (f->last_4016_write) {
					f->poll_controller = true;
					f->controller_bit = 0;
				}
				f->last_4016_write = value;
				return 0;
			} else {
				if (f->poll_controller) {
					bool button_stat;
					switch (f->controller_bit) {
					case joypad_right:
						button_stat = f->controller_p1.right;
						break;
					case joypad_left:
						button_stat = f->controller_p1.left;
						break;
					case joypad_down:
						button_stat = f->controller_p1.down;
						break;
					case joypad_up:
						button_stat = f->controller_p1.up;
						break;
					case joypad_start:
						button_stat = f->controller_p1.start;
						break;
					case joypad_select:
						button_stat = f->controller_p1.select;
						break;
					case joypad_b:
						button_stat = f->controller_p1.button_b;
						break;
					case joypad_a:
						button_stat = f->controller_p1.button_a;
						break;
					}
					f->controller_bit++;
					if (7 < f->controller_bit) {
						f->poll_controller = false;
						f->controller_bit = 0;
					}
					return button_stat;
				}
				return 0;
			}
		case 0x4017:
			return 0;
		default:
			return 0;
		}
		return 0;
	// cartridge
	} else if (unmapped_addr_start < addr) {
		switch(f->loaded_rom.mapper) {
		default:
		case 0:
			int offset;
			if (f->prg_size == 16384) {
				offset = 0xC000;
			} else if (f->prg_size == 32768) {
				offset = 0x8000;
			}
			if (0x7FFF < addr && addr <= 0xFFFF) {
				if (write)
					return 0;
				if (f->prg_size == 16384) {
					return f->prg[(addr - offset) % f->prg_size];
				} else if (f->prg_size == 32768) {
					return f->prg[(addr - offset) % f->prg_size];
				}
			}
			break;
		case 2:
			if (0x8000 <= addr && addr <= 0xFFFF) {
				if (write)
					return 0;
				return f->prg[(addr) - 0xC000];
			} else {
				return 0;
			}
			break;
		case 3:
			if (0x8000 <= addr && addr <= 0xFFFF) {
				if (write) {
					memcpy(f->chr_window, f->chr + ((value & f->prg[addr - 0x8000]) & 0x03) * 8192, 8192);
				}
				return f->prg[(addr) - 0x8000];
			} else {
				return 0;
			}
			break;
		}
	}
	return 0;
}

void oamdma(Famicom* f, byte value)
{
	int i = (value << 8);
	for (int oam_i=0; oam_i<64; oam_i++) {
		for (int ii=0; ii<4; ii++) {
			f->ppu->oam[oam_i][ii] = mmap_famicom(f, i+ii, 0, false);
		}
		i += 4;
	}
}

byte mmap_famicom_read ( Famicom* famicom, word addr )
{
	return mmap_famicom(famicom, addr, 0, false);
}

void mmap_famicom_write(Famicom *famicom, word addr, byte value)
{
	mmap_famicom(famicom, addr, value, true);
}

void famicom_step(Famicom* famicom, int cycles, bool debug, FILE* dfh)
{
	System system; system.s = famicom_system; system.h = famicom;
	Instruction i;
	byte oper1;
	byte oper2;
	byte oper[2];
	for (int c=0; c<cycles; c++) {
		famicom->debug.nmi = false;
		famicom->cpu->branch_taken = false;
		i = parse(mmap_famicom(famicom, famicom->cpu->pc, 0, false));
		if (i.n == unimplemented) {
			printf("unimplemented opcode %X!\n", mmap_famicom_read(famicom, famicom->cpu->pc));
			famicom->cpu->running = false;
			return;
		}
		oper1 = mmap_famicom(famicom, famicom->cpu->pc + 1, 0, false);
		oper2 = 0;
		switch (i.a) {
		default:
			break;
		case absolute:
		case absolute_indirect:
		case absolute_x:
		case absolute_y:
			oper2 = mmap_famicom(famicom, famicom->cpu->pc + 2, 0, false);
			break;
		}
		oper[0] = oper1;
		oper[1] = oper2;
		//printf("%s %X %X pc: %X sp: %X\n", i.m, oper1, oper2, famicom->cpu->pc, famicom->cpu->reg[reg_sp]);
		step(system, famicom->cpu, i, oper);

		if (debug)
			write_cpu_state(famicom->cpu, system, dfh);
		famicom->cycles++;
		if (famicom->ppu->vblank_flag && famicom->ppu->nmi_enable) {
			nmi(system, famicom->cpu);
			famicom->ppu->nmi_enable = false;
			famicom->ppu->vblank_flag = false;
			famicom->ppu->nmi_hit = true;
			famicom->debug.nmi = true;
		}
	}
}
