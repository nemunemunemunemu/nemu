#include <SDL2/SDL.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "inprint/SDL2_inprint.h"

const int window_width = 514;
const int window_height = 256;
SDL_Instance* init_graphics()
{
	SDL_Instance* instance;
	instance = malloc(sizeof(SDL_Instance));
	if (instance == NULL) {
		return NULL;
	}
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		printf("unable to start SDL: %s\n", SDL_GetError());
		return NULL;
	}
	instance->window = SDL_CreateWindow("nemu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_width, window_height, 0);
	if (instance->window == NULL) {
		printf("unable to create window: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	instance->renderer = SDL_CreateRenderer(instance->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (instance->renderer == NULL) {
		printf("unable to create renderer: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	inrenderer(instance->renderer);
	prepare_inline_font();
	return instance;
}

void draw_debug(SDL_Instance* g, Famicom* f, int x, int y)
{
	const int WHITE = 0xFFFFFF;
	const int GREEN = 0x00FF00;
	const int GREY = 0x666666;
	incolor(WHITE, 0);
	char romstatus[50];
	snprintf(romstatus, sizeof(romstatus), "%s, %d", f->debug.rom_name, f->debug.rom_mapper);
	inprint(g->renderer, romstatus, x,  y);

	char cpustatus[50];
	snprintf(cpustatus, sizeof(cpustatus), "PC:%0X A:%0X X:%0X Y:%0X S:%0X",
	    f->cpu->pc, f->cpu->reg[reg_a], f->cpu->reg[reg_x], f->cpu->reg[reg_y], f->cpu->reg[reg_sp]);
	inprint(g->renderer, cpustatus, x,  y+9);

	char istatus[50];
	Instruction i = f->cpu->current_instruction;
	byte oper1 = f->cpu->oper[0];
	byte oper2 = f->cpu->oper[1];

	word opera = bytes_to_word(oper2, oper1);
	char addr_mode[50];
	switch (i.a) {
	case accumulator: case implied:
		snprintf(addr_mode, sizeof(addr_mode), " ");
		break;
	case immediate: case zeropage: case zeropage_x: case zeropage_y: case zeropage_xi: case zeropage_yi:
		snprintf(addr_mode, sizeof(addr_mode), "(%X) ", oper1);
		break;
	case relative:
		snprintf(addr_mode, sizeof(addr_mode), "%d (%X)", (int8_t)oper1, oper1);
		break;
	case absolute: case absolute_indirect: case absolute_x: case absolute_y:
		snprintf(addr_mode, sizeof(addr_mode), "0x%X", opera);
		break;
	}
	snprintf(istatus, sizeof(istatus), "%s %s", f->cpu->current_instruction_name, addr_mode);
	char* flags[] = {
		"C",
		"Z",
		"I",
		"D",
		"B",
		"-",
		"V",
		"N",
	};
	for (int i=0; i<8; i++) {
		if (get_bit(f->cpu->reg[reg_p], i) == 1) {
			incolor(GREEN, 0);
		} else {
			incolor(GREY, 0);
		}
		inprint(g->renderer, flags[i], x+(i*9), y+18);
	}
	incolor(0xffffff, 0);
	inprint(g->renderer, istatus, x,  y+27);
	printf("%s %s\n", cpustatus, istatus);
	if (f->debug.nmi) {
		incolor(GREEN, 0);
	} else {
		incolor(GREY, 0);
	}
	inprint(g->renderer, "nmi", x,  y+36);

	incolor(WHITE, 0);
	char ppustatus[50];
	char ppuctrl[50];
	snprintf(ppuctrl, sizeof(ppuctrl), "%X00%X%X%X%X",
	    f->ppu->nmi_enable, f->ppu->bg_pattern_table, f->ppu->sprite_pattern_table, f->ppu->vram_increment, f->ppu->nametable_base);
	snprintf(ppustatus, sizeof(ppustatus), "%s  %0X        %0X", ppuctrl, f->ppu->address, f->ppu->oam_address);
	inprint(g->renderer, ppustatus, x, y+45);
	inprint(g->renderer, "PPUCTRL  PPUADDR  OAMADDR", x, y+54);
}

void draw_tile(SDL_Instance* g, Famicom* f, int tile, int x_offset, int y_offset, bool mirrored, int table, SDL_Color palette[])
{
	word table_start;
	if (table == 0) {
		table_start = 0;
	} else if (table == 1) {
		table_start = 0x1000;
	}
	for (int y=0; y<8; y++) {
		for (int x=0; x<8; x++) {
			byte plane1;
			byte plane2;
			plane1 = f->chr[table_start + tile + y];
			plane2 = f->chr[table_start + tile + y + 8];
			if (!mirrored) {
				plane1 = reverse_byte_order(plane1);
				plane2 = reverse_byte_order(plane2);
			}
			byte first_bit = get_bit(plane1, x);
			byte second_bit = get_bit(plane2, x);
			if (first_bit == 0 && second_bit == 0) {
				SDL_SetRenderDrawColor(g->renderer, palette[0].r, palette[0].g, palette[0].b, 0xFF);
			} else if (first_bit == 1 && second_bit == 0) {
				SDL_SetRenderDrawColor(g->renderer, palette[1].r, palette[1].g, palette[1].b, 0xFF);
			} else if (first_bit == 0 && second_bit == 1) {
				SDL_SetRenderDrawColor(g->renderer, palette[2].r, palette[2].g, palette[2].b, 0xFF);
			} else if (first_bit == 1 && second_bit == 1) {
				SDL_SetRenderDrawColor(g->renderer, palette[3].r, palette[3].g, palette[3].b, 0xFF);
			}
			SDL_RenderDrawPoint(g->renderer, x + x_offset, y + y_offset);
		}
	}
}

void draw_pattern_table(SDL_Instance* g, Famicom* f, int table, int x_offset, int y_offset, SDL_Color palette[])
{
	int i = 0;
	for (int y=0; y<16; y++) {
		for (int x=0; x<16; x++) {
			draw_tile(g, f, i, (x*8)+x_offset, (y*8)+y_offset, false, table, palette);
			i = i + 16;
		}
	}
}

void draw_nametable(SDL_Instance* g, Famicom* f, int x, int y, SDL_Color palette[])
{
	for (int y=0;y<30;y++) {
		for (int x=0;x<32;x++) {
			draw_tile(g, f, f->ppu->nametable[0][32*y+x]*16, (x*8), (y*8), false, f->ppu->bg_pattern_table, palette);
		}
	}
	/*
	incolor(0xFFFFFF, 0);
	for (int y=0;y<8;y++) {
		for (int x=0;x<8;x++) {
			char attr[2];
			sprintf(attr, "%X", f->ppu->attribute_table[0][8*y+x]);
			inprint(g->renderer, attr, (x*32)+8, y*32);
		}
	}
	*/
}

void draw_oam(SDL_Instance* g, Famicom* f, SDL_Color palette[])
{
	for (int i=0; i<64; i++) {
		byte sprite_y = f->ppu->oam[i][0];
		byte tile = f->ppu->oam[i][1] * 16;
		byte sprite_x = f->ppu->oam[i][3];
		draw_tile(g, f, tile, sprite_x, sprite_y, false, f->ppu->sprite_pattern_table, palette);
	}
}
