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

#define VERSION "0.0.0"

Famicom* famicom;
SDL_Instance* graphics;
SDL_Color palette[4];
bool debug_file;
FILE* dfh;


void usage(char* name);
void handle_signal(int sig);
void nemu_exit();
void draw_graphics ();

int main(int argc, char* argv[])
{
	palette[0].r = 0x00; palette[0].g = 0x00; palette[0].b = 0x00;
	palette[1].r = 0x00; palette[1].g = 0x00; palette[1].b = 0xAA;
	palette[2].r = 0xFF; palette[2].g = 0xBE; palette[2].b = 0xB2;
	palette[3].r = 0xDB; palette[3].g = 0x28; palette[3].b = 0x00;

	signal(SIGINT, handle_signal);

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	FILE* f;
	f = fopen(argv[1], "rb");
	if (f == NULL) {
		printf("couldn't open file\n");
		return 1;
	}

	if (2 < argc) {
		if (strcmp("-debug", argv[2]) == 0) {
			printf("logging to file\n");
			debug_file = true;
			dfh = fopen("debug.log", "w");
			if (dfh == NULL) {
				printf("couldn't open debug log\n");
				return 1;
			}
		}
	}
	famicom = famicom_create();
	if (famicom == NULL) {
		return 1;
	}
	if (famicom_load_rom(famicom, f) == 1) {
		free(famicom);
		return 1;
	}

	graphics = init_graphics();
	if (graphics == NULL) {
		famicom_destroy(famicom);
		return 1;
	}

	famicom->debug.rom_name = argv[1];
	famicom_reset(famicom);
	bool pause = false;
	SDL_Event e;

	SDL_SetRenderDrawColor(graphics->renderer, 0,0,0,0xFF);
	SDL_RenderClear(graphics->renderer);
	SDL_RenderPresent(graphics->renderer);
	System system;
	system.h = famicom;
	system.s = famicom_system;
	while (famicom->cpu->running) {
		while( SDL_PollEvent( &e ) != 0 ) {
			switch(e.type) {
			case SDL_QUIT:
				famicom->cpu->running = false;
				break;
			case SDL_KEYDOWN:
				switch (e.key.keysym.sym) {
				case SDLK_ESCAPE:
					famicom->cpu->running = false;
					break;
				case SDLK_SPACE:
					pause = !pause;
					break;
				case SDLK_LCTRL:
					famicom_step(famicom);
					draw_graphics();
					break;
				case SDLK_F2:
					famicom_reset(famicom);
					break;
				default:
					break;
				}
			}
		}
		if (!pause) {
			if (debug_file)
				write_cpu_state(famicom->cpu, system, dfh);
			famicom_step(famicom);
		}
		if (famicom->cycles % 1000 == 0 && !pause) {
			draw_graphics();
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
	graphics_destroy(graphics);
	famicom_destroy(famicom);
	if (debug_file)
		fclose(dfh);
}

void handle_signal(int sig)
{
	printf("caught signal, exiting\n");
	nemu_exit();
	exit(sig);
}

void draw_graphics ()
{
	SDL_SetRenderDrawColor(graphics->renderer, 0,0,0,0xFF);
	SDL_RenderClear(graphics->renderer);
	SDL_SetRenderDrawColor(graphics->renderer_debug, 0,0,0x80,0xFF);
	SDL_RenderClear(graphics->renderer_debug);
	if (famicom->chr_size != 0) {
		draw_nametable(graphics, famicom, 0, 0);
		draw_oam(graphics, famicom, palette);
	}
	if (strcmp(famicom->cpu->current_instruction_name, "")) {
		draw_debug(graphics, famicom, 0, 0);
		if (famicom->chr_size != 0) {
			draw_pattern_table(graphics, famicom, 0, 0, 100, palette);
			draw_pattern_table(graphics, famicom, 1, 129, 100, palette);
		}
	}
	SDL_RenderPresent(graphics->renderer);
	SDL_RenderPresent(graphics->renderer_debug);
}
