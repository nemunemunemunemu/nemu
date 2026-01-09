#include <SDL2/SDL.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "inprint/SDL2_inprint.h"

const int window_width = 512;
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
	incolor(0xffffff, 0);
	char romstatus[50];
	snprintf(romstatus, sizeof(romstatus), "%s, %d", f->debug.rom_name, f->debug.rom_mapper);
	inprint(g->renderer, romstatus, x,  y);

	char cpustatus[50];
	snprintf(cpustatus, sizeof(cpustatus), "PC:%0X A:%0X X:%0X Y:%0X",
	    f->cpu->pc, f->cpu->reg[reg_a], f->cpu->reg[reg_x], f->cpu->reg[reg_y]);
	inprint(g->renderer, cpustatus, x,  y+9);

	char istatus[50];
	Instruction i = parse(mmap_famicom_read(f, f->cpu->pc));
	byte oper1 = mmap_famicom_read(f, f->cpu->pc + 1);
	byte oper2 = 0;
	word opera = 0;
	switch (i.a) {
	default:
		break;
	case absolute:
	case absolute_indirect:
	case absolute_x:
	case absolute_y:
		oper2 = mmap_famicom_read(f, f->cpu->pc + 2);
		opera = bytes_to_word(oper2, oper1);
		break;
	}
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
			incolor(0x00FF00, 0);
		} else {
			incolor(0x666666, 0);
		}
		inprint(g->renderer, flags[i], x+(i*9), y+18);
	}
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
		snprintf(addr_mode, sizeof(addr_mode), "0x%X (%X,%X)", opera, oper1, oper2);
		break;
	}
	snprintf(istatus, sizeof(istatus), "%s %s", f->cpu->current_instruction, addr_mode);
	incolor(0xffffff, 0);
	inprint(g->renderer, istatus, x,  y+27);
}

void draw_tile(SDL_Instance* g, Famicom* f, int tile, int x_offset, int y_offset, bool mirrored, SDL_Color palette[])
{
	for (int y=0; y<8; y++) {
		for (int x=0; x<8; x++) {
			byte plane1;
			byte plane2;
			if (mirrored) {
				plane1 = f->chr[(tile)+y];
				plane2 = f->chr[(tile)+y+8];
			} else {
				plane1 = reverse_byte_order(f->chr[(tile)+y]);
				plane2 = reverse_byte_order(f->chr[(tile)+y+8]);
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
	word table_start;
	if (table == 0) {
		table_start = 0;
	} else if (table == 1) {
		table_start = 0x1000;
	}
	int i = 0;
	for (int x=0; x<16; x++) {
		for (int y=0; y<16; y++) {
			draw_tile(g, f, (i)+table_start, (y*8)+x_offset, (x*8)+y_offset, false, palette);
			i = i + 16;
		}
	}
}
