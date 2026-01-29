typedef struct sdl_instance {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Window* window_debug;
	SDL_Renderer* renderer_debug;
} SDL_Instance;

SDL_Instance* init_graphics();
void graphics_destroy(SDL_Instance* graphics);

void draw_tile(SDL_Renderer* r, Famicom* f, int tile, int x_offset, int y_offset, bool mirrored, int table, SDL_Color palette[4]);
void draw_debug(SDL_Instance* g, System system, Cpu_6502* cpu, int x, int y);
void draw_pattern_table(SDL_Instance* g, Famicom* f, int table, int x, int y);
void draw_nametable(SDL_Instance* g, Famicom* f, int x_offset, int y_offset, int table);
void draw_oam(SDL_Instance* g, Famicom* f);
