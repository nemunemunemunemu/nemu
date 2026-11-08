#include <raylib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#include "types.h"
#include "systems/system.h"

#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"

#define VERSION "0.0.0"

#ifdef _WIN32
// windows.h clashes with raylib's names, so just the one function we need
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int code_page);
#endif

System selected_system;
Famicom famicom;

bool debug_file = false;
FILE* dfh;

void usage(char* name);
void destroy_system();
void handle_signal(int sig);
void nemu_exit();
void famicom_loop();

int window_scale;

int main(int argc, char* argv[])
{
	selected_system.s = famicom_system;

#ifdef _WIN32
	// paths are utf-8 thanks to the manifest, let the console print them too
	SetConsoleOutputCP(65001);
#endif
	signal(SIGINT, handle_signal);

	if (1 < argc && (strcmp("-h", argv[1]) == 0 || strcmp("--help", argv[1]) == 0)) {
		usage(argv[0]);
		return 0;
	}

	/*
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
		}*/

	switch (selected_system.s) {
	case famicom_system:
		char* filename = NULL;
		if (debug_file) {
			if (2 < argc)
				filename = argv[2];
		} else if (1 < argc) {
			filename = argv[1];
		}
		if (famicom_create(&famicom) == -1)
			return 1;
		selected_system.h = &famicom;
		// without a rom the cartridge slot starts empty, one can be picked from the ui
		if (filename != NULL && famicom_insert_cartridge(&famicom, filename) == -1)
			printf("error loading rom\n");
		break;
	}
	graphics_init();
	switch(selected_system.s) {
	default:
	case famicom_system:
		famicom_loop();
		break;
	}
	nemu_exit();
	return 0;
}

void usage (char* name)
{
	printf("%s %s\n", name, VERSION);
	printf("usage: %s [-debug] [file]\n", name);
	printf("without a file the cartridge slot starts empty, load one from the Famicom window\n");
	return;
}

void destroy_system()
{
	switch(selected_system.s) {
	case famicom_system:
		famicom_destroy(&famicom);
		break;
	}
}

void nemu_exit()
{
	graphics_destroy();
	destroy_system();
	if (debug_file && dfh != NULL) {
		fclose(dfh);
		dfh = NULL;
	}
}

void handle_signal(int sig)
{
	printf("caught signal, exiting\n");
	nemu_exit();
	exit(sig);
}

void famicom_loop()
{
	while (!WindowShouldClose()) {
		graphics_draw(&famicom);
	}
}
