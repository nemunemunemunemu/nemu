#include <SDL2/SDL.h>
#include <stdlib.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "inprint/SDL2_inprint.h"
const int window_width = 256;
const int window_height = 264;

const int debug_window_width = 256;
const int debug_window_height = 812;


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
	instance->window_debug = SDL_CreateWindow("debug", 0,0, debug_window_width, debug_window_height, 0);
	if (instance->window_debug == NULL) {
		printf("unable to create window: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	instance->renderer_debug = SDL_CreateRenderer(instance->window_debug, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (instance->renderer_debug == NULL) {
		printf("unable to create renderer: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	inrenderer(instance->renderer_debug);
	prepare_inline_font();
	return instance;
}

void graphics_destroy(SDL_Instance* graphics)
{
	SDL_DestroyWindow(graphics->window);
	SDL_DestroyRenderer(graphics->renderer);
	SDL_DestroyWindow(graphics->window_debug);
	SDL_DestroyRenderer(graphics->renderer_debug);
	free(graphics);
	SDL_Quit();
}

// taken from https://emudev.de/nes-emulator/palettes-attribute-tables-and-sprites/.
uint32_t EMUDEV_PALETTE[64] = {
		0x7C7C7C, 0x0000FC, 0x0000BC, 0x4428BC, 0x940084, 0xA80020, 0xA81000, 0x881400, 0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0x000000, 0x000000,
		0xBCBCBC, 0x0078F8, 0x0058F8, 0x6844FC, 0xD800CC, 0xE40058, 0xF83800, 0xE45C10, 0xAC7C00, 0x00B800, 0x00A800, 0x00A844, 0x008888, 0x000000, 0x000000, 0x000000,
		0xF8F8F8, 0x3CBCFC, 0x6888FC, 0x9878F8, 0xF878F8, 0xF85898, 0xF87858, 0xFCA044, 0xF8B800, 0xB8F818, 0x58D854, 0x58F898, 0x00E8D8, 0x787878, 0x000000, 0x000000,
		0xFCFCFC, 0xA4E4FC, 0xB8B8F8, 0xD8B8F8, 0xF8B8F8, 0xF8A4C0, 0xF0D0B0, 0xFCE0A8, 0xF8D878, 0xD8F878, 0xB8F8B8, 0xB8F8D8, 0x00FCFC, 0xF8D8F8, 0x000000, 0x000000
};

SDL_Color palette_lookup(Famicom* f, int id) {
	SDL_Color c;
	c.r = (EMUDEV_PALETTE[f->ppu->palettes[id]] & 0xFF0000) >> 16;
	c.g = (EMUDEV_PALETTE[f->ppu->palettes[id]] & 0x00FF00) >> 8;
	c.b = (EMUDEV_PALETTE[f->ppu->palettes[id]] & 0x0000FF);
	c.a = 0xFF;
	return c;
}

void draw_debug(SDL_Instance* g, System system, Cpu_6502* cpu, int x, int y)
{
	const int WHITE = 0xFFFFFF;
	const int GREEN = 0x00FF00;
	const int GREY = 0x666666;
	const int YELLOW = 0xbbaa00;
	incolor(WHITE, 0);
	char romstatus[50];
	Famicom* f;
	switch (system.s) {
	case famicom_system:
		f = (Famicom*)system.h;
		snprintf(romstatus, sizeof(romstatus), "%s, %d", f->debug.rom_name, f->debug.rom_mapper);
		inprint(g->renderer_debug, romstatus, x,  y);
		break;
	default:
		break;
	}

	char cpustatus[50];
	snprintf(cpustatus, sizeof(cpustatus), "PC:%0X A:%0X X:%0X Y:%0X S:%0X",
	    cpu->pc, cpu->reg[reg_a], cpu->reg[reg_x], cpu->reg[reg_y], cpu->reg[reg_sp]);
	inprint(g->renderer_debug, cpustatus, x,  y+9);

	char istatus[50];
	Instruction i = cpu->current_instruction;
	byte oper1 = cpu->oper[0];
	byte oper2 = cpu->oper[1];

	word opera = bytes_to_word(oper2, oper1);
	char addr_mode[50];
	switch (i.a) {
	case accumulator: case implied:
		snprintf(addr_mode, sizeof(addr_mode), " ");
		break;
	case immediate: case zeropage: case zeropage_x: case zeropage_y: case zeropage_xi: case zeropage_yi:
		snprintf(addr_mode, sizeof(addr_mode), "%X", oper1);
		break;
	case relative:
		snprintf(addr_mode, sizeof(addr_mode), "0x%X", cpu->pc + (int8_t)oper1);
		break;
	case absolute: case absolute_indirect: case absolute_x: case absolute_y:
		snprintf(addr_mode, sizeof(addr_mode), "0x%X", opera);
		break;
	}
	snprintf(istatus, sizeof(istatus), "%X %s %s", cpu->current_instruction.o, cpu->current_instruction_name, addr_mode);
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
		if (get_bit(cpu->reg[reg_p], i) == 1) {
			incolor(GREEN, 0);
		} else {
			incolor(GREY, 0);
		}
		inprint(g->renderer_debug, flags[i], x+(i*9), y+18);
	}
	incolor(0xffffff, 0);
	inprint(g->renderer_debug, istatus, x,  y+27);
	if (system.s == famicom_system) {
		if (f->debug.nmi) {
			incolor(GREEN, 0);
		} else {
			incolor(GREY, 0);
		}
		inprint(g->renderer_debug, "nmi", x,  y+36);
		incolor(WHITE, 0);
		char ppustatus[50];
		char ppuctrl[50];
		snprintf(ppuctrl, sizeof(ppuctrl), "%X00%X%X%X%X",
		    f->ppu->nmi_enable, f->ppu->bg_pattern_table, f->ppu->sprite_pattern_table, f->ppu->vram_increment, f->ppu->nametable_base);
		snprintf(ppustatus, sizeof(ppustatus), "%s  %0X        %0X", ppuctrl, f->ppu->address, f->ppu->oam_address);
		inprint(g->renderer_debug, ppustatus, x, y+45);
		inprint(g->renderer_debug, "PPUCTRL  PPUADDR  OAMADDR", x, y+54);

		char* buttons[] = {
			">",
			"<",
			"v",
			"^",
			"S",
			"s",
			"B",
			"A"
		};
		for (int i=0; i<8; i++) {
			switch (i) {
			case joypad_right:
				if (f->controller_p1.right)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_left:
				if (f->controller_p1.left)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_down:
				if (f->controller_p1.down)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_up:
				if (f->controller_p1.up)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_start:
				if (f->controller_p1.start)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_select:
				if (f->controller_p1.select)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_b:
				if (f->controller_p1.button_b)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			case joypad_a:
				if (f->controller_p1.button_a)
					inprint(g->renderer_debug, buttons[i], x+(i*9), y+63);
				break;
			}
		}
		for (int i=0; i<0x16; i++) {
			SDL_Color palette = palette_lookup(f, i);
			SDL_SetRenderDrawColor(g->renderer_debug, palette.r, palette.g, palette.b, 0xFF);
			SDL_RenderDrawPoint(g->renderer_debug, x + i, y + 74);
		}
		incolor(YELLOW, 0);
		inprint(g->renderer_debug, "0 1 2 3 4 5 6 7 8 9 A B C D E F", 0, y+247);
		incolor(WHITE, 0);
		char hex[3];
		for (int ry=0; ry<0x50;ry++) {
			for (int rx=0; rx<16; rx++) {
				byte value = mmap_famicom(f, (16 * ry + rx), 0, false);
				snprintf(hex, sizeof(hex), "%02x", value);
				inprint(g->renderer_debug, hex, x+(rx*16), y+(ry*9)+256);
			}
		}
		SDL_SetRenderDrawColor(g->renderer_debug, 0x00,0x00,0x00,0xFF);
		for (int rx=0; rx<16; rx++) {
			SDL_RenderDrawLine(g->renderer_debug, x+(rx*16)-1, y+247, x+(rx*16)-1, debug_window_height);
		}
	}
}

void draw_tile(SDL_Renderer* r, Famicom* f, int tile, int x_offset, int y_offset, bool hflip, bool vflip, int table, SDL_Color palette[])
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
			if (!hflip) {
				plane1 = reverse_byte_order(plane1);
				plane2 = reverse_byte_order(plane2);
			}
			byte first_bit = get_bit(plane1, x);
			byte second_bit = get_bit(plane2, x);
			if (!(first_bit == 0 && second_bit == 0)) {
				if (first_bit == 1 && second_bit == 0) {
					SDL_SetRenderDrawColor(r, palette[1].r, palette[1].g, palette[1].b, 0xFF);
				} else if (first_bit == 0 && second_bit == 1) {
					SDL_SetRenderDrawColor(r, palette[2].r, palette[2].g, palette[2].b, 0xFF);
				} else if (first_bit == 1 && second_bit == 1) {
					SDL_SetRenderDrawColor(r, palette[3].r, palette[3].g, palette[3].b, 0xFF);
				}
				if (vflip) {
					SDL_RenderDrawPoint(r, y + x_offset, x + y_offset);
				} else {
					SDL_RenderDrawPoint(r, x + x_offset, y + y_offset);
				}
			}
		}
	}
}



