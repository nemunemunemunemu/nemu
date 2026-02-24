typedef struct sdl_instance {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_AudioStream* stream;
	SDL_Texture* ppu_texture;
} SDL_Instance;

SDL_Instance* init_graphics();
void graphics_destroy(SDL_Instance* graphics);

void draw_tile(SDL_Renderer* r, Famicom* f, int tile, int x_offset, int y_offset, bool hflip, bool vflip, int table, SDL_Color palette[4]);
void draw_pattern_table(SDL_Instance* g, Famicom* f, int table, int x, int y);
void draw_oam(SDL_Instance* g, Famicom* f);
void draw_ppu(SDL_Instance* g, Famicom* f);
