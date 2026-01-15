typedef struct sdl_instance {
	SDL_Window* window;
	SDL_Renderer* renderer;
} SDL_Instance;

SDL_Instance* init_graphics();
void draw_tile(SDL_Instance* g, Famicom* f, int tile, int x_offset, int y_offset, bool mirrored, int table, SDL_Color palette[]);
void draw_debug(SDL_Instance* g, Famicom* f, int x, int y);
void draw_pattern_table(SDL_Instance* g, Famicom* f, int table, int x, int y, SDL_Color palette[]);
void draw_nametable(SDL_Instance* g, Famicom* f, int x, int y);
void draw_oam(SDL_Instance* g, Famicom* f, SDL_Color palette[]);
