#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdlib.h>
#include <stdio.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "audio.h"

const int window_width = 256;
const int window_height = 264;

const int debug_window_width = 256;
const int debug_window_height = 812;

const int window_scale = 2;

SDL_Instance* init_graphics()
{
	SDL_Instance* instance;
	instance = malloc(sizeof(SDL_Instance));
	if (instance == NULL) {
		return NULL;
	}
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
		printf( "sdl error: %s\n", SDL_GetError() );
		return NULL;
	}
	instance->window = SDL_CreateWindow("nemu", window_width, window_height, 0);
	if (instance->window == NULL) {
		printf("unable to create window: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	instance->renderer = SDL_CreateRenderer(instance->window, NULL);
	if (instance->renderer == NULL) {
		printf("unable to create renderer: %s\n", SDL_GetError());
		SDL_Quit();
		return NULL;
	}
	if (window_scale != 1) {
		SDL_SetWindowSize(instance->window, window_width * window_scale, window_height * window_scale);
		SDL_SetRenderLogicalPresentation(instance->renderer, window_width, window_height, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
	}
	init_audio(instance);
	return instance;
}

void graphics_destroy(SDL_Instance* graphics)
{
	SDL_DestroyWindow(graphics->window);
	SDL_DestroyRenderer(graphics->renderer);
	SDL_DestroyAudioStream(graphics->stream);
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

void draw_tile(SDL_Renderer* r, Famicom* f, int tile, int x_offset, int y_offset, bool hflip, bool vflip, int table, SDL_Color palette[])
{
	word table_start;
	if (table == 0) {
		table_start = 0;
	} else if (table == 1) {
		table_start = 0x1000;
	}
	if (!vflip) {
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
					SDL_RenderPoint(r, x + x_offset, y + y_offset);
				}
			}
		}
	} else {
		for (int y=7; -1<y; y--) {
			for (int x=0; x<8; x++) {
				byte plane1;
				byte plane2;
				plane1 = f->chr[table_start + tile + y];
				plane2 = f->chr[table_start + tile + y + 8];
				if (hflip) {
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
					SDL_RenderPoint(r, x + x_offset, y + y_offset);
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
			draw_tile(g->renderer, f, i, (x*8)+x_offset, (y*8)+y_offset, false, false, table, palette);
			i = i + 16;
		}
	}
}

void draw_nametable(SDL_Instance* g, Famicom* f, int x_offset, int y_offset, int table)
{
	for (byte y=0;y<30;y++) {
		for (byte x=0;x<32;x++) {
			byte attr = f->ppu->attribute_table[table][8 * (y/4) + (x/4)];
			byte topleft = (attr & 0x03)*4;
			byte topright = (attr & 0x0c)*4;
			byte bottomleft = (attr & 0x30)*4;
			byte bottomright = (attr & 0xc0)*4;
			byte quadrant;
			const int width = 2;
			if ((x % width) <= 2 && (y % width) <= 2) {
				quadrant = topleft;
			}
			if (2 <= (x % width) && (y % width) <= 2) {
				quadrant = topright;
			}
			if ((x % width) <= 2 && 2 <= (y % width)) {
				quadrant = bottomleft;
			}
			if (2 <= (x % width) && 2 <= (y % width) ) {
				quadrant = bottomright;
			}
			SDL_Color palette[4];
			palette[0].r = 0; palette[0].g = 0; palette[0].b = 0; palette[0].a = 0xFF;
			palette[1] = palette_lookup(f,quadrant+1);
			palette[2] = palette_lookup(f,quadrant+2);
			palette[3] = palette_lookup(f,quadrant+3);
			draw_tile(g->renderer, f, f->ppu->nametable[table][32 * y + x]*16, (x*8)+x_offset, (y*8)+y_offset, false, false, f->ppu->bg_pattern_table, palette);
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
		int tile = f->ppu->oam[i][1]*16;
		byte sprite_x = f->ppu->oam[i][3];
		draw_tile(g->renderer, f, tile, sprite_x, sprite_y, hflip, vflip, f->ppu->sprite_pattern_table, palette);
	}
}

