#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include "types.h"
#include "systems/system.h"

#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "systems/apple1.h"

#include "graphics.h"

#define VERSION "0.0.0"

System selected_system;
Famicom* famicom; // todo: shove these pointers inside selected_system
Apple1* apple1;

SDL_Instance* graphics;
SDL_Color palette[4];
bool debug_file;
FILE* dfh;

void usage(char* name);
void destroy_system();
void handle_signal(int sig);
void nemu_exit();
void draw_graphics();
void famicom_loop();
void apple1_loop();

int main(int argc, char* argv[])
{
	selected_system.s = famicom_system;

	signal(SIGINT, handle_signal);

	if (selected_system.s == famicom_system && argc == 1) {
		usage(argv[0]);
		return 0;
	}

	if (2 < argc) {
		if (strcmp("-debug", argv[1]) == 0) {
			printf("logging to file\n");
			debug_file = true;
			dfh = fopen("debug.log", "w");
			if (dfh == NULL) {
				printf("couldn't open debug log\n");
				return 1;
			}
		}
	}

	switch (selected_system.s) {
	case famicom_system:
		FILE* f;
		if (debug_file) {
			f = fopen(argv[2], "rb");
		} else {
			f = fopen(argv[1], "rb");
		}
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
		selected_system.h = famicom;
		famicom->debug.rom_name = argv[1];
		famicom_reset(famicom, false);
		break;
	case apple1_system:
		apple1 = apple1_create();
		selected_system.h = apple1;
		apple1_reset(apple1);
		break;
	}

	graphics = init_graphics();
	if (graphics == NULL) {
		destroy_system();
		return 1;
	}

	switch(selected_system.s) {
	case famicom_system:
		famicom_loop();
		break;
	case apple1_system:
		apple1_loop();
		break;
	}
	SDL_SetRenderDrawColor(graphics->renderer, 0,0,0,0xFF);
	SDL_RenderClear(graphics->renderer);
	SDL_RenderPresent(graphics->renderer);
	printf("stopping\n");
	nemu_exit();
	return 0;
}

void usage (char* name)
{
	printf("%s %s\n", name, VERSION);
	printf("usage: %s [-debug] [file]\n", name);
	return;
}

void destroy_system()
{
	switch(selected_system.s) {
	case famicom_system:
		famicom_destroy(famicom);
		break;
	case apple1_system:
		apple1_destroy(apple1);
		break;
	}
}

void nemu_exit()
{
	graphics_destroy(graphics);
	destroy_system();
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
	SDL_SetRenderDrawColor(graphics->renderer_debug, 0x99,0x50,0x99,0xFF);
	SDL_RenderClear(graphics->renderer_debug);

	switch (selected_system.s) {
	case famicom_system:
		if (famicom->chr_size != 0) {
			draw_nametable(graphics, famicom, famicom->ppu->scroll_x, famicom->ppu->scroll_y, 0);
			draw_oam(graphics, famicom);
		}
		if (famicom->cpu->current_instruction_name) {
			draw_debug(graphics, selected_system, famicom->cpu, 0, 0);
		}
		if (famicom->chr_size != 0) {
			draw_pattern_table(graphics, famicom, 0, 0, 100);
			draw_pattern_table(graphics, famicom, 1, 129, 100);
		}
		break;
	case apple1_system:
		draw_debug(graphics, selected_system, apple1->cpu, 0, 0);
		break;
	}

	SDL_RenderPresent(graphics->renderer);
	SDL_RenderPresent(graphics->renderer_debug);
}

void apple1_loop()
{
	bool pause = false;
	SDL_Event e;
	while (apple1->cpu->running) {
		while (SDL_PollEvent(&e) != 0 ) {
			switch (e.type) {
			case SDL_QUIT:
				apple1->cpu->running = false;
				break;
			case SDL_KEYDOWN:
				switch (e.key.keysym.sym) {
				case SDLK_ESCAPE:
					apple1->cpu->running = false;
					break;
				case SDLK_SPACE:
					pause = !pause;
					break;
				case SDLK_LCTRL:
					apple1_step(apple1);
					draw_graphics();
					break;
				case SDLK_F2:
					apple1_reset(apple1);
					draw_graphics();
					break;
				}
			}
		}
		if (!pause) {
			for (int i=0; i<1; i++) {
				if (apple1->cpu->running) {
					apple1_step(apple1);
					if (debug_file)
						write_cpu_state(apple1->cpu, selected_system, dfh);
				}
			}
			draw_graphics();
		}
		SDL_Delay(1000 / 60);
	}
}

const int famicom_cycles = 29780;

void famicom_loop()
{
	bool pause = false;
	SDL_Event e;
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
					/*
				case SDLK_SPACE:
					pause = !pause;
					break;
				case SDLK_LCTRL:
					famicom_step(famicom);
					draw_graphics();
					break;*/
				case SDLK_F1:
					famicom_reset(famicom, true);
					famicom_step(famicom);
					draw_graphics();
				case SDLK_F2:
					famicom_reset(famicom, false);
					famicom_step(famicom);
					draw_graphics();
					break;
				case SDLK_RETURN:
					famicom->controller_p1.start = true;
					break;
				case SDLK_RSHIFT:
					famicom->controller_p1.select = true;
					break;
				case SDLK_LCTRL:
					famicom->controller_p1.button_b = true;
					break;
				case SDLK_LALT:
					famicom->controller_p1.button_a = true;
					break;
				case SDLK_UP:
					famicom->controller_p1.up = true;
					break;
				case SDLK_DOWN:
					famicom->controller_p1.down = true;
					break;
				case SDLK_LEFT:
					famicom->controller_p1.left = true;
					break;
				case SDLK_RIGHT:
					famicom->controller_p1.right = true;
					break;
				}
				break;
			case SDL_KEYUP:
				switch (e.key.keysym.sym) {
				case SDLK_RETURN:
					famicom->controller_p1.start = false;
					break;
				case SDLK_LCTRL:
					famicom->controller_p1.button_b = false;
					break;
				case SDLK_LALT:
					famicom->controller_p1.button_a = false;
					break;
				case SDLK_UP:
					famicom->controller_p1.up = false;
					break;
				case SDLK_DOWN:
					famicom->controller_p1.down = false;
					break;
				case SDLK_LEFT:
					famicom->controller_p1.left = false;
					break;
				case SDLK_RIGHT:
					famicom->controller_p1.right = false;
					break;
				}
				break;
			}
		}
		if (!pause) {
			for (int i=0; i<famicom_cycles; i++) {
				famicom_step(famicom);
				if (!famicom->cpu->running)
					break;
				if (debug_file)
					write_cpu_state(famicom->cpu, selected_system, dfh);
			}
			draw_graphics();
		}
		SDL_Delay(1000 / 60);
	}
}
