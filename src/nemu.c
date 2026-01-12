#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include "types.h"
#include "systems/system.h"

#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"

#include "graphics.h"

#define VERSION "0.0.1"

Famicom* famicom;
SDL_Instance* graphics;

void usage(char* name);
void handle_signal(int sig);
void nemu_exit();

int main(int argc, char* argv[])
{
	signal(SIGINT, handle_signal);

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	graphics = init_graphics();
	if (graphics == NULL) {
		return 1;
	}

	FILE* f;
	f = fopen(argv[1], "rb");
	if (f == NULL) {
		printf("couldn't open file\n");
		return 1;
	}

	famicom = famicom_create();
	if (famicom == NULL) {
		return 1;
	}
	if (famicom_load_rom(famicom, f) == 1) {
		free(famicom);
		return 1;
	}
	famicom->debug.rom_name = argv[1];
	famicom_reset(famicom);
	SDL_Color palette[4];
	palette[0].r = 0x00; palette[0].g = 0x00; palette[0].b = 0x00;
	palette[1].r = 0x00; palette[1].g = 0x00; palette[1].b = 0xAA;
	palette[2].r = 0xFF; palette[2].g = 0xBE; palette[2].b = 0xB2;
	palette[3].r = 0xDB; palette[3].g = 0x28; palette[3].b = 0x00;
	bool debug = true;
	bool pause = false;
	SDL_Event e;
	//SDL_RenderSetScale(graphics->renderer, 4, 4);
	SDL_SetRenderDrawColor(graphics->renderer, 0,0,0,0xFF);
	SDL_RenderClear(graphics->renderer);
	while (famicom->cpu->running) {
		while( SDL_PollEvent( &e ) != 0 ) {
			switch(e.type) {
			case SDL_QUIT:
				famicom->cpu->running = false;
				break;
			case SDL_KEYDOWN:
				switch(e.key.keysym.scancode) {
				case SDL_SCANCODE_D:
					debug = !debug;
					break;
				case SDL_SCANCODE_P:
					pause = !pause;
					break;
				case SDL_SCANCODE_Q:
					famicom->cpu->running = false;
					break;
				case SDL_SCANCODE_S:
					famicom_step(famicom);
					break;
				case SDL_SCANCODE_R:
					famicom_reset(famicom);
					break;
				default:
					break;
				}
				break;
			}
		}
		if (!pause) {
			famicom_step(famicom);
		}
		if (famicom->cycles % 2500 == 0 || debug) {
			SDL_SetRenderDrawColor(graphics->renderer, 0,0,0,0xFF);
			SDL_RenderClear(graphics->renderer);
			draw_nametable(graphics, famicom, 0, 0, palette);
			if (strcmp(famicom->cpu->current_instruction_name, "") != 0 && debug) {
				draw_debug(graphics, famicom, 256, 0);
				draw_pattern_table(graphics, famicom, 0, 256, 100, palette);
				draw_pattern_table(graphics, famicom, 1, 385, 100, palette);
			}
			SDL_RenderPresent(graphics->renderer);
		}
	}
	printf("stopping\n");
	nemu_exit();
	return 0;
}

void usage (char* name)
{
	printf("%s %s\n", name, VERSION);
	printf("usage: %s [file]\n", name);
	return;
}

void nemu_exit()
{
	SDL_DestroyWindow(graphics->window);
	SDL_DestroyRenderer(graphics->renderer);
	free(graphics);
	famicom_destroy(famicom);
}

void handle_signal(int sig)
{
	printf("caught signal, exiting\n");
	nemu_exit();
	exit(sig);
}
