#include <setjmp.h>
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


const bool debug = true;

Famicom* famicom_create ()
{
	const int memsize = 0x0800;
	Famicom* famicom = malloc(sizeof(Famicom));
	famicom->mem = malloc( sizeof(byte) * memsize );
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

void famicom_reset (Famicom* famicom)
{
	const int memsize = 0x0800;
	System system; system.s = famicom_system; system.h = famicom;
	memset( famicom->mem, 0, sizeof(byte) * memsize );
	memset( famicom->ppu->nametable, 0, sizeof(byte) * sizeof(famicom->ppu->nametable) );
	memset( famicom->ppu->attribute_table, 0, sizeof(byte) * sizeof(famicom->ppu->attribute_table) );
	famicom->ppu->vblank_flag = true;
	famicom->ppu->nmi_enable = false;
	famicom->ppu->write_latch = false;
	famicom->ppu->vram_addr = 0;
	famicom->ppu->vram_increment=false;
	famicom->ppu->sprite_pattern_table = false;
	famicom->ppu->bg_pattern_table = false;
	famicom->ppu->address = 0;
	famicom->ppu->nametable_base = 0;
	cpu_reset(famicom->cpu, system);
	//	famicom->cpu->pc = 0xF1B4;
}

void famicom_destroy (Famicom* famicom)
{
	free(famicom->mem);
	free(famicom->prg);
	free(famicom->chr);
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
		byte mapper_lo = (byte)(header[6] & 0x0F);
		byte mapper_hi = (byte)((header[7] & 0xF0) >> 4);
		byte mapper = (byte)((mapper_hi << 4) | mapper_lo);
		famicom->debug.rom_mapper = mapper;
		printf("NES rom, mapper: %d, \nprg size: %d, chr size: %d\n", mapper, prg_size, chr_size);
		switch (mapper) {
		case 0:
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

byte mmap_famicom(Famicom* f, word addr, byte value, bool write)
{
	 // zero page
	if (addr < ppu_addr_start) {
		if (write) {
			f->mem[addr % 0x0801] = value;
			return 0;
		} else {
			return f->mem[addr % 0x0801];
		}
	// PPU registers
	} else if (addr < apu_addr_start) {
		byte ppu_reg = get_lower_byte(addr) % 8;
		switch (ppu_reg) {
		case PPUCTRL:
			if (write) {
				f->ppu->nmi_enable = get_bit(value, 7);
				f->ppu->sprite_pattern_table = get_bit(value, 5);
				f->ppu->bg_pattern_table = get_bit(value, 4);
				f->ppu->vram_increment = get_bit(value, 2);
				f->ppu->nametable_base = value & 0x03;
				return 0;
			} else {
				byte ppuctrl = 0;
				ppuctrl = set_bit(ppuctrl, 0, f->ppu->nmi_enable);
				ppuctrl = set_bit(ppuctrl, 3, f->ppu->bg_pattern_table);
				ppuctrl = set_bit(ppuctrl, 4, f->ppu->sprite_pattern_table);
				ppuctrl = set_bit(ppuctrl, 5, f->ppu->vram_increment);
				return ppuctrl;
			}
			break;
		case PPUMASK:
			return 0;
		case PPUSTATUS:
			if (write) {
				// PPUSTATUS should not be written to
				return 0;
			} else {
				byte ppustatus = 0;
				if (f->ppu->vblank_flag) {
					ppustatus = set_bit(ppustatus, 0, true);
				}
				return ppustatus;
			}
			f->ppu->vblank_flag = false;
			f->ppu->write_latch = false;
			return 0;
		case OAMADDR:
			return 0;
		case OAMDATA:
			return 0;
		case PPUSCROLL:
			f->ppu->write_latch = !f->ppu->write_latch;
			if (write) {
				return 0;
			} else {
				return 0;
			}
		case PPUADDR:
			if (write) {
				if (f->ppu->write_latch) {
					f->ppu->address = bytes_to_word(f->ppu->vram_addr, value);
				} else {
					f->ppu->vram_addr = value;
				}
				f->ppu->write_latch = !f->ppu->write_latch;
			}
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
				}
				if (f->ppu->vram_increment) {
					f->ppu->address += 32;
				} else {
					f->ppu->address += 1;
				}
			}
			return 0;
		}
	} else if (addr < unmapped_addr_start) {
		return 0;
	// cartridge
	} else if (unmapped_addr_start < addr) {
		if (0x7FFF < addr && addr <= 0xFFFF) {
			if (write) {
				printf("tried to write to cartridge space %0X - something went wrong\n", addr);
				return 0;
			} else {
				return f->prg[(addr - 0xC000)];
			}
		}
	} else {
		return 0;
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

bool branch_taken = false;

void famicom_step(Famicom* famicom)
{
	System system; system.s = famicom_system; system.h = famicom;
	famicom->debug.nmi = false;
	famicom->cpu->branch_taken = false;
	Instruction i = parse(mmap_famicom_read(famicom, famicom->cpu->pc));
	if (i.n == unimplemented) {
		printf("unimplemented opcode %X!\n", mmap_famicom_read(famicom, famicom->cpu->pc));
		famicom->cpu->running = false;
	}
	byte oper1 = mmap_famicom_read(famicom, famicom->cpu->pc + 1);
	byte oper2 = 0;
	word opera = 0;
	switch (i.a) {
	default:
		break;
	case absolute:
	case absolute_indirect:
	case absolute_x:
	case absolute_y:
		oper2 = mmap_famicom_read(famicom, famicom->cpu->pc + 2);
		opera = bytes_to_word(oper2, oper1);
		break;
	}
	byte oper[2] = {oper1, oper2};
	famicom->cpu->oper[0] = oper1;
	famicom->cpu->oper[1] = oper2;
	famicom->cpu->current_instruction = i;
	step(system, famicom->cpu, i, oper);
	famicom->cycles++;
	if (famicom->ppu->vblank_flag && famicom->ppu->nmi_enable) {
		nmi(system, famicom->cpu);
		famicom->ppu->nmi_enable = false;
		famicom->debug.nmi = true;
	}
}
