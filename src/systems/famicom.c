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
	memset( famicom->mem, 0, sizeof(byte) * memsize );
	System system; system.s = famicom_system; system.h = famicom;
	famicom->ppu->vblank_flag = true;
	cpu_reset(famicom->cpu, system);
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

byte mmap_famicom(Famicom* famicom, word addr, byte value, bool write)
{
	if (addr < ppu_addr_start) {
		if (write) {
			famicom->mem[addr % 0x0801] = value;
			return 0;
		} else {
			return famicom->mem[addr % 0x0801];
		}
	} else if (addr < apu_addr_start) {
		byte ppu_reg = get_lower_byte(addr) % 7;
		switch (ppu_reg) {
		case PPUCTRL:
			if (write) {
				if (get_bit(value, 7) == 0) {
					famicom->ppu->nmi_enable = true;
					return 0;
				}
			} else {
				byte ppuctrl;
				if (famicom->ppu->nmi_enable) {
					ppuctrl = set_bit(ppuctrl, 0, true);
					print_byte_as_bits(ppuctrl); printf("\n");
				}
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
				byte ppustatus;
				if (famicom->ppu->vblank_flag) {
					ppustatus = set_bit(ppustatus, 0, true);
				}
				return ppustatus;
			}
			return 0;
		case OAMADDR:
			return 0;
		case OAMDATA:
			return 0;
		case PPUSCROLL:
			return 0;
		case PPUADDR:
			return 0;
		case PPUDATA:
			return 0;
		}
	} else if (addr < unmapped_addr_start) {
		return 0;
	} else if (unmapped_addr_start < addr) {
		if (0x7FFF < addr && addr <= 0xFFFF) {
			if (write) {
				if (debug) {
				printf("tried to write to %X\n", addr);
				}
				return 0;
			} else {
				return famicom->prg[(addr - 0xC000) % 0x4001];
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

void mmap_famicom_write ( Famicom* famicom, word addr, byte value )
{
	mmap_famicom(famicom, addr, value, true);
}

bool branch_taken = false;

void famicom_step(Famicom* famicom)
{
	System system;
	system.s = famicom_system;
	system.h = famicom;
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
	step(system, famicom->cpu, i, oper);
	famicom->cycles++;
}