void draw_pattern_table(SDL_Instance* g, Famicom* f, int table, int x_offset, int y_offset)
{
	SDL_Color palette[4];
	palette[0].r = 0; palette[0].g = 0; palette[0].b = 0; palette[0].a = 0xFF;
	palette[1] = palette_lookup(f,1);
	palette[2] = palette_lookup(f,2);
	palette[3] = palette_lookup(f,3);

	int i = 0;
	for (int y=0; y<16; y++) {
		for (int x=0; x<16; x++) {
			draw_tile(g->renderer_debug, f, i, (x*8)+x_offset, (y*8)+y_offset, false, false, table, palette);
			i = i + 16;
		}
	}
}

void draw_nametable(SDL_Instance* g, Famicom* f, int x_offset, int y_offset, int table)
{
	for (int y=0;y<30;y++) {
		for (int x=0;x<32;x++) {
			byte attr = f->ppu->attribute_table[0][8 * (y/4) + (x/4)];
			byte topleft = (attr & 0x03)*4;
			byte topright = (attr & 0x0c)*4;
			byte bottomleft = (attr & 0x30)*4;
			byte bottomright = (attr & 0xc0)*4;
			byte quadrant;
			if ((x % 2) < 2 && (y % 2) < 2) {
				quadrant = topleft;
			}
			if (2 <= (x % 2) && (y % 2) < 2) {
				quadrant = topright;
			}
			if ((x % 2) < 2 && 2 <= (y % 2)) {
				quadrant = bottomleft;
			}
			if (2 <= (x % 2) && 2 < (y % 2) ) {
				quadrant = bottomright;
			}
			SDL_Color palette[4];
			palette[0].r = 0; palette[0].g = 0; palette[0].b = 0; palette[0].a = 0xFF;
			palette[1] = palette_lookup(f,quadrant+1);
			palette[2] = palette_lookup(f,quadrant+2);
			palette[3] = palette_lookup(f,quadrant+3);
			draw_tile(g->renderer, f, f->ppu->nametable[table][32*y+x]*16, (x*8)+x_offset, (y*8)+y_offset, false, false, f->ppu->bg_pattern_table, palette);
		}
	}
}

void draw_oam(SDL_Instance* g, Famicom* f)
{
	for (int i=0; i<64; i++) {
		byte hflip = (f->ppu->oam[i][2]&0x80);
		byte vflip = (f->ppu->oam[i][2]&0x40);
		byte sprite_palette = (f->ppu->oam[i][2]&0x03) + 0x10;
		SDL_Color palette[4];
		palette[0] = palette_lookup(f,sprite_palette);
		palette[1] = palette_lookup(f,sprite_palette+1);
		palette[2] = palette_lookup(f,sprite_palette+2);
		palette[3] = palette_lookup(f,sprite_palette+3);
		byte sprite_y = f->ppu->oam[i][0];
		byte tile = f->ppu->oam[i][1]*16;
		byte sprite_x = f->ppu->oam[i][3];
		draw_tile(g->renderer, f, tile, sprite_x, sprite_y, hflip, vflip, f->ppu->sprite_pattern_table, palette);
	}
}

