#include "types.h"
#include "systems/system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include "chips/6502.h"
#include "chips/2C02.h"
#include "systems/famicom.h"
#include "systems/mappers.h"
#include "systems/fds.h"
#include "bitmath.h"
#include "audio.h"
#include <raylib.h>
#include "input.h"
#include "graphics.h"
#include "ntsc.h"

#define RAYLIB_NUKLEAR_IMPLEMENTATION
#define NK_INCLUDE_DEFAULT_FONT
#include "raylib-nuklear.h"
#include <rlgl.h>

#define GET_BIT(b,i) (b>>i) & 1

const int screenwidth = 1580;
const int screenheight = 800;
struct nk_context *ctx;

// row heights follow the font size picked in the configuration window
int ui_font_size = 15;
int ui_font_pending = 0; // a new size waiting to be applied between frames
#define UI_ROW (ui_font_size + 4)
#define UI_BUTTON (ui_font_size + 8)

Font ui_font;
bool ui_font_loaded = false;
char ui_font_path[1024];
int ui_theme = 1;
Color background_color = { 24, 24, 28, 255 };
bool config_open = false;
bool config_dirty = false; // settings changed and waiting to be written

enum picker_mode {
	picker_rom,
	picker_palette,
	picker_bios,
	picker_wave,
	picker_ffmpeg,
};

// the .wav each tonal channel's sample waveform came from
char wave_file[wave_channel_count][1024];
int picker_wave_channel = 0;

// the palette picked, remembered across runs: a built in preset or a file
int config_palette_preset = 0;
char palette_file[1024];
extern int palette_preset;
static void palette_presets_init();
static void palette_restore();
static int palette_use_file(const char* path);
static void favorites_load(void);
static void favorites_free(void);
static void export_stop(void);
static void export_begin_fade(void);
extern char ffmpeg_path[1024];
extern int export_size;

// reads a .wav, mixes it down to mono and hands it to the audio side as one cycle
static int wave_load_file(int channel, const char* path)
{
	Wave wave = LoadWave(path);
	if (!IsWaveValid(wave))
		return -1;
	float* samples = LoadWaveSamples(wave);
	int frames = wave.frameCount;
	int channels = wave.channels;
	if (samples == NULL || frames < 2 || channels < 1) {
		if (samples)
			UnloadWaveSamples(samples);
		UnloadWave(wave);
		return -1;
	}
	if (AUDIO_MAX_WAVE_SAMPLES < frames)
		frames = AUDIO_MAX_WAVE_SAMPLES;
	float* mono = malloc(frames * sizeof(float));
	if (mono) {
		for (int i=0; i<frames; i++) {
			float sum = 0;
			for (int c=0; c<channels; c++)
				sum += samples[i * channels + c];
			mono[i] = sum / channels;
		}
		audio_set_wave_sample(channel, mono, frames);
		free(mono);
	}
	UnloadWaveSamples(samples);
	UnloadWave(wave);
	if (mono == NULL)
		return -1;
	snprintf(wave_file[channel], sizeof(wave_file[channel]), "%s", path);
	return 0;
}

// copies a path setting up to the end of its line, paths can hold spaces
static void config_path_value(const char* value, char* out, size_t size)
{
	const char* end = strchr(value, '\n');
	size_t length = end ? (size_t)(end - value) : strlen(value);
	if (0 < length && value[length - 1] == '\r')
		length--;
	if (size <= length)
		length = size - 1;
	memcpy(out, value, length);
	out[length] = '\0';
}

#define SCOPE_MAX_WIDTH 1024
#define SCOPE_MAX_HEIGHT 400
#define SCOPE_MAX_SAMPLES SCOPE_MAX_WIDTH // one sample per pixel, wider scopes show more time

RenderTexture2D ppu_fb_tex;
RenderTexture2D channel_scope_tex[channel_count];
RenderTexture2D pattern_table;
int scope_width = 200;
int scope_height = 40;
int ppu_scale = 2;

// blargg's ntsc filter on the gpu: the frame is stretched to 640 wide, encoded to a
// composite signal by pass 0, then decoded into ntsc_target at display size by pass 1
#define NTSC_WIDTH 640
Shader ntsc_pass0, ntsc_pass1;
bool ntsc_ready = false;
nk_bool ntsc_enabled = false;
RenderTexture2D ntsc_wide; // the frame at 640x240
RenderTexture2D ntsc_signal_tex; // pass 0's composite signal
RenderTexture2D ntsc_target;

// the filter's settings, at the libretro shader's defaults
typedef struct ntsc_settings {
	float saturation, resolution, sharpness, fringing, artifacts, bleed, luma_cutoff;
	float brightness, hue, phase_angle, vertical_scale, kernel_half;
	bool dot_crawl;
} Ntsc_settings;
const Ntsc_settings ntsc_defaults = { 2.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f, 0.0f, 96.0f, 0.6667f, 16.0f, true };
Ntsc_settings ntsc = { 2.0f, 0.0f, 0.1f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f, 0.0f, 96.0f, 0.6667f, 16.0f, true };

AudioStream stream;
bool graphics_ready = false;

static void picker_close();
static void palette_load_default();
static Texture2D debug_texture(int width, int height);
#define OAM_GRID_W (8 * 10)
#define OAM_GRID_H (8 * 18)
extern Texture2D nametable_tex;
extern Texture2D sprite_layer_tex;
extern Texture2D oam_grid_tex;
extern RenderTexture2D piano_tex;

static void audio_callback(void* buffer, unsigned int frames)
{
	audio_pull((float*)buffer, frames);
}

// the font file stays in memory so the atlas can be rebuilt when sizes or glyphs change
unsigned char* ui_font_data = NULL;
int ui_font_data_size = 0;

// glyphs in the atlas: latin and kana up front, kanji added as file names need them
#define UI_GLYPH_MAX 8192
int ui_glyphs[UI_GLYPH_MAX];
int ui_glyph_count = 0;
unsigned char ui_glyph_present[0x10000 / 8];
bool ui_font_rebuild = false;

static void ui_add_glyph(int codepoint)
{
	if (codepoint < 0 || 0xFFFF < codepoint || UI_GLYPH_MAX <= ui_glyph_count)
		return;
	if (ui_glyph_present[codepoint >> 3] & (1 << (codepoint & 7)))
		return;
	ui_glyph_present[codepoint >> 3] |= 1 << (codepoint & 7);
	ui_glyphs[ui_glyph_count++] = codepoint;
	ui_font_rebuild = true;
}

// makes sure every character in a utf-8 string can be drawn
static void ui_need_text(const char* text)
{
	if (text == NULL || !ui_font_data)
		return;
	while (*text) {
		int size = 0;
		int codepoint = GetCodepointNext(text, &size);
		if (0x80 <= codepoint)
			ui_add_glyph(codepoint);
		text += size ? size : 1;
	}
}

static void ui_base_glyphs()
{
	for (int c=0x20; c<=0x7E; c++) ui_add_glyph(c);
	for (int c=0xA0; c<=0xFF; c++) ui_add_glyph(c);   // latin-1
	for (int c=0x2010; c<=0x203F; c++) ui_add_glyph(c); // dashes, quotes, ellipsis
	for (int c=0x3000; c<=0x30FF; c++) ui_add_glyph(c); // cjk punctuation, hiragana, katakana
	for (int c=0xFF01; c<=0xFF9F; c++) ui_add_glyph(c); // fullwidth and halfwidth forms
	ui_font_rebuild = false;
}

static unsigned int read_be32(const unsigned char* p)
{
	return (unsigned int)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

// raylib only loads plain fonts, so for a .ttc collection the first face's table
// directory is moved to the front; its table offsets are from the file start anyway
static bool ttc_to_ttf(unsigned char* data, int size)
{
	if (size < 16 || memcmp(data, "ttcf", 4) != 0)
		return true;
	unsigned int offset = read_be32(data + 12);
	if (size < (int)offset + 12)
		return false;
	int tables = data[offset + 4] << 8 | data[offset + 5];
	int header = 12 + 16 * tables;
	if (size < (int)offset + header)
		return false;
	memmove(data, data + offset, header);
	return true;
}

static Font ui_build_font(int size)
{
	Font font = LoadFontFromMemory(".ttf", ui_font_data, ui_font_data_size, size, ui_glyphs, ui_glyph_count);
	if (IsFontValid(font))
		SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
	return font;
}

// nuklear hands over byte lengths, measure exactly those bytes
static float ui_text_width(nk_handle handle, float height, const char* text, int len)
{
	if (len <= 0)
		return 0;
	char buffer[1024];
	if ((int)sizeof(buffer) <= len)
		len = sizeof(buffer) - 1;
	memcpy(buffer, text, len);
	buffer[len] = '\0';
	float spacing = height * RAYLIB_NUKLEAR_FONT_SPACING_RATIO;
	return MeasureTextEx(*(Font*)handle.ptr, buffer, height, spacing).x + spacing;
}

// the first font found wins, japanese capable ones first so file names show up,
// raylib's built in font is the fallback
static void load_ui_font()
{
	const char* paths[] = {
		TextFormat("%sresources/fonts/ui.ttf", GetApplicationDirectory()),
		"resources/fonts/ui.ttf",
		"C:/Windows/Fonts/YuGothM.ttc",
		"C:/Windows/Fonts/YuGothR.ttc",
		"C:/Windows/Fonts/meiryo.ttc",
		"C:/Windows/Fonts/msgothic.ttc",
		"C:/Windows/Fonts/segoeui.ttf",
		"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/TTF/DejaVuSans.ttf",
		"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	};
	char path[1024];
	ui_base_glyphs();
	for (size_t i=0; i<sizeof(paths)/sizeof(paths[0]); i++) {
		// TextFormat buffers get reused, keep a copy
		snprintf(path, sizeof(path), "%s", paths[i]);
		if (!FileExists(path))
			continue;
		ui_font_data = LoadFileData(path, &ui_font_data_size);
		if (ui_font_data == NULL)
			continue;
		if (ttc_to_ttf(ui_font_data, ui_font_data_size)) {
			ui_font = ui_build_font(ui_font_size);
			if (IsFontValid(ui_font)) {
				ctx = InitNuklearEx(ui_font, ui_font_size);
				((struct nk_user_font*)ctx->style.font)->width = ui_text_width;
				snprintf(ui_font_path, sizeof(ui_font_path), "%s", path);
				ui_font_loaded = true;
				return;
			}
		}
		UnloadFileData(ui_font_data);
		ui_font_data = NULL;
	}
	ctx = InitNuklear(ui_font_size);
}

// swaps the glyphs under nuklear's font in place, so window state survives
static void set_ui_font_size(int size)
{
	struct nk_user_font* user_font = (struct nk_user_font*)ctx->style.font;
	if (ui_font_loaded) {
		Font font = ui_build_font(size);
		if (!IsFontValid(font))
			return;
		UnloadFont(ui_font);
		ui_font = font;
		*(Font*)user_font->userdata.ptr = font;
	}
	user_font->height = size;
	ui_font_size = size;
	ui_font_rebuild = false;
}

typedef struct ui_theme {
	const char* name;
	struct nk_color text, window, header, border, widget, hover, active, accent;
	Color background;
} Ui_theme;

// "Classic" is nuklear's own style, the rest are built from a few base colours
const Ui_theme themes[] = {
	{ "Classic", {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, { 112, 31, 126, 255 } },
	{ "Dark", {210,210,214,255}, {32,33,36,255}, {24,25,28,255}, {55,57,62,255}, {48,50,55,255}, {62,65,72,255}, {75,80,90,255}, {80,140,220,255}, { 18, 18, 20, 255 } },
	{ "Light", {30,30,30,255}, {236,236,236,255}, {210,210,212,255}, {175,175,180,255}, {220,220,222,255}, {200,200,204,255}, {185,185,190,255}, {60,120,215,255}, { 196, 198, 204, 255 } },
	{ "Blue", {220,225,235,255}, {30,40,60,255}, {20,30,50,255}, {50,70,100,255}, {45,60,90,255}, {60,80,120,255}, {70,95,140,255}, {90,160,255,255}, { 15, 20, 32, 255 } },
	{ "Red", {235,222,222,255}, {50,28,28,255}, {38,20,20,255}, {90,45,45,255}, {75,38,38,255}, {100,50,50,255}, {120,60,60,255}, {225,85,70,255}, { 25, 12, 12, 255 } },
	{ "Terminal", {140,240,150,255}, {15,30,18,255}, {10,22,12,255}, {40,90,50,255}, {25,50,30,255}, {35,70,42,255}, {45,90,55,255}, {90,220,110,255}, { 5, 12, 6, 255 } },
};
const int theme_count = sizeof(themes) / sizeof(themes[0]);

static void apply_theme(int index)
{
	if (index < 0 || theme_count <= index)
		index = 0;
	ui_theme = index;
	const Ui_theme* t = &themes[index];
	background_color = t->background;
	if (index == 0) {
		nk_style_default(ctx);
		return;
	}
	struct nk_color table[NK_COLOR_COUNT];
	for (int i=0; i<NK_COLOR_COUNT; i++)
		table[i] = t->widget;
	table[NK_COLOR_TEXT] = t->text;
	table[NK_COLOR_WINDOW] = t->window;
	table[NK_COLOR_HEADER] = t->header;
	table[NK_COLOR_BORDER] = t->border;
	table[NK_COLOR_BUTTON] = t->widget;
	table[NK_COLOR_BUTTON_HOVER] = t->hover;
	table[NK_COLOR_BUTTON_ACTIVE] = t->active;
	table[NK_COLOR_TOGGLE] = t->widget;
	table[NK_COLOR_TOGGLE_HOVER] = t->hover;
	table[NK_COLOR_TOGGLE_CURSOR] = t->accent;
	table[NK_COLOR_SELECT] = t->window;
	table[NK_COLOR_SELECT_ACTIVE] = t->accent;
	table[NK_COLOR_SLIDER] = t->widget;
	table[NK_COLOR_SLIDER_CURSOR] = t->accent;
	table[NK_COLOR_SLIDER_CURSOR_HOVER] = t->hover;
	table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = t->active;
	table[NK_COLOR_PROPERTY] = t->widget;
	table[NK_COLOR_EDIT] = t->header;
	table[NK_COLOR_EDIT_CURSOR] = t->text;
	table[NK_COLOR_COMBO] = t->widget;
	table[NK_COLOR_CHART] = t->widget;
	table[NK_COLOR_CHART_COLOR] = t->accent;
	table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = t->hover;
	table[NK_COLOR_SCROLLBAR] = t->header;
	table[NK_COLOR_SCROLLBAR_CURSOR] = t->widget;
	table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = t->hover;
	table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = t->active;
	table[NK_COLOR_TAB_HEADER] = t->header;
	nk_style_from_table(ctx, table);
}

// effect settings, stored as whole numbers: name, the setting, and its scale
typedef struct config_float {
	const char* name;
	float* value;
	float scale;
	float min, max;
} Config_float;

static const Config_float effect_floats[] = {
	{ "fx_limit", &audio_fx.limiter_threshold, 10, -24, 0 },
	{ "fx_chorus_mix", &audio_fx.chorus_mix, 100, 0, 1 },
	{ "fx_chorus_rate", &audio_fx.chorus_rate, 100, 0.05f, 5 },
	{ "fx_chorus_depth", &audio_fx.chorus_depth, 10, 0.1f, 10 },
	{ "fx_reverb_mix", &audio_fx.reverb_mix, 100, 0, 1 },
	{ "fx_room", &audio_fx.reverb_size, 100, 0, 1 },
	{ "fx_damping", &audio_fx.reverb_damping, 100, 0, 1 },
	{ "fx_width", &audio_fx.width, 100, 0, 3 },
	{ "fx_surround", &audio_fx.surround, 100, 0, 1 },
	{ "master_volume", &audio_master_volume, 100, 0, 2 },
	{ "pitch_shift", &audio_pitch_shift, 100, -AUDIO_MAX_PITCH_SHIFT, AUDIO_MAX_PITCH_SHIFT },
	{ "ntsc_saturation", &ntsc.saturation, 100, 0, 6 },
	{ "ntsc_resolution", &ntsc.resolution, 100, -1, 1 },
	{ "ntsc_sharpness", &ntsc.sharpness, 100, -1, 1 },
	{ "ntsc_fringing", &ntsc.fringing, 100, 0, 1 },
	{ "ntsc_artifacts", &ntsc.artifacts, 100, 0, 1 },
	{ "ntsc_bleed", &ntsc.bleed, 100, -0.75f, 2 },
	{ "ntsc_luma_cutoff", &ntsc.luma_cutoff, 1000, 0, 1 },
	{ "ntsc_brightness", &ntsc.brightness, 100, 0, 2 },
	{ "ntsc_hue", &ntsc.hue, 100, -1, 6.3f },
	{ "ntsc_phase_angle", &ntsc.phase_angle, 1, 1, 360 },
	{ "ntsc_vertical_scale", &ntsc.vertical_scale, 10000, 0, 2 },
	{ "ntsc_kernel", &ntsc.kernel_half, 1, 1, 16 },
	{ "fx_echo_time", &audio_fx.echo_time, 1, 20, 1000 },
	{ "fx_echo_feedback", &audio_fx.echo_feedback, 100, 0, 0.95f },
	{ "fx_echo_mix", &audio_fx.echo_mix, 100, 0, 1 },
	{ "eq_freq_0", &audio_fx.eq_freq[0], 1, 20, 20000 },
	{ "eq_freq_1", &audio_fx.eq_freq[1], 1, 20, 20000 },
	{ "eq_freq_2", &audio_fx.eq_freq[2], 1, 20, 20000 },
	{ "eq_freq_3", &audio_fx.eq_freq[3], 1, 20, 20000 },
	{ "eq_freq_4", &audio_fx.eq_freq[4], 1, 20, 20000 },
	{ "eq_gain_0", &audio_fx.eq_gain[0], 10, -EQ_MAX_GAIN, EQ_MAX_GAIN },
	{ "eq_gain_1", &audio_fx.eq_gain[1], 10, -EQ_MAX_GAIN, EQ_MAX_GAIN },
	{ "eq_gain_2", &audio_fx.eq_gain[2], 10, -EQ_MAX_GAIN, EQ_MAX_GAIN },
	{ "eq_gain_3", &audio_fx.eq_gain[3], 10, -EQ_MAX_GAIN, EQ_MAX_GAIN },
	{ "eq_gain_4", &audio_fx.eq_gain[4], 10, -EQ_MAX_GAIN, EQ_MAX_GAIN },
};

typedef struct config_bool {
	const char* name;
	bool* value;
} Config_bool;

static const Config_bool effect_bools[] = {
	{ "fx_limiter", &audio_fx.limiter },
	{ "fx_chorus", &audio_fx.chorus },
	{ "fx_reverb", &audio_fx.reverb },
	{ "fx_widen", &audio_fx.widen },
	{ "fx_follow_duty", &audio_fx.follow_duty },
	{ "fx_echo", &audio_fx.echo },
	{ "fx_echo_pingpong", &audio_fx.echo_pingpong },
	{ "fx_eq", &audio_fx.eq },
	{ "ntsc_dot_crawl", &ntsc.dot_crawl },
};

// reads one effect line, false if it isn't one
static bool config_effect(const char* line)
{
	char name[32];
	int value;
	if (sscanf(line, "%31[a-z_0-9]=%d", name, &value) != 2)
		return false;
	for (size_t i=0; i<sizeof(effect_floats)/sizeof(effect_floats[0]); i++) {
		const Config_float* c = &effect_floats[i];
		if (strcmp(name, c->name) == 0) {
			float v = value / c->scale;
			*c->value = v < c->min ? c->min : c->max < v ? c->max : v;
			return true;
		}
	}
	for (size_t i=0; i<sizeof(effect_bools)/sizeof(effect_bools[0]); i++) {
		if (strcmp(name, effect_bools[i].name) == 0) {
			*effect_bools[i].value = value != 0;
			return true;
		}
	}
	int index;
	if (sscanf(name, "mute_%d", &index) == 1 && 0 <= index && index < channel_count) {
		audio_mute[index] = value != 0;
		return true;
	}
	if (strcmp(name, "perfect_triangle") == 0) {
		audio_perfect_triangle = value != 0;
		return true;
	}
	if (strcmp(name, "fx_chorus_channels") == 0) {
		for (int c=0; c<channel_count; c++)
			audio_fx.chorus_channels[c] = value & (1 << c);
		return true;
	}
	for (int w=0; w<wave_channel_count; w++) {
		if (strcmp(name, TextFormat("wave_%d", w)) == 0) {
			audio_fx.waveform[w] = (0 <= value && value < wave_count) ? value : wave_default;
			return true;
		}
	}
	// mixer, by channel number: volume in percent and octave shift
	int channel;
	if (sscanf(name, "volume_%d", &channel) == 1 && 0 <= channel && channel < channel_count) {
		audio_gain[channel] = value < 0 ? 0 : 200 < value ? 2.0f : value / 100.0f;
		return true;
	}
	if (sscanf(name, "octave_%d", &channel) == 1 && 0 <= channel && channel < channel_count) {
		audio_transpose[channel] = value < -AUDIO_MAX_TRANSPOSE ? -AUDIO_MAX_TRANSPOSE
			: AUDIO_MAX_TRANSPOSE < value ? AUDIO_MAX_TRANSPOSE : value;
		return true;
	}
	return false;
}

// appends to text; stops rather than run past the end, snprintf reports what it wanted to write
static void config_append(char* text, size_t size, const char* line)
{
	size_t used = strlen(text);
	if (used + 1 < size)
		snprintf(text + used, size - used, "%s", line);
}

// writes the sound settings; a snapshot (for a favorite song) leaves out the video ones
// and adds the mutes and the perfect triangle, which the main config doesn't keep
static void config_write_effects(char* text, size_t size, bool snapshot)
{
	for (size_t i=0; i<sizeof(effect_floats)/sizeof(effect_floats[0]); i++) {
		if (snapshot && strncmp(effect_floats[i].name, "ntsc_", 5) == 0)
			continue;
		config_append(text, size, TextFormat("%s=%d\n", effect_floats[i].name,
						     (int)roundf(*effect_floats[i].value * effect_floats[i].scale)));
	}
	for (size_t i=0; i<sizeof(effect_bools)/sizeof(effect_bools[0]); i++) {
		if (snapshot && strncmp(effect_bools[i].name, "ntsc_", 5) == 0)
			continue;
		config_append(text, size, TextFormat("%s=%d\n", effect_bools[i].name, *effect_bools[i].value ? 1 : 0));
	}
	if (snapshot) {
		for (int c=0; c<channel_count; c++)
			config_append(text, size, TextFormat("mute_%d=%d\n", c, audio_mute[c] ? 1 : 0));
		config_append(text, size, TextFormat("perfect_triangle=%d\n", audio_perfect_triangle ? 1 : 0));
	}
	int mask = 0;
	for (int c=0; c<channel_count; c++)
		mask |= audio_fx.chorus_channels[c] ? 1 << c : 0;
	config_append(text, size, TextFormat("fx_chorus_channels=%d\n", mask));
	for (int w=0; w<wave_channel_count; w++)
		config_append(text, size, TextFormat("wave_%d=%d\n", w, audio_fx.waveform[w]));
	for (int c=0; c<channel_count; c++) {
		config_append(text, size, TextFormat("volume_%d=%d\n", c, (int)roundf(audio_gain[c] * 100)));
		config_append(text, size, TextFormat("octave_%d=%d\n", c, audio_transpose[c]));
	}
	// a file palette is kept by path, a built in one by its place in the list
	if (snapshot) {
	} else if (palette_file[0]) {
		char line[1100];
		snprintf(line, sizeof(line), "palette_file=%.1023s\n", palette_file);
		config_append(text, size, line);
	} else if (0 <= palette_preset) {
		config_append(text, size, TextFormat("palette_preset=%d\n", palette_preset));
	}
	// paths can be long, so these skip TextFormat's limit
	for (int w=0; w<wave_channel_count; w++) {
		if (!wave_file[w][0])
			continue;
		char line[1100];
		snprintf(line, sizeof(line), "wave_file_%d=%.1023s\n", w, wave_file[w]);
		config_append(text, size, line);
	}
}

// settings live next to the exe as simple key=value lines
static const char* config_path()
{
	return TextFormat("%snemu.cfg", GetApplicationDirectory());
}

static void config_load()
{
	char* text = LoadFileText(config_path());
	if (text == NULL)
		return;
	int value;
	const char* line = text;
	while (*line) {
		// paths can hold spaces, so this one is taken up to the end of the line
		if (strncmp(line, "fds_bios=", 9) == 0) {
			config_path_value(line + 9, fds_bios_path, sizeof(fds_bios_path));
		} else if (strncmp(line, "ffmpeg_path=", 12) == 0) {
			config_path_value(line + 12, ffmpeg_path, sizeof(ffmpeg_path));
		} else if (sscanf(line, "export_size=%d", &value) == 1 && 0 <= value && value < 3) {
			export_size = value;
		} else if (strncmp(line, "palette_file=", 13) == 0) {
			config_path_value(line + 13, palette_file, sizeof(palette_file));
		} else if (sscanf(line, "palette_preset=%d", &value) == 1) {
			config_palette_preset = value;
		} else if (strncmp(line, "wave_file_", 10) == 0 && '0' <= line[10] && line[10] < '0' + wave_channel_count && line[11] == '=') {
			config_path_value(line + 12, wave_file[line[10] - '0'], sizeof(wave_file[0]));
		} else if (sscanf(line, "font_size=%d", &value) == 1 && 8 <= value && value <= 40)
			ui_font_size = value;
		else if (sscanf(line, "theme=%d", &value) == 1 && 0 <= value && value < theme_count)
			ui_theme = value;
		else if (sscanf(line, "region=%d", &value) == 1 && region_auto <= value && value <= region_force_dendy)
			famicom_region_setting = value;
		else if (config_effect(line))
			;
		else if (input_config_line(line))
			;
		else if (sscanf(line, "ntsc=%d", &value) == 1)
			ntsc_enabled = value != 0;
		const char* next = strchr(line, '\n');
		if (next == NULL)
			break;
		line = next + 1;
	}
	UnloadFileText(text);
}

static void config_save()
{
	// built in a local buffer, the bios path can be longer than TextFormat allows
	static char text[8192];
	snprintf(text, sizeof(text), "font_size=%d\ntheme=%d\nregion=%d\nntsc=%d\nfds_bios=%.1023s\n",
		ui_font_size, ui_theme, famicom_region_setting, ntsc_enabled ? 1 : 0, fds_bios_path);
	config_write_effects(text, sizeof(text), false);
	input_config_write(text, sizeof(text));
	config_append(text, sizeof(text), TextFormat("export_size=%d\n", export_size));
	if (ffmpeg_path[0]) {
		char line[1100];
		snprintf(line, sizeof(line), "ffmpeg_path=%.1023s\n", ffmpeg_path);
		config_append(text, sizeof(text), line);
	}
	SaveFileText(config_path(), text);
	config_dirty = false;
}

static void picker_show(Famicom* famicom, enum picker_mode mode);

void config_window(Famicom* famicom)
{
	if (!config_open)
		return;
	if (nk_begin(ctx, "Configuration", nk_rect(260, 120, 360, 260), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_CLOSABLE|NK_WINDOW_TITLE)) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){0.4f, 0.6f});
		nk_label(ctx, "Font size", NK_TEXT_LEFT);
		int size = ui_font_size;
		nk_property_int(ctx, "#px", 8, &size, 40, 1, 0.25f);
		if (size != ui_font_size)
			ui_font_pending = size;

		nk_label(ctx, "Color scheme", NK_TEXT_LEFT);
		const char* names[16];
		for (int i=0; i<theme_count; i++)
			names[i] = themes[i].name;
		int theme = nk_combo(ctx, names, theme_count, ui_theme, UI_BUTTON, nk_vec2(180, 200));
		if (theme != ui_theme) {
			apply_theme(theme);
			config_save();
		}

		// the fds bios is nintendo's, so it has to be the user's own copy
		nk_label(ctx, "FDS BIOS", NK_TEXT_LEFT);
		if (nk_button_label(ctx, "Browse..."))
			picker_show(famicom, picker_bios);
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		if (fds_bios_path[0])
			nk_label(ctx, GetFileName(fds_bios_path), NK_TEXT_LEFT);
		else
			nk_label(ctx, "not set, looks for disksys.rom by the game", NK_TEXT_LEFT);

		// video export runs ffmpeg
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){0.4f, 0.6f});
		nk_label(ctx, "FFmpeg", NK_TEXT_LEFT);
		if (nk_button_label(ctx, "Browse..."))
			picker_show(famicom, picker_ffmpeg);
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		nk_label(ctx, ffmpeg_path[0] ? GetFileName(ffmpeg_path) : "not set, uses ffmpeg.exe by nemu or on the PATH", NK_TEXT_LEFT);
	} else {
		config_open = false;
	}
	nk_end(ctx);
}

// the controller map: each nes button's key and gamepad input, for both players;
// clicking a binding waits for the next key or button
bool controllers_open = false;
static int bind_player = 0;
static int bind_button = -1; // the button waiting for an input, -1 for none
static bool bind_pad = false; // waiting for the gamepad rather than the keyboard

static bool input_rebinding(void)
{
	return controllers_open && 0 <= bind_button;
}

static void bind_begin(int button, bool pad)
{
	bind_button = button;
	bind_pad = pad;
	input_capture_begin(input_players[bind_player].gamepad);
}

static void bind_capture(void)
{
	if (bind_button < 0)
		return;
	Input_player* p = &input_players[bind_player];
	if (IsKeyPressed(KEY_ESCAPE)) {
		bind_button = -1;
		return;
	}
	if (IsKeyPressed(KEY_DELETE)) {
		if (bind_pad)
			p->pad[bind_button] = (Pad_binding){ pad_none, 0 };
		else
			p->keys[bind_button] = 0;
		bind_button = -1;
		config_save();
		return;
	}
	if (!bind_pad) {
		int key;
		if (!input_capture_key(&key))
			return;
		// a key works one button only, wherever it was before
		for (int q=0; q<INPUT_PLAYERS; q++)
			for (int b=0; b<nes_button_count; b++)
				if (input_players[q].keys[b] == key)
					input_players[q].keys[b] = 0;
		p->keys[bind_button] = key;
	} else {
		Pad_binding pad;
		if (!input_capture_pad(p->gamepad, &pad))
			return;
		for (int b=0; b<nes_button_count; b++)
			if (p->pad[b].kind == pad.kind && p->pad[b].code == pad.code)
				p->pad[b] = (Pad_binding){ pad_none, 0 };
		p->pad[bind_button] = pad;
	}
	bind_button = -1;
	config_save();
}

void controllers_window(void)
{
	if (!controllers_open) {
		bind_button = -1;
		return;
	}
	bind_capture();
	if (nk_begin(ctx, "Controllers", nk_rect(300, 80, 420, 470), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_CLOSABLE|NK_WINDOW_TITLE)) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 3, (float[]){ 0.3f, 0.35f, 0.35f });
		nk_label(ctx, "Player", NK_TEXT_LEFT);
		for (int q=0; q<INPUT_PLAYERS; q++) {
			if (nk_option_label(ctx, q == 0 ? "1" : "2", bind_player == q) && bind_player != q) {
				bind_player = q;
				bind_button = -1;
			}
		}
		Input_player* p = &input_players[bind_player];

		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){ 0.3f, 0.7f });
		nk_label(ctx, "Gamepad", NK_TEXT_LEFT);
		static char pad_labels[INPUT_GAMEPADS][96];
		const char* pad_names[INPUT_GAMEPADS];
		for (int g=0; g<INPUT_GAMEPADS; g++) {
			if (IsGamepadAvailable(g))
				snprintf(pad_labels[g], sizeof(pad_labels[g]), "%d: %.80s", g + 1, GetGamepadName(g));
			else
				snprintf(pad_labels[g], sizeof(pad_labels[g]), "%d: not connected", g + 1);
			pad_names[g] = pad_labels[g];
		}
		int gamepad = nk_combo(ctx, pad_names, INPUT_GAMEPADS, p->gamepad, UI_BUTTON, nk_vec2(300, 160));
		if (gamepad != p->gamepad) {
			p->gamepad = gamepad;
			config_save();
		}
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		bool stick = p->stick;
		if (nk_checkbox_label(ctx, "Left stick steers too", &stick)) {
			p->stick = stick;
			config_save();
		}

		// what's held right now lights up its row, handy for checking a pad works
		Famicom_controller held;
		input_poll(&held, bind_player, false);
		bool down[nes_button_count] = { held.up, held.down, held.left, held.right, held.select, held.start, held.button_b, held.button_a };
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW, 3, (float[]){ 0.2f, 0.35f, 0.45f });
		nk_label(ctx, "Button", NK_TEXT_LEFT);
		nk_label(ctx, "Key", NK_TEXT_LEFT);
		nk_label(ctx, "Gamepad", NK_TEXT_LEFT);
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 3, (float[]){ 0.2f, 0.35f, 0.45f });
		for (int b=0; b<nes_button_count; b++) {
			if (down[b] && !input_rebinding())
				nk_label_colored(ctx, nes_button_names[b], NK_TEXT_LEFT, nk_rgb(255, 220, 0));
			else
				nk_label(ctx, nes_button_names[b], NK_TEXT_LEFT);
			bool waiting_key = bind_button == b && !bind_pad;
			if (nk_button_label(ctx, waiting_key ? "press a key..." : input_key_name(p->keys[b])))
				bind_begin(b, false);
			bool waiting_pad = bind_button == b && bind_pad;
			if (nk_button_label(ctx, waiting_pad ? "press a button..." : input_pad_name(p->pad[b])))
				bind_begin(b, true);
		}
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		if (input_rebinding() && bind_pad && !IsGamepadAvailable(p->gamepad))
			nk_label_colored(ctx, "That gamepad isn't connected", NK_TEXT_LEFT, nk_rgb(255, 80, 80));
		else
			nk_label(ctx, "Click one, then press. Esc cancels, Delete clears", NK_TEXT_LEFT);
		nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
		if (nk_button_label(ctx, "Reset this player")) {
			input_defaults(bind_player);
			bind_button = -1;
			config_save();
		}
		if (nk_button_label(ctx, "Clear keys")) {
			memset(p->keys, 0, sizeof(p->keys));
			bind_button = -1;
			config_save();
		}
	} else {
		controllers_open = false;
	}
	nk_end(ctx);
}

static void clear_texture(RenderTexture2D tex)
{
	BeginTextureMode(tex);
	ClearBackground(BLACK);
	EndTextureMode();
}

static void scope_resize(int width, int height)
{
	if (width < 32) width = 32;
	if (SCOPE_MAX_WIDTH < width) width = SCOPE_MAX_WIDTH;
	if (height < 16) height = 16;
	if (SCOPE_MAX_HEIGHT < height) height = SCOPE_MAX_HEIGHT;
	if (width == scope_width && height == scope_height && channel_scope_tex[0].id)
		return;
	scope_width = width;
	scope_height = height;
	for (int c=0; c<channel_count; c++) {
		if (channel_scope_tex[c].id)
			UnloadRenderTexture(channel_scope_tex[c]);
		channel_scope_tex[c] = LoadRenderTexture(scope_width, scope_height);
		clear_texture(channel_scope_tex[c]);
	}
}

void graphics_init()
{
	SetConfigFlags(FLAG_WINDOW_RESIZABLE);
	InitWindow(screenwidth, screenheight, "nemu");
	SetWindowMinSize(640, 400);
	SetTargetFPS(60);
	InitAudioDevice();
	for (int p=0; p<INPUT_PLAYERS; p++)
		input_defaults(p);
	config_load();
	// sample waveforms picked last time
	for (int w=0; w<wave_channel_count; w++) {
		if (!wave_file[w][0])
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%.1023s", wave_file[w]);
		if (wave_load_file(w, path) != 0) {
			printf("couldn't reload waveform %s\n", path);
			wave_file[w][0] = '\0';
		}
	}
	// a bios sitting next to the exe works without setting it up
	if (!fds_bios_path[0] && FileExists(TextFormat("%sdisksys.rom", GetApplicationDirectory())))
		snprintf(fds_bios_path, sizeof(fds_bios_path), "%sdisksys.rom", GetApplicationDirectory());
	load_ui_font();
	apply_theme(ui_theme);
	palette_presets_init();
	palette_restore();
	favorites_load();
	SetAudioStreamBufferSizeDefault(1024);
	stream = LoadAudioStream(AUDIO_SAMPLE_RATE, 32, 2); // stereo, for the effects
	SetAudioStreamCallback(stream, audio_callback);
	PlayAudioStream(stream);
	ppu_fb_tex = LoadRenderTexture(256, 240);
	pattern_table = LoadRenderTexture(256,256);
	// render textures start out with junk in them
	clear_texture(ppu_fb_tex);
	scope_resize(scope_width, scope_height);
	ntsc_pass0 = LoadShaderFromMemory(NULL, ntsc_pass0_source);
	ntsc_pass1 = LoadShaderFromMemory(NULL, ntsc_pass1_source);
	ntsc_ready = IsShaderValid(ntsc_pass0) && IsShaderValid(ntsc_pass1);
	if (ntsc_ready) {
		ntsc_wide = LoadRenderTexture(NTSC_WIDTH, 240);
		ntsc_signal_tex = LoadRenderTexture(NTSC_WIDTH, 240);
	} else {
		printf("ntsc shader failed to compile, the filter is unavailable\n");
		ntsc_enabled = false;
	}
	nametable_tex = debug_texture(512, 480);
	sprite_layer_tex = debug_texture(256, 240);
	oam_grid_tex = debug_texture(OAM_GRID_W, OAM_GRID_H);
	graphics_ready = true;
}

void graphics_destroy()
{
	if (!graphics_ready)
		return;
	graphics_ready = false;
	export_stop();
	if (config_dirty)
		config_save();
	picker_close();
	// gpu and audio resources have to go before their contexts do
	UnloadRenderTexture(ppu_fb_tex);
	for (int c=0; c<channel_count; c++)
		UnloadRenderTexture(channel_scope_tex[c]);
	UnloadRenderTexture(pattern_table);
	if (ntsc_target.id)
		UnloadRenderTexture(ntsc_target);
	if (piano_tex.id)
		UnloadRenderTexture(piano_tex);
	if (ntsc_ready) {
		UnloadRenderTexture(ntsc_wide);
		UnloadRenderTexture(ntsc_signal_tex);
	}
	if (IsShaderValid(ntsc_pass0))
		UnloadShader(ntsc_pass0);
	if (IsShaderValid(ntsc_pass1))
		UnloadShader(ntsc_pass1);
	UnloadTexture(nametable_tex);
	UnloadTexture(sprite_layer_tex);
	UnloadTexture(oam_grid_tex);
	// nuklear only frees its copy of the font struct, the glyphs are ours
	favorites_free();
	UnloadNuklear(ctx);
	if (ui_font_loaded)
		UnloadFont(ui_font);
	if (ui_font_data)
		UnloadFileData(ui_font_data);
	StopAudioStream(stream);
	UnloadAudioStream(stream);
	CloseAudioDevice();
	CloseWindow();
}

const char* channel_names[channel_count] = { "Pulse 1", "Pulse 2", "Triangle", "Noise", "DMC", "FDS", "N163", "VRC6", "VRC7" };
// bright, fully saturated, so they stand out on black
Color channel_colors[channel_count] = {
	{ 255, 30, 30, 255 },  // pulse 1, red
	{ 255, 150, 0, 255 },  // pulse 2, orange
	{ 0, 255, 60, 255 },   // triangle, green
	{ 0, 140, 255, 255 },  // noise, azure
	{ 180, 40, 255, 255 }, // dmc, violet
	{ 0, 255, 255, 255 },  // fds, cyan
	{ 255, 0, 200, 255 },  // n163, magenta
	{ 255, 240, 0, 255 },  // vrc6, yellow
	{ 80, 80, 255, 255 },  // vrc7, blue violet
};
int scope_order[channel_count] = { channel_pulse1, channel_pulse2, channel_triangle, channel_noise, channel_dmc, channel_fds, channel_n163, channel_vrc6, channel_vrc7 };

// expansion channels only get a scope while their chip is in use
static bool channel_visible(Famicom* f, int channel)
{
	if (channel == channel_fds)
		return f->fds.active;
	if (channel == channel_n163)
		return f->n163.active;
	if (channel == channel_vrc6)
		return f->vrc6.active;
	if (channel == channel_vrc7)
		return f->vrc7.active;
	return true;
}
nk_bool scope_smooth = false;
float scope_samples[SCOPE_MAX_SAMPLES * 2];
Vector2 scope_points[SCOPE_MAX_SAMPLES];

// one texture per channel, triggered on a rising edge so the waveform stands still
void draw_scope()
{
	int count = scope_width < SCOPE_MAX_SAMPLES ? scope_width : SCOPE_MAX_SAMPLES;
	float step = (float)(scope_width - 1) / (count - 1);
	for (int c=0; c<channel_count; c++) {
		audio_scope_channel(c, scope_samples, count * 2);
		float low = 1, high = 0;
		for (int i=0; i<count * 2; i++) {
			if (scope_samples[i] < low) low = scope_samples[i];
			if (high < scope_samples[i]) high = scope_samples[i];
		}
		float mid = (low + high) / 2;
		int start = 0;
		for (int i=1; i<count; i++) {
			if (scope_samples[i-1] < mid && mid <= scope_samples[i]) {
				start = i;
				break;
			}
		}
		for (int i=0; i<count; i++) {
			float v = scope_samples[start + i];
			if (scope_smooth) {
				// moving average over a few samples rounds off the steps
				float sum = 0;
				int n = 0;
				for (int k=-2; k<=2; k++) {
					if (0 <= start + i + k && start + i + k < count * 2) {
						sum += scope_samples[start + i + k];
						n++;
					}
				}
				v = sum / n;
			}
			// centred on the middle line, and flipped because render textures
			// come out upside down in nuklear
			scope_points[i].x = i * step;
			scope_points[i].y = scope_height / 2.0f + (v - mid) * (scope_height - 4);
		}
		// muted channels keep drawing, greyed out
		Color color = audio_mute[c] ? (Color){ 90, 90, 90, 255 } : channel_colors[c];
		BeginTextureMode(channel_scope_tex[c]);
		ClearBackground(BLACK);
		DrawLine(0, scope_height/2, scope_width, scope_height/2, DARKGRAY);
		if (scope_smooth) {
			DrawSplineCatmullRom(scope_points, count, 1.5f, color);
		} else {
			for (int i=1; i<count; i++)
				DrawLine(scope_points[i-1].x, scope_points[i-1].y, scope_points[i].x, scope_points[i].y, color);
		}
		EndTextureMode();
	}
}


const int default_palette[64] = {
0x7C7C7C, 0x0000FC, 0x0000BC, 0x4428BC, 0x940084, 0xA80020, 0xA81000, 0x881400, 0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0x000000, 0x000000,
0xBCBCBC, 0x0078F8, 0x0058F8, 0x6844FC, 0xD800CC, 0xE40058, 0xF83800, 0xE45C10, 0xAC7C00, 0x00B800, 0x00A800, 0x00A844, 0x008888, 0x000000, 0x000000, 0x000000,
0xF8F8F8, 0x3CBCFC, 0x6888FC, 0x9878F8, 0xF878F8, 0xF85898, 0xF87858, 0xFCA044, 0xF8B800, 0xB8F818, 0x58D854, 0x58F898, 0x00E8D8, 0x787878, 0x000000, 0x000000,
0xFCFCFC, 0xA4E4FC, 0xB8B8F8, 0xD8B8F8, 0xF8B8F8, 0xF8A4C0, 0xF0D0B0, 0xFCE0A8, 0xF8D878, 0xD8F878, 0xB8F8B8, 0xB8F8D8, 0x00FCFC, 0xF8D8F8, 0x000000, 0x000000
};

// the loaded palette, and what's left after the picture adjustments
Color base_palette[64];
Color palette[64];
char palette_name[256] = "Default";
float palette_hue = 0; // degrees
float palette_saturation = 1;
float palette_contrast = 1;

static unsigned char clamp_channel(float v)
{
	if (v < 0) return 0;
	if (255 < v) return 255;
	return (unsigned char)(v + 0.5f);
}

// hue spins the chroma, saturation scales it, contrast stretches luma around grey
static void palette_apply()
{
	float angle = palette_hue * PI / 180.0f;
	float hc = cosf(angle), hs = sinf(angle);
	for (int i=0; i<64; i++) {
		float r = base_palette[i].r / 255.0f;
		float g = base_palette[i].g / 255.0f;
		float b = base_palette[i].b / 255.0f;
		float y = 0.299f*r + 0.587f*g + 0.114f*b;
		float ci = 0.596f*r - 0.274f*g - 0.322f*b;
		float cq = 0.211f*r - 0.523f*g + 0.312f*b;
		float ri = (ci*hc - cq*hs) * palette_saturation;
		float rq = (ci*hs + cq*hc) * palette_saturation;
		y = (y - 0.5f) * palette_contrast + 0.5f;
		palette[i].r = clamp_channel((y + 0.956f*ri + 0.621f*rq) * 255);
		palette[i].g = clamp_channel((y - 0.272f*ri - 0.647f*rq) * 255);
		palette[i].b = clamp_channel((y - 1.106f*ri + 1.703f*rq) * 255);
		palette[i].a = 0xFF;
	}
}

static void palette_load_default()
{
	for (int i=0; i<64; i++) {
		base_palette[i].r = (default_palette[i] >> 16) & 0xFF;
		base_palette[i].g = (default_palette[i] >> 8) & 0xFF;
		base_palette[i].b = default_palette[i] & 0xFF;
		base_palette[i].a = 0xFF;
	}
	snprintf(palette_name, sizeof(palette_name), "Default");
	palette_apply();
}

// .pal files are 64 rgb triples, larger ones carry emphasis variants we skip
static int palette_load_file(const char* path)
{
	int size = 0;
	unsigned char* data = LoadFileData(path, &size);
	if (data == NULL)
		return -1;
	if (size < 64 * 3) {
		UnloadFileData(data);
		return -1;
	}
	for (int i=0; i<64; i++) {
		base_palette[i].r = data[i*3];
		base_palette[i].g = data[i*3 + 1];
		base_palette[i].b = data[i*3 + 2];
		base_palette[i].a = 0xFF;
	}
	UnloadFileData(data);
	snprintf(palette_name, sizeof(palette_name), "%s", GetFileName(path));
	palette_apply();
	return 0;
}

// palettes made the way a tv sees the 2C02: its square wave voltages for each colour,
// decoded back to rgb, after the method on the nesdev wiki's ntsc video page
static float ntsc_signal(int pixel, int phase)
{
	static const float attenuation = 0.746f;
	static const float levels[8] = { 0.350f, 0.518f, 0.962f, 1.550f,   // low side of the wave
	                                 1.094f, 1.506f, 1.962f, 1.962f }; // high side
	int color = pixel & 0x0F;
	int level = (pixel >> 4) & 0x03;
	int emphasis = pixel >> 6;
	if (13 < color)
		level = 1;
	float low = levels[level];
	float high = levels[4 + level];
	if (color == 0)
		low = high; // greys only sit on the high level
	if (12 < color)
		high = low; // the blacks only on the low one
	#define IN_PHASE(c) (((c) + phase) % 12 < 6)
	float signal = IN_PHASE(color) ? high : low;
	if (((emphasis & 1) && IN_PHASE(0)) || ((emphasis & 2) && IN_PHASE(4)) || ((emphasis & 4) && IN_PHASE(8)))
		signal *= attenuation;
	#undef IN_PHASE
	return signal;
}

// a tv lines its colour decoder up with the colour burst, four of the wave's twelve
// phase steps away from where it starts, so the hues sit where the nes puts them
#define BURST_ALIGNMENT 120.0f

static void generate_palette(Color out[64], float saturation, float hue, float gamma, bool grey)
{
	const float black = 0.518f, white = 1.962f;
	hue += BURST_ALIGNMENT;
	for (int pixel=0; pixel<64; pixel++) {
		// average twelve samples of the wave for luma and demodulate them for chroma
		float y = 0, i = 0, q = 0;
		for (int p=0; p<12; p++) {
			float spot = (ntsc_signal(pixel, p) - black) / (white - black);
			float angle = PI * (p + hue / 30.0f) / 6.0f;
			y += spot;
			i += spot * cosf(angle);
			q += spot * sinf(angle);
		}
		y /= 12;
		i *= saturation / 12;
		q *= saturation / 12;
		if (grey)
			i = q = 0;
		float rgb[3] = {
			y + 0.946882f * i + 0.623557f * q,
			y - 0.274788f * i - 0.635691f * q,
			y - 1.108545f * i + 1.709007f * q,
		};
		unsigned char c[3];
		for (int k=0; k<3; k++) {
			float v = rgb[k] <= 0 ? 0 : powf(rgb[k], 2.2f / gamma);
			c[k] = clamp_channel(v * 255);
		}
		out[pixel] = (Color){ c[0], c[1], c[2], 255 };
	}
}

// the presets: the built in table, generated ones, then any .pal in a palettes folder
typedef struct palette_preset {
	char name[96];
	char path[1024]; // empty for the built in ones
	float saturation, hue, gamma;
	bool grey;
	bool table; // the original built in table
} Palette_preset;

#define MAX_PALETTE_PRESETS 64
Palette_preset palette_presets[MAX_PALETTE_PRESETS];
int palette_preset_count = 0;
int palette_preset = 0; // -1 when a file was loaded by hand
const char* palette_preset_names[MAX_PALETTE_PRESETS + 1];

static void add_generated(const char* name, float saturation, float hue, float gamma, bool grey)
{
	Palette_preset* p = &palette_presets[palette_preset_count++];
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->saturation = saturation;
	p->hue = hue;
	p->gamma = gamma;
	p->grey = grey;
}

static void scan_palette_folder(const char* folder)
{
	if (!DirectoryExists(folder))
		return;
	FilePathList files = LoadDirectoryFiles(folder);
	for (unsigned int i=0; i<files.count && palette_preset_count < MAX_PALETTE_PRESETS; i++) {
		if (!IsFileExtension(files.paths[i], ".pal") || GetFileLength(files.paths[i]) < 64 * 3)
			continue;
		// the same file can turn up through two of the folders
		bool seen = false;
		for (int k=0; k<palette_preset_count; k++)
			seen |= strcmp(GetFileName(palette_presets[k].path), GetFileName(files.paths[i])) == 0 && palette_presets[k].path[0];
		if (seen)
			continue;
		Palette_preset* p = &palette_presets[palette_preset_count++];
		memset(p, 0, sizeof(*p));
		snprintf(p->name, sizeof(p->name), "%s", GetFileName(files.paths[i]));
		snprintf(p->path, sizeof(p->path), "%s", files.paths[i]);
		ui_need_text(p->name);
	}
	UnloadDirectoryFiles(files);
}

static void palette_presets_init()
{
	palette_preset_count = 0;
	Palette_preset* table = &palette_presets[palette_preset_count++];
	memset(table, 0, sizeof(*table));
	snprintf(table->name, sizeof(table->name), "nemu default");
	table->table = true;
	// gamma 2.2 leaves the decoded levels as they are, lower darkens and higher lightens
	add_generated("NTSC composite", 1.2f, 0, 2.2f, false);
	add_generated("NTSC vivid", 1.45f, 0, 2.2f, false);
	add_generated("NTSC soft", 0.95f, 0, 2.0f, false);
	add_generated("NTSC bright", 1.2f, 0, 2.6f, false);
	add_generated("NTSC dark", 1.2f, 0, 1.8f, false);
	add_generated("Greyscale", 1.0f, 0, 2.2f, true);
	char folder[1100];
	snprintf(folder, sizeof(folder), "%spalettes", GetApplicationDirectory());
	scan_palette_folder(folder);
	scan_palette_folder("palettes");
	scan_palette_folder("resources/palettes");
	for (int i=0; i<palette_preset_count; i++)
		palette_preset_names[i] = palette_presets[i].name;
}

// a .pal chosen by hand; if it's one of the folder presets that entry is selected
static int palette_use_file(const char* path)
{
	if (palette_load_file(path) != 0)
		return -1;
	palette_preset = -1;
	for (int i=0; i<palette_preset_count; i++) {
		if (palette_presets[i].path[0] && strcmp(GetFileName(palette_presets[i].path), GetFileName(path)) == 0)
			palette_preset = i;
	}
	char copy[1024];
	snprintf(copy, sizeof(copy), "%s", path);
	snprintf(palette_file, sizeof(palette_file), "%s", copy);
	ui_need_text(palette_name);
	return 0;
}

static int palette_select_preset(int index)
{
	if (index < 0 || palette_preset_count <= index)
		return -1;
	Palette_preset* p = &palette_presets[index];
	palette_file[0] = '\0';
	if (p->path[0]) {
		if (palette_load_file(p->path) != 0)
			return -1;
		snprintf(palette_file, sizeof(palette_file), "%s", p->path);
	} else if (p->table) {
		palette_load_default();
	} else {
		generate_palette(base_palette, p->saturation, p->hue, p->gamma, p->grey);
		snprintf(palette_name, sizeof(palette_name), "%s", p->name);
		palette_apply();
	}
	palette_preset = index;
	return 0;
}

// puts back the palette from last time, falling back to the default
static void palette_restore()
{
	if (palette_file[0]) {
		char path[1024];
		snprintf(path, sizeof(path), "%s", palette_file);
		if (palette_use_file(path) == 0)
			return;
		printf("couldn't reload palette %s\n", path);
		palette_file[0] = '\0';
	}
	if (palette_select_preset(config_palette_preset) != 0)
		palette_select_preset(0);
}

Color color_lookup(byte index)
{
	return palette[index & 0x3F];
}

Color palette_lookup(Famicom* f, int id) {
	return color_lookup(f->ppu.palettes[id & 0x1F]);
}

void draw_tile(Famicom* f, int tile, int x_offset, int y_offset, bool hflip, bool vflip, int table, Color palette[])
{
	word table_start = table ? 0x1000 : 0;
	for (int y=0; y<8; y++) {
		int row = vflip ? 7 - y : y;
		byte plane1 = mapper_chr_read(f, table_start + tile + row);
		byte plane2 = mapper_chr_read(f, table_start + tile + row + 8);
		if (!hflip) {
			plane1 = reverse_byte_order(plane1);
			plane2 = reverse_byte_order(plane2);
		}
		for (int x=0; x<8; x++) {
			byte pixel = (GET_BIT(plane1, x)) | (GET_BIT(plane2, x)) << 1;
			if (pixel)
				DrawPixel(x + x_offset, y + y_offset, palette[pixel]);
		}
	}
}

void draw_pattern_table(Famicom* f, int table)
{
	BeginTextureMode(pattern_table);
	Color palette[4];
	palette[0].r = 0; palette[0].g = 0; palette[0].b = 0; palette[0].a = 0xFF;
	palette[1] = palette_lookup(f,1);
	palette[2] = palette_lookup(f,2);
	palette[3] = palette_lookup(f,3);

	int i = 0;
	for (int y=0; y<16; y++) {
		for (int x=0; x<16; x++) {
			draw_tile(f, i, (x*8), (y*8), false, false, table, palette);
			i = i + 16;
		}
	}
	EndTextureMode();
}

Color ppu_fb[240][256];
void update_framebuffer(Famicom* f)
{
	for (int y=0; y<240; y++) {
		for (int x=0; x<256; x++)
			ppu_fb[y][x] = color_lookup(f->ppu.framebuffer[y][x]);
	}
	UpdateTexture(ppu_fb_tex.texture, ppu_fb);
}

static void shader_float(Shader s, const char* name, float value)
{
	SetShaderValue(s, GetShaderLocation(s, name), &value, SHADER_UNIFORM_FLOAT);
}

// a texture drawn into the current render target keeping its rows in the same order;
// render targets store rows the other way up, the negative source height undoes that
static void copy_into(Texture2D source, int width, int height)
{
	DrawTexturePro(source, (Rectangle){ 0, 0, source.width, -source.height },
		       (Rectangle){ 0, 0, width, height }, (Vector2){ 0, 0 }, 0, WHITE);
}

// the frame through blargg's ntsc filter, at the size it's shown at
void ntsc_render(Famicom* f)
{
	if (!ntsc_ready || !ntsc_enabled)
		return;
	int width = 256 * ppu_scale, height = 240 * ppu_scale;
	if (ntsc_target.texture.width != width || ntsc_target.texture.height != height) {
		if (ntsc_target.id)
			UnloadRenderTexture(ntsc_target);
		ntsc_target = LoadRenderTexture(width, height);
	}
	float size[2] = { NTSC_WIDTH, 240 };
	float frame_count = (float)(f->ppu.frame & 0xFFFF);
	float dot_crawl = ntsc.dot_crawl ? 1.0f : 0.0f;

	// stretch to 640 wide, pixels kept sharp, as the libretro preset does
	BeginTextureMode(ntsc_wide);
	copy_into(ppu_fb_tex.texture, NTSC_WIDTH, 240);
	EndTextureMode();

	// pass 0: encode to a composite signal
	SetShaderValue(ntsc_pass0, GetShaderLocation(ntsc_pass0, "TextureSize"), size, SHADER_UNIFORM_VEC2);
	shader_float(ntsc_pass0, "FrameCount", frame_count);
	shader_float(ntsc_pass0, "ntsc_bri", ntsc.brightness);
	shader_float(ntsc_pass0, "ntsc_hue", ntsc.hue);
	shader_float(ntsc_pass0, "stat_ph", dot_crawl);
	shader_float(ntsc_pass0, "pi_mod", ntsc.phase_angle);
	shader_float(ntsc_pass0, "vert_scal", ntsc.vertical_scale);
	BeginTextureMode(ntsc_signal_tex);
	BeginShaderMode(ntsc_pass0);
	copy_into(ntsc_wide.texture, NTSC_WIDTH, 240);
	EndShaderMode();
	EndTextureMode();

	// pass 1: decode at display size
	SetShaderValue(ntsc_pass1, GetShaderLocation(ntsc_pass1, "TextureSize"), size, SHADER_UNIFORM_VEC2);
	shader_float(ntsc_pass1, "FrameCount", frame_count);
	shader_float(ntsc_pass1, "ntsc_sat", ntsc.saturation);
	shader_float(ntsc_pass1, "ntsc_bleed", ntsc.bleed);
	shader_float(ntsc_pass1, "ntsc_res", ntsc.resolution);
	shader_float(ntsc_pass1, "ntsc_sharp", ntsc.sharpness);
	shader_float(ntsc_pass1, "LUMA_CUTOFF", ntsc.luma_cutoff);
	shader_float(ntsc_pass1, "stat_ph", dot_crawl);
	shader_float(ntsc_pass1, "fring", ntsc.fringing);
	shader_float(ntsc_pass1, "afacts", ntsc.artifacts);
	shader_float(ntsc_pass1, "kernel_half", ntsc.kernel_half);
	shader_float(ntsc_pass1, "pi_mod", ntsc.phase_angle);
	shader_float(ntsc_pass1, "vert_scal", ntsc.vertical_scale);
	BeginTextureMode(ntsc_target);
	ClearBackground(BLACK);
	BeginShaderMode(ntsc_pass1);
	copy_into(ntsc_signal_tex.texture, width, height);
	EndShaderMode();
	EndTextureMode();
}

// rom picker
#define PICKER_MAX_ENTRIES 4096
typedef struct picker_entry {
	char* path;
	bool dir;
} Picker_entry;

static const char* picker_extensions(enum picker_mode mode)
{
	switch (mode) {
	case picker_palette: return ".pal";
	case picker_bios: return ".rom;.bin";
	case picker_wave: return ".wav";
	case picker_ffmpeg: return ".exe";
	default: return ".nes;.nsf;.nsfe;.fds;.zip";
	}
}

static void config_save();

bool picker_open = false;
enum picker_mode picker_mode = picker_rom;
char picker_dir[1024];
char picker_edit[1024];
char picker_status[128];
FilePathList picker_files;
bool picker_files_loaded = false;
Picker_entry picker_entries[PICKER_MAX_ENTRIES];
int picker_count = 0;
int picker_selected = -1;

static int picker_compare(const void* a, const void* b)
{
	const Picker_entry* ea = a;
	const Picker_entry* eb = b;
	if (ea->dir != eb->dir)
		return ea->dir ? -1 : 1;
	const char* na = GetFileName(ea->path);
	const char* nb = GetFileName(eb->path);
	for (; *na && *nb; na++, nb++) {
		int ca = (*na >= 'A' && *na <= 'Z') ? *na + 32 : *na;
		int cb = (*nb >= 'A' && *nb <= 'Z') ? *nb + 32 : *nb;
		if (ca != cb)
			return ca - cb;
	}
	return (unsigned char)*na - (unsigned char)*nb;
}

static void picker_refresh(const char* dir)
{
	if (!DirectoryExists(dir)) {
		snprintf(picker_status, sizeof(picker_status), "not a folder");
		return;
	}
	// copy first, dir may point into the list that's about to be freed
	snprintf(picker_dir, sizeof(picker_dir), "%s", dir);
	snprintf(picker_edit, sizeof(picker_edit), "%s", picker_dir);
	ui_need_text(picker_dir);
	if (picker_files_loaded)
		UnloadDirectoryFiles(picker_files);
	picker_files = LoadDirectoryFiles(picker_dir);
	picker_files_loaded = true;
	picker_count = 0;
	for (unsigned int i=0; i<picker_files.count && picker_count < PICKER_MAX_ENTRIES; i++) {
		char* path = picker_files.paths[i];
		bool dir = !IsPathFile(path);
		if (dir || IsFileExtension(path, picker_extensions(picker_mode))) {
			ui_need_text(GetFileName(path));
			picker_entries[picker_count].path = path;
			picker_entries[picker_count].dir = dir;
			picker_count++;
		}
	}
	qsort(picker_entries, picker_count, sizeof(Picker_entry), picker_compare);
	picker_selected = -1;
	picker_status[0] = '\0';
}

static void picker_show(Famicom* famicom, enum picker_mode mode)
{
	const char* start;
	picker_mode = mode;
	if (mode == picker_rom && famicom_has_cartridge(famicom))
		start = GetDirectoryPath(famicom->rom_path);
	else if (picker_dir[0])
		start = picker_dir;
	else
		start = GetWorkingDirectory();
	picker_refresh(start);
	if (!DirectoryExists(start))
		picker_refresh(GetWorkingDirectory());
	picker_open = true;
}

static void picker_load(Famicom* famicom, const char* path)
{
	if (picker_mode == picker_palette) {
		if (palette_use_file(path) == 0) {
			config_dirty = true;
			picker_open = false;
		}
		else
			snprintf(picker_status, sizeof(picker_status), "not a palette file");
		return;
	}
	if (picker_mode == picker_ffmpeg) {
		snprintf(ffmpeg_path, sizeof(ffmpeg_path), "%s", path);
		config_dirty = true;
		picker_open = false;
		return;
	}
	if (picker_mode == picker_wave) {
		if (wave_load_file(picker_wave_channel, path) == 0) {
			audio_fx.waveform[picker_wave_channel] = wave_sample;
			config_dirty = true;
			picker_open = false;
		} else {
			snprintf(picker_status, sizeof(picker_status), "couldn't read that .wav");
		}
		return;
	}
	if (picker_mode == picker_bios) {
		if (GetFileLength(path) == 0x2000) {
			snprintf(fds_bios_path, sizeof(fds_bios_path), "%s", path);
			config_save();
			picker_open = false;
		} else {
			snprintf(picker_status, sizeof(picker_status), "the bios is an 8KB file");
		}
		return;
	}
	if (famicom_insert_cartridge(famicom, path) == 0) {
		picker_open = false;
	} else if (IsFileExtension(path, ".fds")) {
		snprintf(picker_status, sizeof(picker_status), "needs disksys.rom, see Settings");
	} else {
		snprintf(picker_status, sizeof(picker_status), "couldn't load that rom");
	}
}

static void picker_close()
{
	picker_open = false;
	if (picker_files_loaded) {
		UnloadDirectoryFiles(picker_files);
		picker_files_loaded = false;
	}
	picker_count = 0;
}

void picker_window(Famicom* famicom)
{
	if (!picker_open)
		return;
	char navigate[1024];
	navigate[0] = '\0';
	int open = -1;
	const char* title = picker_mode == picker_palette ? "Open Palette"
		: picker_mode == picker_bios ? "Choose FDS BIOS"
		: picker_mode == picker_wave ? "Choose a waveform .wav"
		: picker_mode == picker_ffmpeg ? "Choose ffmpeg.exe" : "Open ROM";
	if (nk_begin_titled(ctx, "Picker", title, nk_rect(250, 40, 460, 480), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_CLOSABLE|NK_WINDOW_TITLE)) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){0.15f, 0.85f});
		if (nk_button_label(ctx, "Up"))
			snprintf(navigate, sizeof(navigate), "%s", GetPrevDirectoryPath(picker_dir));
		nk_flags edit = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD|NK_EDIT_SIG_ENTER, picker_edit, sizeof(picker_edit), nk_filter_default);
		if (edit & NK_EDIT_COMMITTED)
			snprintf(navigate, sizeof(navigate), "%s", picker_edit);

		float list_height = nk_window_get_content_region(ctx).h - 2 * UI_BUTTON - 20;
		nk_layout_row_dynamic(ctx, list_height < 40 ? 40 : list_height, 1);
		if (nk_group_begin(ctx, "files", NK_WINDOW_BORDER)) {
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			for (int i=0; i<picker_count; i++) {
				Picker_entry* e = &picker_entries[i];
				nk_bool selected = (i == picker_selected);
				const char* label = e->dir ? TextFormat("[%s]", GetFileName(e->path)) : GetFileName(e->path);
				if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected)) {
					if (e->dir)
						snprintf(navigate, sizeof(navigate), "%s", e->path);
					else if (i == picker_selected)
						open = i; // clicking the selected rom again opens it
					else
						picker_selected = i;
				}
			}
			nk_group_end(ctx);
		}

		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 3, (float[]){0.5f, 0.25f, 0.25f});
		nk_label(ctx, picker_status, NK_TEXT_LEFT);
		if (nk_button_label(ctx, "Open") && 0 <= picker_selected)
			open = picker_selected;
		if (nk_button_label(ctx, "Cancel"))
			picker_open = false;
	} else {
		picker_open = false;
	}
	nk_end(ctx);

	if (0 <= open)
		picker_load(famicom, picker_entries[open].path);
	else if (navigate[0])
		picker_refresh(navigate);
	if (!picker_open)
		picker_close();
}

// apu register view, cells glow hotter the more often they're written
byte register_values[APU_REGISTER_COUNT];
float register_heat[APU_REGISTER_COUNT];

static void update_register_heat()
{
	int writes[APU_REGISTER_COUNT];
	audio_take_register_writes(register_values, writes);
	for (int i=0; i<APU_REGISTER_COUNT; i++)
		register_heat[i] = register_heat[i] * 0.92f + writes[i];
}

static struct nk_color heat_color(float heat)
{
	// cold grey -> red -> yellow
	float t = 1.0f - expf(-heat / 6.0f);
	struct nk_color cold = nk_rgb(45, 45, 52);
	struct nk_color warm = nk_rgb(200, 45, 30);
	struct nk_color hot = nk_rgb(255, 225, 90);
	struct nk_color a = t < 0.5f ? cold : warm;
	struct nk_color b = t < 0.5f ? warm : hot;
	float k = t < 0.5f ? t * 2 : (t - 0.5f) * 2;
	return nk_rgb(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k);
}

static void register_cell(int index)
{
	struct nk_rect bounds;
	if (!nk_widget(&bounds, ctx))
		return;
	struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
	float heat = register_heat[index];
	struct nk_color bg = heat_color(heat);
	struct nk_color fg = (1.0f - expf(-heat / 6.0f)) < 0.75f ? nk_rgb(230, 230, 230) : nk_rgb(20, 20, 20);
	nk_fill_rect(canvas, bounds, 3, bg);
	const char* text = TextFormat("$%04X %02X", 0x4000 + index, register_values[index]);
	const struct nk_user_font* font = ctx->style.font;
	int len = strlen(text);
	float width = font->width(font->userdata, font->height, text, len);
	struct nk_rect r = nk_rect(bounds.x + (bounds.w - width) / 2, bounds.y + (bounds.h - font->height) / 2, width, font->height);
	nk_draw_text(canvas, r, text, len, font, bg, fg);
}

static void register_row(const char* name, int first, int count)
{
	nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 5, (float[]){0.16f, 0.21f, 0.21f, 0.21f, 0.21f});
	nk_label(ctx, name, NK_TEXT_LEFT);
	for (int i=0; i<4; i++) {
		if (i < count)
			register_cell(first + i);
		else
			nk_spacing(ctx, 1);
	}
}

// piano roll: a keyboard on the left, each voice's pitch scrolling off to the right
#define PIANO_HISTORY 2048
#define PIANO_KEYBOARD 48

typedef struct piano_point {
	float pitch; // midi note, fractional so slides and vibrato bend the trail
	float volume; // 0 when silent
	byte channel;
} Piano_point;

Piano_point piano_history[PIANO_HISTORY][AUDIO_MAX_VOICES];
int piano_head = 0;
nk_bool piano_open = false;
RenderTexture2D piano_tex;
int piano_width = 0, piano_height = 0;
int piano_low_octave = 1;
int piano_octaves = 7;
int piano_speed = 2; // pixels per frame

// one column of history per emulated frame, so trails stop while paused
void piano_sample(Famicom* f)
{
	Audio_voice voices[AUDIO_MAX_VOICES];
	audio_voices(f, voices);
	piano_head = (piano_head + 1) % PIANO_HISTORY;
	for (int v=0; v<AUDIO_MAX_VOICES; v++) {
		Piano_point* p = &piano_history[piano_head][v];
		p->channel = voices[v].channel;
		if (0 < voices[v].volume && 0 < voices[v].frequency) {
			p->pitch = 69.0f + 12.0f * log2f(voices[v].frequency / 440.0f);
			p->volume = voices[v].volume;
		} else {
			p->volume = 0;
		}
	}
}

static void piano_resize(int width, int height)
{
	if (width < 96) width = 96;
	if (2048 < width) width = 2048;
	if (height < 48) height = 48;
	if (1200 < height) height = 1200;
	if (width == piano_width && height == piano_height && piano_tex.id)
		return;
	if (piano_tex.id)
		UnloadRenderTexture(piano_tex);
	piano_tex = LoadRenderTexture(width, height);
	piano_width = width;
	piano_height = height;
}

static bool black_key(int note)
{
	int n = note % 12;
	return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

static Color piano_color(int channel, float volume)
{
	Color c = audio_mute[channel] ? (Color){ 90, 90, 90, 255 } : channel_colors[channel];
	c.a = 90 + 165 * volume;
	return c;
}

static void piano_render(RenderTexture2D target, int w, int h);

void piano_draw()
{
	if (!piano_open || !piano_tex.id)
		return;
	piano_render(piano_tex, piano_width, piano_height);
}

// drawn with low notes at small y, render textures come out upside down in nuklear
static void piano_render(RenderTexture2D target, int w, int h)
{
	int low = 12 * (piano_low_octave + 1);
	int keys = piano_octaves * 12;
	float key = (float)h / keys;
	int keyboard = w < 200 ? w / 4 : PIANO_KEYBOARD;

	BeginTextureMode(target);
	ClearBackground(BLACK);
	// lanes: a faint shade for black keys, a line under every C
	for (int k=0; k<keys; k++) {
		int note = low + k;
		if (black_key(note))
			DrawRectangle(keyboard, k * key, w - keyboard, key + 1, (Color){ 8, 8, 10, 255 });
		if (note % 12 == 0)
			DrawLine(keyboard, k * key, w, k * key, (Color){ 30, 30, 36, 255 });
	}

	// trails, newest at the keyboard; small pitch changes join up so slides and
	// vibrato bend the trail, bigger jumps start a new note
	int columns = (w - keyboard) / piano_speed + 2;
	if (PIANO_HISTORY - 1 < columns)
		columns = PIANO_HISTORY - 1;
	for (int v=0; v<AUDIO_MAX_VOICES; v++) {
		for (int i=0; i<columns; i++) {
			Piano_point* a = &piano_history[(piano_head - i + PIANO_HISTORY) % PIANO_HISTORY][v];
			if (a->volume <= 0)
				continue;
			Piano_point* b = &piano_history[(piano_head - i - 1 + PIANO_HISTORY) % PIANO_HISTORY][v];
			float x0 = keyboard + i * piano_speed;
			float x1 = x0 + piano_speed;
			float y0 = (a->pitch - low + 0.5f) * key;
			float y1 = (0 < b->volume && fabsf(b->pitch - a->pitch) < 1.5f) ? (b->pitch - low + 0.5f) * key : y0;
			float thick = key * (0.35f + 0.65f * a->volume);
			if (thick < 1.5f)
				thick = 1.5f;
			DrawLineEx((Vector2){ x0, y0 }, (Vector2){ x1, y1 }, thick, piano_color(a->channel, a->volume));
		}
	}

	// keyboard, black keys stop short like a real one
	for (int k=0; k<keys; k++) {
		int note = low + k;
		float y = k * key;
		if (black_key(note)) {
			DrawRectangle(0, y, keyboard, key + 1, (Color){ 210, 210, 216, 255 });
			DrawRectangle(0, y, keyboard * 0.62f, key + 1, (Color){ 30, 30, 34, 255 });
		} else {
			DrawRectangle(0, y, keyboard, key + 1, (Color){ 222, 222, 228, 255 });
			// white keys meet with no black key between at B-C and E-F
			if (note % 12 == 0 || note % 12 == 5)
				DrawLine(0, y, keyboard, y, (Color){ 120, 120, 130, 255 });
		}
	}
	// keys being played light up in their channel's colour
	for (int v=0; v<AUDIO_MAX_VOICES; v++) {
		Piano_point* p = &piano_history[piano_head][v];
		if (p->volume <= 0)
			continue;
		int note = (int)floorf(p->pitch + 0.5f);
		if (note < low || low + keys <= note)
			continue;
		float y = (note - low) * key;
		Color c = piano_color(p->channel, 1.0f);
		DrawRectangle(0, y, black_key(note) ? keyboard * 0.62f : keyboard, key + 1, c);
	}
	DrawLine(keyboard, 0, keyboard, h, (Color){ 70, 70, 80, 255 });
	EndTextureMode();
}

// video export: the scopes and the piano roll drawn offscreen each emulated frame,
// piped to ffmpeg as raw frames, with the sound recorded beside it and muxed at the end
typedef struct export_size {
	const char* name;
	int width, height;
} Export_size;

const Export_size export_sizes[] = { { "1280x720", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "2560x1440", 2560, 1440 } };
#define EXPORT_SIZES (int)(sizeof(export_sizes) / sizeof(export_sizes[0]))
int export_size = 1;
char ffmpeg_path[1024] = ""; // empty: next to the exe, then whatever's on the PATH
bool exporting = false;
FILE* export_pipe = NULL;
RenderTexture2D export_tex, export_piano;
Font export_font;
bool export_font_loaded = false;
Font export_small_font; // the register log's, built at its own size so it stays sharp
bool export_small_font_loaded = false;
#define EXPORT_REGISTER_LINES 1024
static Audio_write export_reg_lines[EXPORT_REGISTER_LINES]; // the register log's history
static long export_reg_count = 0;
long export_frames = 0;
double export_fps = 60;
double export_speed = 0; // how many times faster than real time the render runs
// stopping a render fades the sound out over a few seconds of video first
#define EXPORT_FADE_SECONDS 3.0
bool export_fading = false;
long export_fade_start = 0; // the frame the fade began on
char export_out[1200], export_video[1200], export_audio[1200];
char export_status[300];

// frames go to ffmpeg from a thread of their own, so encoding never holds up emulation;
// a few frames can wait in line before the render has to slow down for ffmpeg
#define EXPORT_QUEUE 6
static void* export_queue[EXPORT_QUEUE];
static int export_queue_head, export_queue_count;
static bool export_writer_quit, export_writer_failed;
static size_t export_frame_bytes;
static pthread_t export_writer;
static pthread_mutex_t export_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t export_ready = PTHREAD_COND_INITIALIZER, export_space = PTHREAD_COND_INITIALIZER;
static bool export_frame_waiting = false; // the target holds a frame not read back yet

static void* export_writer_run(void* unused)
{
	(void)unused;
	pthread_mutex_lock(&export_lock);
	for (;;) {
		while (export_queue_count == 0 && !export_writer_quit)
			pthread_cond_wait(&export_ready, &export_lock);
		if (export_queue_count == 0)
			break;
		void* frame = export_queue[export_queue_head];
		pthread_mutex_unlock(&export_lock);
		bool ok = export_writer_failed ? false : fwrite(frame, 1, export_frame_bytes, export_pipe) == export_frame_bytes;
		MemFree(frame);
		pthread_mutex_lock(&export_lock);
		if (!ok)
			export_writer_failed = true;
		export_queue_head = (export_queue_head + 1) % EXPORT_QUEUE;
		export_queue_count--;
		pthread_cond_signal(&export_space);
	}
	pthread_mutex_unlock(&export_lock);
	return NULL;
}

// a frame as rgba is 4 bytes a pixel; turned into planar yuv 4:2:0 on the gpu first it's
// 1.5, which is what x264 wants anyway, so reading back and piping costs far less.
// the target is a quarter as wide, each rgba texel carrying four bytes of the planes
// in order: the luma rows, then u, then v, top row first once read back
RenderTexture2D export_yuv, export_readback;
bool export_use_yuv = false;
static Shader export_yuv_shader;
static int export_yuv_size_loc = -1;
static int export_yuv_shader_state = 0; // 0 not tried, 1 ready, -1 failed

static const char* export_yuv_source =
	"#version 330\n"
	"uniform sampler2D texture0;\n"
	"uniform vec2 size;\n"
	"out vec4 finalColor;\n"
	// the picture at a point in pixels, top left origin; render targets are bottom up
	"vec3 picture(vec2 p) { return texture(texture0, vec2(p.x / size.x, 1.0 - p.y / size.y)).rgb; }\n"
	// bt.601 limited range, what ffmpeg made from rgba before
	"float plane_byte(float i) {\n"
	"	float luma_bytes = size.x * size.y;\n"
	"	if (i < luma_bytes) {\n"
	"		float y = floor(i / size.x);\n"
	"		vec3 c = picture(vec2(i - y * size.x + 0.5, y + 0.5));\n"
	"		return 16.0 / 255.0 + dot(c, vec3(0.2568, 0.5041, 0.0979));\n"
	"	}\n"
	"	i -= luma_bytes;\n"
	"	float chroma_w = size.x * 0.5;\n"
	"	float chroma_bytes = chroma_w * size.y * 0.5;\n"
	"	bool v = chroma_bytes <= i;\n"
	"	if (v) i -= chroma_bytes;\n"
	"	float cy = floor(i / chroma_w);\n"
	// sampled between four pixels, the filtering averages them
	"	vec3 c = picture(vec2(2.0 * (i - cy * chroma_w) + 1.0, 2.0 * cy + 1.0));\n"
	"	return 128.0 / 255.0 + (v ? dot(c, vec3(0.4392, -0.3678, -0.0714)) : dot(c, vec3(-0.1482, -0.2910, 0.4392)));\n"
	"}\n"
	"void main() {\n"
	"	float base = floor(gl_FragCoord.y) * size.x + floor(gl_FragCoord.x) * 4.0;\n"
	"	finalColor = vec4(plane_byte(base), plane_byte(base + 1.0), plane_byte(base + 2.0), plane_byte(base + 3.0));\n"
	"}\n";

static bool export_yuv_shader_ready(void)
{
	if (export_yuv_shader_state == 0) {
		export_yuv_shader = LoadShaderFromMemory(NULL, export_yuv_source);
		export_yuv_shader_state = IsShaderValid(export_yuv_shader) ? 1 : -1;
		if (export_yuv_shader_state == 1)
			export_yuv_size_loc = GetShaderLocation(export_yuv_shader, "size");
	}
	return export_yuv_shader_state == 1;
}

static void export_convert_yuv(int w, int h)
{
	float size[2] = { (float)w, (float)h };
	BeginTextureMode(export_yuv);
	BeginShaderMode(export_yuv_shader);
	SetShaderValue(export_yuv_shader, export_yuv_size_loc, size, SHADER_UNIFORM_VEC2);
	// alpha is a data byte here, blending would mix it into the others
	rlDisableColorBlend();
	DrawTexturePro(export_tex.texture, (Rectangle){ 0, 0, w, h },
		       (Rectangle){ 0, 0, export_yuv.texture.width, export_yuv.texture.height }, (Vector2){ 0, 0 }, 0, WHITE);
	rlDrawRenderBatchActive();
	rlEnableColorBlend();
	EndShaderMode();
	EndTextureMode();
}

// hands a frame (MemAlloc'd, the writer frees it) to the writer, waiting for room
static void export_queue_push(void* frame)
{
	pthread_mutex_lock(&export_lock);
	while (export_queue_count == EXPORT_QUEUE && !export_writer_failed)
		pthread_cond_wait(&export_space, &export_lock);
	if (export_writer_failed) {
		MemFree(frame);
	} else {
		export_queue[(export_queue_head + export_queue_count) % EXPORT_QUEUE] = frame;
		export_queue_count++;
		pthread_cond_signal(&export_ready);
	}
	pthread_mutex_unlock(&export_lock);
}

// reading pixels straight back stalls until the gpu has drawn them and copied them
// over, several milliseconds a frame; pixel pack buffers let the copy happen in the
// background, each frame is started into one when drawn and picked up a couple of
// frames later. raylib doesn't wrap them, so the calls come from the driver directly
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_RGBA_FORMAT 0x1908
#define GL_UNSIGNED_BYTE_TYPE 0x1401
#define GL_PACK_ALIGNMENT_PARAM 0x0D05
#define EXPORT_PBOS 3
#ifdef _WIN32
__declspec(dllimport) void* __stdcall wglGetProcAddress(const char* name);
__declspec(dllimport) void __stdcall glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void* pixels);
__declspec(dllimport) void __stdcall glPixelStorei(unsigned name, int value);
typedef void (__stdcall* Gl_gen_buffers)(int count, unsigned* buffers);
typedef void (__stdcall* Gl_bind_buffer)(unsigned target, unsigned buffer);
typedef void (__stdcall* Gl_buffer_data)(unsigned target, ptrdiff_t size, const void* data, unsigned usage);
typedef void* (__stdcall* Gl_map_buffer)(unsigned target, unsigned access);
typedef unsigned char (__stdcall* Gl_unmap_buffer)(unsigned target);
static Gl_gen_buffers gl_gen_buffers, gl_delete_buffers_fn;
static Gl_bind_buffer gl_bind_buffer;
static Gl_buffer_data gl_buffer_data;
static Gl_map_buffer gl_map_buffer;
static Gl_unmap_buffer gl_unmap_buffer;
#endif
static unsigned export_pbo[EXPORT_PBOS];
static bool export_pbo_ready = false;
static long export_pbo_started, export_pbo_taken;

static void export_pbo_init(void)
{
	export_pbo_ready = false;
	export_pbo_started = export_pbo_taken = 0;
#ifdef _WIN32
	gl_gen_buffers = (Gl_gen_buffers)wglGetProcAddress("glGenBuffers");
	gl_delete_buffers_fn = (Gl_gen_buffers)wglGetProcAddress("glDeleteBuffers"); // same shape
	gl_bind_buffer = (Gl_bind_buffer)wglGetProcAddress("glBindBuffer");
	gl_buffer_data = (Gl_buffer_data)wglGetProcAddress("glBufferData");
	gl_map_buffer = (Gl_map_buffer)wglGetProcAddress("glMapBuffer");
	gl_unmap_buffer = (Gl_unmap_buffer)wglGetProcAddress("glUnmapBuffer");
	if (!gl_gen_buffers || !gl_delete_buffers_fn || !gl_bind_buffer || !gl_buffer_data || !gl_map_buffer || !gl_unmap_buffer)
		return;
	gl_gen_buffers(EXPORT_PBOS, export_pbo);
	for (int i=0; i<EXPORT_PBOS; i++) {
		gl_bind_buffer(GL_PIXEL_PACK_BUFFER, export_pbo[i]);
		gl_buffer_data(GL_PIXEL_PACK_BUFFER, (ptrdiff_t)export_frame_bytes, NULL, GL_STREAM_READ);
	}
	gl_bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	export_pbo_ready = export_pbo[0] != 0;
#endif
}

static void export_pbo_free(void)
{
#ifdef _WIN32
	if (export_pbo_ready)
		gl_delete_buffers_fn(EXPORT_PBOS, export_pbo);
#endif
	export_pbo_ready = false;
}

// the oldest frame in flight, off to the writer
static void export_pbo_collect(void)
{
#ifdef _WIN32
	gl_bind_buffer(GL_PIXEL_PACK_BUFFER, export_pbo[export_pbo_taken % EXPORT_PBOS]);
	void* pixels = gl_map_buffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	void* frame = pixels ? MemAlloc((unsigned int)export_frame_bytes) : NULL;
	if (frame)
		memcpy(frame, pixels, export_frame_bytes);
	if (pixels)
		gl_unmap_buffer(GL_PIXEL_PACK_BUFFER);
	gl_bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	export_pbo_taken++;
	if (frame)
		export_queue_push(frame);
	else
		export_writer_failed = true;
#endif
}

// starts copying the frame just drawn, collecting older ones once enough are in flight
static void export_pbo_start(int w, int h)
{
#ifdef _WIN32
	if (export_pbo_started - export_pbo_taken == EXPORT_PBOS)
		export_pbo_collect();
	rlEnableFramebuffer(export_readback.id);
	gl_bind_buffer(GL_PIXEL_PACK_BUFFER, export_pbo[export_pbo_started % EXPORT_PBOS]);
	glPixelStorei(GL_PACK_ALIGNMENT_PARAM, 1);
	glReadPixels(0, 0, w, h, GL_RGBA_FORMAT, GL_UNSIGNED_BYTE_TYPE, NULL);
	gl_bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
	rlDisableFramebuffer();
	export_pbo_started++;
	// one copy can still be running while the next frame draws; older ones are done
	while (export_pbo_started - export_pbo_taken > EXPORT_PBOS - 1)
		export_pbo_collect();
#else
	(void)w; (void)h;
#endif
}

// reads the frame drawn last time off the gpu and hands it to the writer; without
// pixel pack buffers it's read the slow way, after the emulation gave the gpu time
static void export_send_frame(void)
{
	if (export_pbo_ready) {
		// the stop: whatever's still in flight
		if (!exporting)
			while (export_pbo_taken < export_pbo_started)
				export_pbo_collect();
		return;
	}
	if (!export_frame_waiting)
		return;
	export_frame_waiting = false;
	Image image = LoadImageFromTexture(export_readback.texture);
	if (image.data == NULL) {
		export_writer_failed = true;
		return;
	}
	export_queue_push(image.data);
}

// the ffmpeg to run, quoted
static const char* ffmpeg_command(void)
{
	static char command[1100];
	const char* beside = TextFormat("%sffmpeg.exe", GetApplicationDirectory());
	if (ffmpeg_path[0])
		snprintf(command, sizeof(command), "\"%s\"", ffmpeg_path);
	else if (FileExists(beside))
		snprintf(command, sizeof(command), "\"%s\"", beside);
	else
		snprintf(command, sizeof(command), "ffmpeg");
	return command;
}

// cmd strips one pair of quotes off the whole line, so every command is wrapped once more
static int run_command(const char* line)
{
	char* wrapped = malloc(strlen(line) + 3);
	if (wrapped == NULL)
		return -1;
	sprintf(wrapped, "\"%s\"", line);
	int result = system(wrapped);
	free(wrapped);
	return result;
}

static bool ffmpeg_available(void)
{
	char line[1300];
	snprintf(line, sizeof(line), "%s -version >nul 2>&1", ffmpeg_command());
	return run_command(line) == 0;
}

static void export_unload_fonts(void)
{
	if (export_font_loaded)
		UnloadFont(export_font);
	if (export_small_font_loaded)
		UnloadFont(export_small_font);
	export_font_loaded = export_small_font_loaded = false;
}

static void export_start(Famicom* famicom)
{
	if (exporting)
		return;
	if (!ffmpeg_available()) {
		snprintf(export_status, sizeof(export_status), "FFmpeg not found, set it in Settings or put ffmpeg.exe next to nemu");
		return;
	}
	const Export_size* size = &export_sizes[export_size];
	char folder[1100];
	snprintf(folder, sizeof(folder), "%sexports", GetApplicationDirectory());
	if (!DirectoryExists(folder))
		MakeDirectory(folder);
	char stamp[32];
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
	const char* base = famicom_has_cartridge(famicom) ? GetFileNameWithoutExt(famicom->rom_path) : "nemu";
	snprintf(export_out, sizeof(export_out), "%s/%s %s.mp4", folder, base, stamp);
	snprintf(export_video, sizeof(export_video), "%s/%s %s.video.mp4", folder, base, stamp);
	snprintf(export_audio, sizeof(export_audio), "%s/%s %s.audio.wav", folder, base, stamp);
	export_fps = famicom_frame_rate(famicom);

	// frames go over as yuv made on the gpu when the shader's there, else as rgba;
	// render targets store rows bottom up, the shader or vflip puts them right way round
	export_use_yuv = export_yuv_shader_ready();
	char line[2600];
	if (export_use_yuv)
		snprintf(line, sizeof(line), "%s -y -loglevel error -f rawvideo -pix_fmt yuv420p -s %dx%d -framerate %.4f -i - "
			 "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"%s\"",
			 ffmpeg_command(), size->width, size->height, export_fps, export_video);
	else
		snprintf(line, sizeof(line), "%s -y -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -framerate %.4f -i - "
			 "-vf vflip -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"%s\"",
			 ffmpeg_command(), size->width, size->height, export_fps, export_video);
	char* wrapped = malloc(strlen(line) + 3);
	sprintf(wrapped, "\"%s\"", line);
	export_pipe = popen(wrapped, "wb");
	free(wrapped);
	if (export_pipe == NULL) {
		snprintf(export_status, sizeof(export_status), "couldn't start FFmpeg");
		return;
	}
	if (!audio_record_start(export_audio)) {
		pclose(export_pipe);
		export_pipe = NULL;
		snprintf(export_status, sizeof(export_status), "couldn't write the audio file");
		return;
	}
	export_tex = LoadRenderTexture(size->width, size->height);
	export_piano = LoadRenderTexture(size->width, size->height - (int)(size->height * 0.72f));
	if (ui_font_data) {
		export_font = ui_build_font(size->height / 32);
		export_font_loaded = IsFontValid(export_font);
		export_small_font = ui_build_font((int)(size->height / 32.0f * 0.62f));
		export_small_font_loaded = IsFontValid(export_small_font);
	}
	// the register log starts empty, with nothing from before the render
	export_reg_count = 0;
	{
		static Audio_write discard[1];
		audio_take_writes(discard, 1);
	}
	if (export_use_yuv) {
		// the 2x2 chroma average comes from sampling between pixels
		SetTextureFilter(export_tex.texture, TEXTURE_FILTER_BILINEAR);
		export_yuv = LoadRenderTexture(size->width / 4, size->height * 3 / 2);
		export_readback = export_yuv;
		export_frame_bytes = (size_t)size->width * size->height * 3 / 2;
	} else {
		export_readback = export_tex;
		export_frame_bytes = (size_t)size->width * size->height * 4;
	}
	export_queue_head = export_queue_count = 0;
	export_writer_quit = export_writer_failed = false;
	export_frame_waiting = false;
	export_pbo_init();
	if (pthread_create(&export_writer, NULL, export_writer_run, NULL) != 0) {
		export_pbo_free();
		if (export_use_yuv)
			UnloadRenderTexture(export_yuv);
		UnloadRenderTexture(export_tex);
		UnloadRenderTexture(export_piano);
		export_unload_fonts();
		pclose(export_pipe);
		export_pipe = NULL;
		audio_record_stop(AUDIO_SAMPLE_RATE);
		remove(export_audio);
		snprintf(export_status, sizeof(export_status), "couldn't start the frame writer");
		return;
	}
	export_frames = 0;
	export_fading = false;
	exporting = true;
	// the render starts the song over with clean effect tails, and stays off the speakers
	// so it can run as fast as the machine allows
	if (famicom->nsf.active)
		famicom_nsf_select(famicom, famicom->nsf.song);
	audio_clear_effects();
	audio_output_muted = true;
	// and with an empty piano roll, no trails left over from before
	memset(piano_history, 0, sizeof(piano_history));
	export_speed = 0;
	snprintf(export_status, sizeof(export_status), "rendering...");
}

static void export_stop(void)
{
	if (!exporting)
		return;
	exporting = false;
	export_fading = false;
	audio_output_muted = false;
	audio_volume = 1.0f;
	// the last frame drawn, then whatever's still in line, before ffmpeg is told it's over
	export_send_frame();
	pthread_mutex_lock(&export_lock);
	export_writer_quit = true;
	pthread_cond_signal(&export_ready);
	pthread_mutex_unlock(&export_lock);
	pthread_join(export_writer, NULL);
	export_pbo_free();
	pclose(export_pipe);
	export_pipe = NULL;
	// the audio's rate is whatever was really made per second of emulated time, so
	// ffmpeg's resampling lines it up exactly with the frames
	unsigned long samples = audio_record_frames();
	double seconds = export_frames / export_fps;
	int rate = seconds > 0 ? (int)(samples / seconds + 0.5) : AUDIO_SAMPLE_RATE;
	if (rate < 8000 || 192000 < rate)
		rate = AUDIO_SAMPLE_RATE;
	audio_record_stop(rate);
	if (export_use_yuv)
		UnloadRenderTexture(export_yuv);
	UnloadRenderTexture(export_tex);
	UnloadRenderTexture(export_piano);
	export_unload_fonts();

	if (export_frames == 0) {
		remove(export_video);
		remove(export_audio);
		snprintf(export_status, sizeof(export_status), "nothing was recorded, the game wasn't running");
		return;
	}
	char line[4000];
	snprintf(line, sizeof(line), "%s -y -loglevel error -i \"%s\" -i \"%s\" -c:v copy -c:a aac -b:a 192k -ar 48000 -shortest \"%s\"",
		 ffmpeg_command(), export_video, export_audio, export_out);
	if (run_command(line) == 0 && FileExists(export_out)) {
		remove(export_video);
		remove(export_audio);
		snprintf(export_status, sizeof(export_status), "saved %s (%ld frames)", GetFileName(export_out), export_frames);
	} else {
		snprintf(export_status, sizeof(export_status), "muxing failed, the video and audio were left in exports");
	}
	ui_need_text(export_status);
}

// one channel's trace in screen style coordinates, triggered and centred like the scopes
static void export_trace(int channel, Rectangle r, float thickness)
{
	static float samples[2048];
	int count = r.width < 1024 ? (int)r.width : 1024;
	if (count < 2)
		return;
	audio_scope_channel(channel, samples, count * 2);
	float low = 1, high = 0;
	for (int i=0; i<count * 2; i++) {
		if (samples[i] < low) low = samples[i];
		if (high < samples[i]) high = samples[i];
	}
	float mid = (low + high) / 2;
	int start = 0;
	for (int i=1; i<count; i++) {
		if (samples[i - 1] < mid && mid <= samples[i]) {
			start = i;
			break;
		}
	}
	Color color = audio_mute[channel] ? (Color){ 90, 90, 90, 255 } : channel_colors[channel];
	float centre = r.y + r.height / 2;
	DrawLine(r.x, centre, r.x + r.width, centre, (Color){ 22, 22, 26, 255 });
	float step = r.width / (count - 1);
	Vector2 previous = { r.x, centre - (samples[start] - mid) * r.height * 0.8f };
	for (int i=1; i<count; i++) {
		Vector2 point = { r.x + i * step, centre - (samples[start + i] - mid) * r.height * 0.8f };
		DrawLineEx(previous, point, thickness, color);
		previous = point;
	}
}

// the register log: every sound register write as it happens, newest at the bottom,
// scrolling up and dimming as it ages
static const char* export_chip_names[chip_count] = { "APU", "FDS", "N163", "VRC6", "VRC7" };

static void export_take_register_writes(void)
{
	static Audio_write fresh[EXPORT_REGISTER_LINES];
	int n = audio_take_writes(fresh, EXPORT_REGISTER_LINES);
	for (int i=0; i<n; i++)
		export_reg_lines[export_reg_count++ % EXPORT_REGISTER_LINES] = fresh[i];
}

static void export_draw_registers(Rectangle r, float text, float pad)
{
	Font font = export_small_font_loaded ? export_small_font : export_font_loaded ? export_font : GetFontDefault();
	DrawRectangleLinesEx(r, 1, (Color){ 28, 28, 32, 255 });
	DrawTextEx(export_font_loaded ? export_font : GetFontDefault(), "Registers", (Vector2){ r.x + pad, r.y + pad * 0.5f },
		   text * 0.8f, 1, (Color){ 200, 200, 210, 255 });
	float size = text * 0.62f;
	float line = size * 1.15f;
	float top = r.y + text + pad;
	int fit = (int)((r.y + r.height - pad * 0.5f - top) / line);
	int shown = export_reg_count < fit ? (int)export_reg_count : fit;
	if (shown > EXPORT_REGISTER_LINES)
		shown = EXPORT_REGISTER_LINES;
	// fixed columns so the digits line up: chip, register, value, value in bits
	float unit = MeasureTextEx(font, "0", size, 1).x;
	float x_chip = r.x + pad, x_reg = x_chip + unit * 5, x_value = x_reg + unit * 6.5f, x_bits = x_value + unit * 3.5f;
	bool room_for_bits = x_bits + unit * 9.5f < r.x + r.width;
	for (int i=0; i<shown; i++) {
		// i counts up from the newest, at the bottom
		Audio_write* wr = &export_reg_lines[(export_reg_count - 1 - i) % EXPORT_REGISTER_LINES];
		float y = top + (fit - 1 - i) * line;
		float age = fit > 1 ? (float)i / (fit - 1) : 0;
		unsigned char alpha = (unsigned char)(255 * (1.0f - 0.55f * age));
		Color color = (0 <= wr->channel && !audio_mute[wr->channel]) ? channel_colors[wr->channel] : (Color){ 190, 190, 200, 255 };
		color.a = alpha;
		Color dim = { 120, 120, 130, alpha };
		bool own_registers = wr->chip == chip_n163 || wr->chip == chip_vrc7; // reached through an address latch
		DrawTextEx(font, export_chip_names[wr->chip], (Vector2){ x_chip, y }, size, 1, dim);
		DrawTextEx(font, own_registers ? TextFormat("$%02X", wr->reg) : TextFormat("$%04X", wr->reg), (Vector2){ x_reg, y }, size, 1, color);
		DrawTextEx(font, TextFormat("%02X", wr->value), (Vector2){ x_value, y }, size, 1, (Color){ 255, 255, 255, alpha });
		if (room_for_bits) {
			char bits[10];
			for (int b=0; b<8; b++)
				bits[b + (b >= 4)] = (wr->value >> (7 - b)) & 1 ? '1' : '0';
			bits[4] = ' ';
			bits[9] = 0;
			DrawTextEx(font, bits, (Vector2){ x_bits, y }, size, 1, dim);
		}
	}
}

// the export frame: the scopes in a grid over most of it, the register log beside
// them, the piano roll along the bottom
static void export_frame(Famicom* famicom)
{
	const Export_size* size = &export_sizes[export_size];
	int w = size->width, h = size->height;
	int scope_h = (int)(h * 0.72f);
	int piano_h = h - scope_h;
	// the frame drawn last time is read back first: the gpu has had the emulation's
	// worth of time to finish it, so the read doesn't sit waiting on fresh drawing
	export_send_frame();
	if (export_writer_failed) {
		snprintf(export_status, sizeof(export_status), "FFmpeg stopped taking frames");
		export_stop();
		return;
	}
	piano_render(export_piano, w, piano_h);

	int visible[channel_count];
	int n = 0;
	for (int k=0; k<channel_count; k++) {
		if (channel_visible(famicom, scope_order[k]))
			visible[n++] = scope_order[k];
	}
	int columns = n <= 1 ? 1 : n <= 4 ? 2 : 3;
	int rows = (n + columns - 1) / columns;
	float text = h / 32.0f;
	float pad = h / 90.0f;
	float title_h = text * 1.5f;
	// the register log takes a column on the right, the scopes keep the rest of the top
	float reg_w = w * 0.2f;
	float scopes_w = w - reg_w;
	float cell_w = (scopes_w - pad * (columns + 1)) / columns;
	float cell_h = (scope_h - title_h - pad * (rows + 1)) / rows;
	Font font = export_font_loaded ? export_font : GetFontDefault();
	export_take_register_writes();

	BeginTextureMode(export_tex);
	ClearBackground(BLACK);
	// what's playing, along the top
	char title[300];
	if (famicom->nsf.active) {
		const char* label = famicom->nsf.track_label[famicom->nsf.song];
		snprintf(title, sizeof(title), "%s  -  %s", famicom->nsf.title[0] ? famicom->nsf.title : GetFileName(famicom->rom_path),
			 label[0] ? label : TextFormat("song %d", famicom->nsf.song + 1));
	} else {
		snprintf(title, sizeof(title), "%s", famicom_has_cartridge(famicom) ? GetFileName(famicom->rom_path) : "nemu");
	}
	DrawTextEx(font, title, (Vector2){ pad, pad * 0.6f }, text, 1, WHITE);
	for (int i=0; i<n; i++) {
		int c = visible[i];
		Rectangle cell = { pad + (i % columns) * (cell_w + pad), title_h + pad + (i / columns) * (cell_h + pad), cell_w, cell_h };
		// black cells, only a faint outline between them
		DrawRectangleLinesEx(cell, 1, (Color){ 28, 28, 32, 255 });
		Rectangle trace = { cell.x + pad, cell.y + text + pad, cell.width - pad * 2, cell.height - text - pad * 2 };
		export_trace(c, trace, h / 400.0f + 1.0f);
		Color color = audio_mute[c] ? (Color){ 110, 110, 110, 255 } : channel_colors[c];
		DrawTextEx(font, channel_names[c], (Vector2){ cell.x + pad, cell.y + pad * 0.5f }, text * 0.8f, 1, color);
	}
	export_draw_registers((Rectangle){ scopes_w, title_h + pad, reg_w - pad, scope_h - title_h - pad * 2 }, text, pad);
	// the piano roll draws itself the way nuklear wants, which is how the screen wants it
	DrawTexturePro(export_piano.texture, (Rectangle){ 0, 0, w, piano_h }, (Rectangle){ 0, scope_h, w, piano_h }, (Vector2){ 0, 0 }, 0, WHITE);
	EndTextureMode();
	if (export_use_yuv)
		export_convert_yuv(w, h);
	if (export_pbo_ready)
		export_pbo_start(export_readback.texture.width, export_readback.texture.height);
	else
		export_frame_waiting = true;
	export_frames++;
}

void export_panel(Famicom* famicom)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Video export", NK_MINIMIZED))
		return;
	nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 3, (float[]){ 0.15f, 0.35f, 0.5f });
	nk_label(ctx, "Size", NK_TEXT_LEFT);
	const char* names[EXPORT_SIZES];
	for (int i=0; i<EXPORT_SIZES; i++)
		names[i] = export_sizes[i].name;
	if (exporting) {
		nk_label(ctx, names[export_size], NK_TEXT_LEFT);
	} else {
		int picked = nk_combo(ctx, names, EXPORT_SIZES, export_size, UI_BUTTON, nk_vec2(160, 120));
		if (picked != export_size) {
			export_size = picked;
			config_dirty = true;
		}
	}
	// the first press fades out and then saves, a second one saves straight away
	if (nk_button_label(ctx, !exporting ? "Render" : export_fading ? "Save now" : "Stop and save")) {
		if (!exporting)
			export_start(famicom);
		else if (export_fading)
			export_stop();
		else
			export_begin_fade();
	}
	nk_layout_row_dynamic(ctx, UI_ROW, 1);
	if (exporting) {
		long seconds = (long)(export_frames / export_fps);
		nk_label(ctx, TextFormat("%s %ld:%02ld, %ld frames, %.1fx speed", export_fading ? "fading out" : "rendering",
			 seconds / 60, seconds % 60, export_frames, export_speed), NK_TEXT_LEFT);
	} else {
		nk_label(ctx, export_status[0] ? export_status : "Renders the scopes and piano roll with sound to exports/", NK_TEXT_LEFT);
	}
	nk_layout_row_static(ctx, UI_BUTTON, 140, 1);
	if (nk_button_label(ctx, "Open exports folder")) {
		char folder[1100];
		snprintf(folder, sizeof(folder), "%sexports", GetApplicationDirectory());
		if (!DirectoryExists(folder))
			MakeDirectory(folder);
		OpenURL(folder);
	}
	nk_tree_pop(ctx);
}

void piano_window()
{
	if (!piano_open)
		return;
	if (nk_begin(ctx, "Piano roll", nk_rect(250, 40, 720, 440), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_CLOSABLE|NK_WINDOW_TITLE|NK_WINDOW_MINIMIZABLE|NK_WINDOW_NO_SCROLLBAR)) {
		nk_layout_row_dynamic(ctx, UI_BUTTON, 3);
		float top = nk_widget_bounds(ctx).y;
		nk_property_int(ctx, "Lowest octave", 0, &piano_low_octave, 7, 1, 0.1f);
		nk_property_int(ctx, "Octaves", 1, &piano_octaves, 8, 1, 0.1f);
		nk_property_int(ctx, "Speed", 1, &piano_speed, 8, 1, 0.1f);
		if (8 < piano_low_octave + piano_octaves)
			piano_octaves = 8 - piano_low_octave;
		// the roll takes the rest of the window
		struct nk_rect content = nk_window_get_content_region(ctx);
		int height = content.y + content.h - (top + UI_BUTTON + ctx->style.window.spacing.y) - 2;
		nk_layout_row_dynamic(ctx, height < 48 ? 48 : height, 1);
		piano_resize(nk_widget_width(ctx), height);
		nk_image(ctx, TextureToNuklearImage(piano_tex.texture));
	} else {
		piano_open = false;
	}
	nk_end(ctx);
}

// a labelled slider; returns true when it moved
static bool fx_slider(const char* label, float* value, float min, float max, float step)
{
	nk_label(ctx, label, NK_TEXT_LEFT);
	float old = *value;
	nk_slider_float(ctx, min, value, max, step);
	return old != *value;
}

static bool fx_toggle(const char* label, bool* value)
{
	nk_bool on = *value;
	nk_checkbox_label(ctx, label, &on);
	bool changed = (bool)on != *value;
	*value = on;
	return changed;
}

#define FX_ROW nk_layout_row(ctx, NK_DYNAMIC, UI_ROW, 4, (float[]){ 0.17f, 0.33f, 0.17f, 0.33f })

void effects_panel(Famicom* famicom)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Effects", NK_MINIMIZED))
		return;
	bool changed = false;
	Audio_effects* fx = &audio_fx;

	// waveforms for the 2A03's tonal channels
	nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 6, (float[]){ 0.11f, 0.22f, 0.11f, 0.22f, 0.12f, 0.22f });
	const char* wave_labels[wave_channel_count] = { "Pulse 1", "Pulse 2", "Triangle" };
	for (int w=0; w<wave_channel_count; w++) {
		nk_label(ctx, wave_labels[w], NK_TEXT_LEFT);
		int picked = nk_combo(ctx, audio_waveform_names, wave_count, fx->waveform[w], UI_BUTTON, nk_vec2(140, 180));
		if (picked != fx->waveform[w]) {
			fx->waveform[w] = picked;
			changed = true;
		}
	}
	// a .wav for each channel's sample waveform, picking one also selects it
	nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 6, (float[]){ 0.11f, 0.22f, 0.11f, 0.22f, 0.12f, 0.22f });
	for (int w=0; w<wave_channel_count; w++) {
		nk_spacing(ctx, 1);
		const char* label = wave_file[w][0] ? GetFileName(wave_file[w]) : "Load .wav...";
		if (nk_button_label(ctx, label)) {
			picker_wave_channel = w;
			picker_show(famicom, picker_wave);
		}
	}
	nk_layout_row_dynamic(ctx, UI_ROW, 1);
	changed |= fx_toggle("Follow pulse duty (squeezes the other waveforms like pulse width)", &fx->follow_duty);

	nk_layout_row_dynamic(ctx, UI_ROW, 5);
	changed |= fx_toggle("Limiter", &fx->limiter);
	changed |= fx_toggle("Chorus", &fx->chorus);
	changed |= fx_toggle("Echo", &fx->echo);
	changed |= fx_toggle("Reverb", &fx->reverb);
	changed |= fx_toggle("Stereo widening", &fx->widen);

	FX_ROW;
	changed |= fx_slider(TextFormat("Echo %dms", (int)fx->echo_time), &fx->echo_time, 20, 1000, 1);
	changed |= fx_slider(TextFormat("Feedback %.2f", fx->echo_feedback), &fx->echo_feedback, 0, 0.95f, 0.01f);
	FX_ROW;
	changed |= fx_slider(TextFormat("Echo mix %.2f", fx->echo_mix), &fx->echo_mix, 0, 1, 0.01f);
	changed |= fx_toggle("Ping-pong", &fx->echo_pingpong);
	nk_spacing(ctx, 1);

	FX_ROW;
	changed |= fx_slider(TextFormat("Limit %.1fdB", fx->limiter_threshold), &fx->limiter_threshold, -24, 0, 0.5f);
	changed |= fx_slider(TextFormat("Chorus mix %.2f", fx->chorus_mix), &fx->chorus_mix, 0, 1, 0.01f);
	FX_ROW;
	changed |= fx_slider(TextFormat("Rate %.2fHz", fx->chorus_rate), &fx->chorus_rate, 0.05f, 5, 0.05f);
	changed |= fx_slider(TextFormat("Depth %.1fms", fx->chorus_depth), &fx->chorus_depth, 0.1f, 10, 0.1f);
	FX_ROW;
	changed |= fx_slider(TextFormat("Reverb mix %.2f", fx->reverb_mix), &fx->reverb_mix, 0, 1, 0.01f);
	changed |= fx_slider(TextFormat("Room %.2f", fx->reverb_size), &fx->reverb_size, 0, 1, 0.01f);
	FX_ROW;
	changed |= fx_slider(TextFormat("Damping %.2f", fx->reverb_damping), &fx->reverb_damping, 0, 1, 0.01f);
	changed |= fx_slider(TextFormat("Width %.2f", fx->width), &fx->width, 0, 3, 0.01f);
	FX_ROW;
	changed |= fx_slider(TextFormat("Surround %.2f", fx->surround), &fx->surround, 0, 1, 0.01f);
	nk_spacing(ctx, 2);

	// the chorus can be kept to some channels
	nk_layout_row_dynamic(ctx, UI_ROW, 1);
	nk_label(ctx, "Chorus on:", NK_TEXT_LEFT);
	nk_layout_row_dynamic(ctx, UI_ROW, 5);
	for (int c=0; c<channel_count; c++) {
		if (channel_visible(famicom, c))
			changed |= fx_toggle(channel_names[c], &fx->chorus_channels[c]);
	}
	nk_layout_row_static(ctx, UI_BUTTON, 120, 1);
	if (nk_button_label(ctx, "Reset effects")) {
		audio_fx = audio_fx_defaults;
		changed = true;
	}
	// saved once the slider is let go, not on every step of a drag
	if (changed)
		config_dirty = true;
	nk_tree_pop(ctx);
}

// the eq's frequency response, drawn straight onto the window
static void eq_graph(float height)
{
	nk_layout_row_dynamic(ctx, height, 1);
	struct nk_rect b;
	if (!nk_widget(&b, ctx))
		return;
	struct nk_command_buffer* canvas = nk_window_get_canvas(ctx);
	nk_fill_rect(canvas, b, 3, nk_rgb(16, 16, 20));
	float mid = b.y + b.h / 2;
	float scale = (b.h / 2 - 4) / EQ_MAX_GAIN; // pixels per dB
	// grid: 0dB, +-6dB, and the decades from 100Hz to 10kHz
	nk_stroke_line(canvas, b.x, mid, b.x + b.w, mid, 1, nk_rgb(70, 70, 80));
	nk_stroke_line(canvas, b.x, mid - 6 * scale, b.x + b.w, mid - 6 * scale, 1, nk_rgb(36, 36, 44));
	nk_stroke_line(canvas, b.x, mid + 6 * scale, b.x + b.w, mid + 6 * scale, 1, nk_rgb(36, 36, 44));
	const float low = log10f(20), high = log10f(20000);
	for (float decade=100; decade<20000; decade*=10) {
		float x = b.x + (log10f(decade) - low) / (high - low) * b.w;
		nk_stroke_line(canvas, x, b.y, x, b.y + b.h, 1, nk_rgb(36, 36, 44));
	}
	// band markers and the curve
	struct nk_color curve = audio_fx.eq ? nk_rgb(110, 200, 255) : nk_rgb(90, 90, 100);
	for (int band=0; band<EQ_BANDS; band++) {
		float x = b.x + (log10f(audio_fx.eq_freq[band]) - low) / (high - low) * b.w;
		float y = mid - audio_fx.eq_gain[band] * scale;
		nk_fill_circle(canvas, nk_rect(x - 3, y - 3, 6, 6), curve);
	}
	float px = 0, py = 0;
	for (int i=0; i<=(int)b.w; i+=2) {
		float frequency = powf(10.0f, low + (high - low) * i / b.w);
		float db = audio_eq_response(frequency);
		if (db < -EQ_MAX_GAIN - 3) db = -EQ_MAX_GAIN - 3;
		if (EQ_MAX_GAIN + 3 < db) db = EQ_MAX_GAIN + 3;
		float x = b.x + i, y = mid - db * scale;
		if (i)
			nk_stroke_line(canvas, px, py, x, y, 2, curve);
		px = x;
		py = y;
	}
}

void eq_panel(void)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "EQ", NK_MINIMIZED))
		return;
	bool changed = false;
	nk_layout_row_dynamic(ctx, UI_ROW, 1);
	changed |= fx_toggle("Enable EQ", &audio_fx.eq);
	eq_graph(UI_ROW * 5);
	for (int band=0; band<EQ_BANDS; band++) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 5, (float[]){ 0.12f, 0.12f, 0.30f, 0.12f, 0.34f });
		nk_label(ctx, audio_eq_band_names[band], NK_TEXT_LEFT);
		float frequency = audio_fx.eq_freq[band];
		nk_label(ctx, frequency < 1000 ? TextFormat("%dHz", (int)frequency) : TextFormat("%.1fkHz", frequency / 1000), NK_TEXT_RIGHT);
		// frequency moves on a log scale, so each octave gets the same slider travel
		// only written back when dragged, the log round trip isn't exact
		float position = log10f(frequency);
		float before = position;
		nk_slider_float(ctx, log10f(20), &position, log10f(20000), 0.005f);
		if (position != before) {
			audio_fx.eq_freq[band] = powf(10.0f, position);
			changed = true;
		}
		nk_label(ctx, TextFormat("%+.1fdB", audio_fx.eq_gain[band]), NK_TEXT_RIGHT);
		float gain = audio_fx.eq_gain[band];
		nk_slider_float(ctx, -EQ_MAX_GAIN, &audio_fx.eq_gain[band], EQ_MAX_GAIN, 0.1f);
		changed |= gain != audio_fx.eq_gain[band];
	}
	nk_layout_row_static(ctx, UI_BUTTON, 120, 1);
	if (nk_button_label(ctx, "Reset EQ")) {
		memcpy(audio_fx.eq_freq, audio_fx_defaults.eq_freq, sizeof(audio_fx.eq_freq));
		memcpy(audio_fx.eq_gain, audio_fx_defaults.eq_gain, sizeof(audio_fx.eq_gain));
		changed = true;
	}
	if (changed)
		config_dirty = true;
	nk_tree_pop(ctx);
}

void mixer_panel(Famicom* famicom)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Mixer", NK_MINIMIZED))
		return;
	bool changed = false;
	for (int c=0; c<channel_count; c++) {
		if (!channel_visible(famicom, c))
			continue;
		Color color = channel_colors[c];
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 4, (float[]){ 0.13f, 0.12f, 0.45f, 0.30f });
		nk_label_colored(ctx, channel_names[c], NK_TEXT_LEFT, nk_rgb(color.r, color.g, color.b));
		nk_label(ctx, TextFormat("%d%%", (int)roundf(audio_gain[c] * 100)), NK_TEXT_RIGHT);
		float gain = audio_gain[c];
		nk_slider_float(ctx, 0, &audio_gain[c], 2, 0.01f);
		changed |= gain != audio_gain[c];
		int octave = audio_transpose[c];
		nk_property_int(ctx, "#Octave", -AUDIO_MAX_TRANSPOSE, &audio_transpose[c], AUDIO_MAX_TRANSPOSE, 1, 0.1f);
		changed |= octave != audio_transpose[c];
	}
	nk_layout_row_static(ctx, UI_BUTTON, 120, 1);
	if (nk_button_label(ctx, "Reset mixer")) {
		for (int c=0; c<channel_count; c++) {
			audio_gain[c] = 1.0f;
			audio_transpose[c] = 0;
		}
		changed = true;
	}
	if (changed)
		config_dirty = true;
	nk_tree_pop(ctx);
}

void apu_window(Famicom* famicom)
{
	update_register_heat();
	if (nk_begin(ctx, "APU", nk_rect(800, 0, 770, 790), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE|NK_WINDOW_NO_SCROLLBAR)) {
		struct nk_vec2 spacing = ctx->style.window.spacing;
		nk_layout_row_dynamic(ctx, UI_BUTTON, 4);
		nk_checkbox_label(ctx, "Smooth", &scope_smooth);
		nk_bool perfect = audio_perfect_triangle;
		nk_checkbox_label(ctx, "Perfect triangle", &perfect);
		audio_perfect_triangle = perfect;
		nk_checkbox_label(ctx, "Piano roll", &piano_open);
		// back to the plain hardware sound: mixer, effects, waveforms and mutes
		if (nk_button_label(ctx, "Reset tweaks")) {
			audio_reset_tweaks();
			config_dirty = true;
		}
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 3, (float[]){ 0.10f, 0.08f, 0.82f });
		nk_label(ctx, "Volume", NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("%d%%", (int)roundf(audio_master_volume * 100)), NK_TEXT_RIGHT);
		float master = audio_master_volume;
		nk_slider_float(ctx, 0, &audio_master_volume, 2, 0.01f);
		if (master != audio_master_volume)
			config_dirty = true;
		// every channel, in semitones and cents, on top of the mixer's octaves
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 4, (float[]){ 0.10f, 0.12f, 0.70f, 0.08f });
		nk_label(ctx, "Pitch", NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("%+.2f st", audio_pitch_shift), NK_TEXT_RIGHT);
		float pitch = audio_pitch_shift;
		nk_slider_float(ctx, -AUDIO_MAX_PITCH_SHIFT, &audio_pitch_shift, AUDIO_MAX_PITCH_SHIFT, 0.01f);
		if (nk_button_label(ctx, "0"))
			audio_pitch_shift = 0;
		if (pitch != audio_pitch_shift)
			config_dirty = true;

		mixer_panel(famicom);
		effects_panel(famicom);
		eq_panel();
		export_panel(famicom);

		if (nk_tree_push(ctx, NK_TREE_TAB, "Registers", NK_MAXIMIZED)) {
			register_row("Pulse 1", 0x00, 4);
			register_row("Pulse 2", 0x04, 4);
			register_row("Triangle", 0x08, 4);
			register_row("Noise", 0x0C, 4);
			register_row("DMC", 0x10, 4);
			nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 5, (float[]){0.16f, 0.21f, 0.21f, 0.21f, 0.21f});
			nk_label(ctx, "Control", NK_TEXT_LEFT);
			register_cell(0x15);
			register_cell(0x17);
			nk_spacing(ctx, 2);
			nk_tree_pop(ctx);
		}

		// last in the window so the scopes can take everything down to the bottom edge
		if (exporting && nk_tree_push(ctx, NK_TREE_TAB, "Oscilloscopes", NK_MAXIMIZED)) {
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			nk_label(ctx, "Paused while the video renders", NK_TEXT_LEFT);
			nk_tree_pop(ctx);
		} else if (!exporting && nk_tree_push(ctx, NK_TREE_TAB, "Oscilloscopes", NK_MAXIMIZED)) {
			// positions in scope_order of the channels shown right now
			int visible[channel_count];
			int shown = 0;
			for (int k=0; k<channel_count; k++) {
				if (channel_visible(famicom, scope_order[k]))
					visible[shown++] = k;
			}
			const int columns = 2;
			int rows = (shown + columns - 1) / columns;
			int move_from = -1, move_to = -1;
			int height = 16;
			for (int r=0; r<rows; r++) {
				// mute box, name and reorder buttons, earlier/later in reading order
				nk_layout_row(ctx, NK_DYNAMIC, UI_ROW, 8, (float[]){0.07f, 0.31f, 0.06f, 0.06f, 0.07f, 0.31f, 0.06f, 0.06f});
				if (r == 0) {
					// measure from where the scopes start to the bottom of the window
					struct nk_rect content = nk_window_get_content_region(ctx);
					float top = nk_widget_bounds(ctx).y;
					float space = content.y + content.h - top - rows * (UI_ROW + 2 * spacing.y) - 2;
					height = space / rows;
					if (height < 16) height = 16;
					if (SCOPE_MAX_HEIGHT < height) height = SCOPE_MAX_HEIGHT;
				}
				for (int col=0; col<columns; col++) {
					int v = r * columns + col;
					if (shown <= v) {
						nk_spacing(ctx, 4);
						continue;
					}
					int c = scope_order[visible[v]];
					Color color = channel_colors[c];
					// ticked means audible
					nk_bool on = !audio_mute[c];
					nk_checkbox_label(ctx, "", &on);
					audio_mute[c] = !on;
					nk_label_colored(ctx, channel_names[c], NK_TEXT_LEFT, nk_rgb(color.r, color.g, color.b));
					if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT) && 0 < v) {
						move_from = visible[v];
						move_to = visible[v - 1];
					}
					if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT) && v < shown - 1) {
						move_from = visible[v];
						move_to = visible[v + 1];
					}
				}
				nk_layout_row_dynamic(ctx, height, columns);
				// resize the canvases before any of them is queued for drawing,
				// queued images keep the texture size they were queued with
				if (r == 0)
					scope_resize(nk_widget_width(ctx), height);
				for (int col=0; col<columns; col++) {
					int v = r * columns + col;
					if (v < shown)
						nk_image(ctx, TextureToNuklearImage(channel_scope_tex[scope_order[visible[v]]].texture));
					else
						nk_spacing(ctx, 1);
				}
			}
			// reorder after drawing so nothing is drawn twice this frame
			if (0 <= move_from) {
				int c = scope_order[move_from];
				scope_order[move_from] = scope_order[move_to];
				scope_order[move_to] = c;
			}
			nk_tree_pop(ctx);
		}
	}
	nk_end(ctx);
}

// ppu debug views, built on the cpu side and uploaded to plain textures
#define OAM_CELL_W 10
#define OAM_CELL_H 18

bool ppu_debug_open = false;
int ppu_debug_view = 0; // 0 nametables, 1 sprites
nk_bool ppu_debug_scroll = true;
int ppu_debug_hovered = -1;
Texture2D nametable_tex;
Texture2D sprite_layer_tex;
Texture2D oam_grid_tex;
Color nametable_buf[480][512];
Color sprite_layer_buf[240][256];
Color oam_grid_buf[OAM_GRID_H][OAM_GRID_W];

static Texture2D debug_texture(int width, int height)
{
	Image image = GenImageColor(width, height, BLACK);
	Texture2D tex = LoadTextureFromImage(image);
	UnloadImage(image);
	return tex;
}

// draws one 8x8 tile into a buffer, colour 0 is skipped unless opaque
static void debug_tile(Famicom* f, Color* buf, int buf_w, int buf_h, int x, int y,
		       word addr, int palette_base, bool hflip, bool vflip, bool opaque)
{
	for (int row=0; row<8; row++) {
		int src = vflip ? 7 - row : row;
		byte lo = ppu_peek(f, addr + src);
		byte hi = ppu_peek(f, addr + src + 8);
		int py = y + row;
		if (py < 0 || buf_h <= py)
			continue;
		for (int col=0; col<8; col++) {
			int bit = hflip ? col : 7 - col;
			int pixel = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
			if (pixel == 0 && !opaque)
				continue;
			int px = x + col;
			if (px < 0 || buf_w <= px)
				continue;
			byte index = pixel ? f->ppu.palettes[(palette_base + pixel) & 0x1F] : f->ppu.palettes[0];
			buf[py * buf_w + px] = color_lookup(index);
		}
	}
}

static void debug_box(Color* buf, int buf_w, int buf_h, int x, int y, int w, int h, Color color, bool wrap)
{
	for (int i=0; i<w; i++) {
		for (int e=0; e<2; e++) {
			int px = x + i, py = e ? y + h - 1 : y;
			if (wrap) { px = (px % buf_w + buf_w) % buf_w; py = (py % buf_h + buf_h) % buf_h; }
			if (0 <= px && px < buf_w && 0 <= py && py < buf_h)
				buf[py * buf_w + px] = color;
		}
	}
	for (int j=0; j<h; j++) {
		for (int e=0; e<2; e++) {
			int px = e ? x + w - 1 : x, py = y + j;
			if (wrap) { px = (px % buf_w + buf_w) % buf_w; py = (py % buf_h + buf_h) % buf_h; }
			if (0 <= px && px < buf_w && 0 <= py && py < buf_h)
				buf[py * buf_w + px] = color;
		}
	}
}

static void build_nametables(Famicom* f)
{
	word table = (f->ppu.ctrl & 0x10) ? 0x1000 : 0;
	for (int nt=0; nt<4; nt++) {
		int ox = (nt & 1) * 256;
		int oy = (nt >> 1) * 240;
		word base = 0x2000 + nt * 0x400;
		for (int ty=0; ty<30; ty++) {
			for (int tx=0; tx<32; tx++) {
				byte tile = ppu_peek(f, base + ty * 32 + tx);
				byte at = ppu_peek(f, base + 0x3C0 + (ty / 4) * 8 + tx / 4);
				int shift = ((ty & 2) ? 4 : 0) + ((tx & 2) ? 2 : 0);
				debug_tile(f, &nametable_buf[0][0], 512, 480, ox + tx * 8, oy + ty * 8,
					   table + tile * 16, ((at >> shift) & 3) * 4, false, false, true);
			}
		}
	}
	if (ppu_debug_scroll) {
		// the scroll the game last set, from the ppu's t register
		word t = f->ppu.t;
		int sx = ((t & 0x1F) << 3 | f->ppu.fine_x) + ((t >> 10) & 1) * 256;
		int sy = (((t >> 5) & 0x1F) << 3 | ((t >> 12) & 7)) + ((t >> 11) & 1) * 240;
		debug_box(&nametable_buf[0][0], 512, 480, sx, sy, 256, 240, (Color){ 255, 230, 60, 255 }, true);
	}
	UpdateTexture(nametable_tex, nametable_buf);
}

static void draw_sprite(Famicom* f, Color* buf, int buf_w, int buf_h, int i, int x, int y)
{
	byte* s = &f->ppu.oam[i * 4];
	byte tile = s[1];
	byte attr = s[2];
	bool hflip = attr & 0x40, vflip = attr & 0x80;
	int palette_base = 0x10 + (attr & 3) * 4;
	if (f->ppu.ctrl & 0x20) {
		// 8x16 sprites pick their table from bit 0, flipping swaps the halves
		word table = (tile & 1) ? 0x1000 : 0;
		byte top = tile & 0xFE;
		debug_tile(f, buf, buf_w, buf_h, x, y, table + (vflip ? top + 1 : top) * 16, palette_base, hflip, vflip, false);
		debug_tile(f, buf, buf_w, buf_h, x, y + 8, table + (vflip ? top : top + 1) * 16, palette_base, hflip, vflip, false);
	} else {
		word table = (f->ppu.ctrl & 0x08) ? 0x1000 : 0;
		debug_tile(f, buf, buf_w, buf_h, x, y, table + tile * 16, palette_base, hflip, vflip, false);
	}
}

static void build_sprites(Famicom* f)
{
	int height = (f->ppu.ctrl & 0x20) ? 16 : 8;
	Color backdrop = { 28, 28, 32, 255 };
	for (int y=0; y<240; y++)
		for (int x=0; x<256; x++)
			sprite_layer_buf[y][x] = backdrop;
	// back to front so lower numbered sprites end up on top, like the ppu
	for (int i=63; 0<=i; i--) {
		byte* s = &f->ppu.oam[i * 4];
		if (0xEF <= s[0])
			continue;
		draw_sprite(f, &sprite_layer_buf[0][0], 256, 240, i, s[3], s[0] + 1);
	}
	if (0 <= ppu_debug_hovered) {
		byte* s = &f->ppu.oam[ppu_debug_hovered * 4];
		debug_box(&sprite_layer_buf[0][0], 256, 240, s[3] - 1, s[0], 10, height + 2, (Color){ 255, 230, 60, 255 }, false);
	}
	UpdateTexture(sprite_layer_tex, sprite_layer_buf);

	for (int y=0; y<OAM_GRID_H; y++)
		for (int x=0; x<OAM_GRID_W; x++)
			oam_grid_buf[y][x] = (Color){ 16, 16, 18, 255 };
	for (int i=0; i<64; i++) {
		int cx = (i % 8) * OAM_CELL_W, cy = (i / 8) * OAM_CELL_H;
		draw_sprite(f, &oam_grid_buf[0][0], OAM_GRID_W, OAM_GRID_H, i, cx + 1, cy + 1);
		if (i == ppu_debug_hovered)
			debug_box(&oam_grid_buf[0][0], OAM_GRID_W, OAM_GRID_H, cx, cy, OAM_CELL_W, OAM_CELL_H, (Color){ 255, 230, 60, 255 }, false);
	}
	UpdateTexture(oam_grid_tex, oam_grid_buf);
}

void ppu_debug_window(Famicom* famicom)
{
	if (!ppu_debug_open)
		return;
	if (nk_begin(ctx, "PPU Debug", nk_rect(260, 60, 560, 560), NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_CLOSABLE|NK_WINDOW_TITLE|NK_WINDOW_MINIMIZABLE)) {
		nk_layout_row_dynamic(ctx, UI_ROW, 3);
		if (nk_option_label(ctx, "Nametables", ppu_debug_view == 0)) ppu_debug_view = 0;
		if (nk_option_label(ctx, "Sprites", ppu_debug_view == 1)) ppu_debug_view = 1;
		if (ppu_debug_view == 0)
			nk_checkbox_label(ctx, "Show scroll", &ppu_debug_scroll);
		else
			nk_spacing(ctx, 1);

		bool have_rom = famicom_has_cartridge(famicom);
		float width = nk_window_get_content_region(ctx).w - 2 * ctx->style.window.padding.x;
		if (ppu_debug_view == 0) {
			if (have_rom)
				build_nametables(famicom);
			nk_layout_row_static(ctx, width * 480 / 512, width, 1);
			nk_image(ctx, TextureToNuklearImage(nametable_tex));
		} else {
			if (have_rom)
				build_sprites(famicom);
			// sprite layer and oam grid side by side at the same height
			float layer_w = width * 0.64f;
			float height = layer_w * 240 / 256;
			float grid_w = height * OAM_GRID_W / OAM_GRID_H;
			nk_layout_row(ctx, NK_STATIC, height, 2, (float[]){ layer_w, grid_w });
			nk_image(ctx, TextureToNuklearImage(sprite_layer_tex));
			struct nk_rect grid = nk_widget_bounds(ctx);
			nk_image(ctx, TextureToNuklearImage(oam_grid_tex));

			ppu_debug_hovered = -1;
			struct nk_vec2 mouse = ctx->input.mouse.pos;
			if (nk_input_is_mouse_hovering_rect(&ctx->input, grid)) {
				int cx = (mouse.x - grid.x) / grid.w * 8;
				int cy = (mouse.y - grid.y) / grid.h * 8;
				if (0 <= cx && cx < 8 && 0 <= cy && cy < 8)
					ppu_debug_hovered = cy * 8 + cx;
			}
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			if (0 <= ppu_debug_hovered && have_rom) {
				byte* s = &famicom->ppu.oam[ppu_debug_hovered * 4];
				nk_label(ctx, TextFormat("Sprite %d   X %d   Y %d   Tile $%02X   Attr $%02X",
							 ppu_debug_hovered, s[3], s[0], s[1], s[2]), NK_TEXT_LEFT);
				nk_label(ctx, TextFormat("Palette %d%s%s%s%s", s[2] & 3,
							 (s[2] & 0x20) ? ", behind background" : "",
							 (s[2] & 0x40) ? ", flipped H" : "",
							 (s[2] & 0x80) ? ", flipped V" : "",
							 0xEF <= s[0] ? ", offscreen" : ""), NK_TEXT_LEFT);
			} else {
				nk_label(ctx, "Hover a sprite in the grid for details", NK_TEXT_LEFT);
			}
		}
	} else {
		ppu_debug_open = false;
	}
	nk_end(ctx);
}

void ppu_window()
{
	if (nk_begin(ctx, "PPU", nk_rect(250, 0, 256 * ppu_scale + 30, 240 * ppu_scale + 80), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE))
	{
		nk_layout_row_static(ctx, UI_BUTTON, 90, 2);
		if (nk_button_label(ctx, TextFormat("Scale: %dx", ppu_scale))) {
			ppu_scale = ppu_scale % 4 + 1;
			nk_window_set_size(ctx, "PPU", nk_vec2(256 * ppu_scale + 30, 240 * ppu_scale + 80));
		}
		if (nk_button_label(ctx, "Debug..."))
			ppu_debug_open = true;
		nk_layout_row_static(ctx, 240 * ppu_scale, 256 * ppu_scale, 1);
		if (ntsc_ready && ntsc_enabled && ntsc_target.id)
			nk_image(ctx, TextureToNuklearImage(ntsc_target.texture));
		else
			nk_image(ctx, TextureToNuklearImage(ppu_fb_tex.texture));
	}
	nk_end(ctx);
}

static bool palette_slider(const char* label, float* value, float min, float max, float step)
{
	nk_layout_row(ctx, NK_DYNAMIC, UI_ROW, 2, (float[]){0.42f, 0.58f});
	nk_label(ctx, label, NK_TEXT_LEFT);
	float old = *value;
	nk_slider_float(ctx, min, value, max, step);
	return old != *value;
}

void palette_window(Famicom* famicom)
{
	if (nk_begin_titled(ctx, "Palette", "Video", nk_rect(0, 305, 240, 270), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE)) {
		// presets; a file loaded by hand shows as its own entry at the end
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){ 0.25f, 0.75f });
		nk_label(ctx, "Preset", NK_TEXT_LEFT);
		int count = palette_preset_count;
		int shown = palette_preset;
		if (palette_preset < 0) {
			palette_preset_names[count] = palette_name;
			shown = count++;
		}
		int picked = nk_combo(ctx, palette_preset_names, count, shown, UI_BUTTON, nk_vec2(220, 260));
		if (picked != shown && picked < palette_preset_count) {
			palette_select_preset(picked);
			config_dirty = true;
		}
		nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
		if (nk_button_label(ctx, "Load .pal..."))
			picker_show(famicom, picker_palette);
		if (nk_button_label(ctx, "Default")) {
			palette_select_preset(0);
			config_dirty = true;
		}
		bool changed = false;
		changed |= palette_slider(TextFormat("Hue %+.0f", palette_hue), &palette_hue, -180, 180, 1);
		changed |= palette_slider(TextFormat("Saturation %.2f", palette_saturation), &palette_saturation, 0, 2, 0.01f);
		changed |= palette_slider(TextFormat("Contrast %.2f", palette_contrast), &palette_contrast, 0, 2, 0.01f);
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		if (nk_button_label(ctx, "Reset adjustments")) {
			palette_hue = 0;
			palette_saturation = 1;
			palette_contrast = 1;
			changed = true;
		}
		if (changed)
			palette_apply();

		if (ntsc_ready) {
			bool changed = false;
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			nk_bool enabled = ntsc_enabled;
			nk_checkbox_label(ctx, "Blargg NTSC filter (GPU)", &ntsc_enabled);
			changed |= enabled != ntsc_enabled;
			if (nk_tree_push(ctx, NK_TREE_NODE, "NTSC settings", NK_MINIMIZED)) {
				changed |= palette_slider(TextFormat("Saturation %.2f", ntsc.saturation), &ntsc.saturation, 0, 6, 0.05f);
				changed |= palette_slider(TextFormat("Resolution %+.2f", ntsc.resolution), &ntsc.resolution, -1, 1, 0.05f);
				changed |= palette_slider(TextFormat("Sharpness %+.2f", ntsc.sharpness), &ntsc.sharpness, -1, 1, 0.05f);
				changed |= palette_slider(TextFormat("Fringing %.2f", ntsc.fringing), &ntsc.fringing, 0, 1, 0.05f);
				changed |= palette_slider(TextFormat("Artifacts %.2f", ntsc.artifacts), &ntsc.artifacts, 0, 1, 0.05f);
				changed |= palette_slider(TextFormat("Bleed %+.2f", ntsc.bleed), &ntsc.bleed, -0.75f, 2, 0.05f);
				changed |= palette_slider(TextFormat("Luma cutoff %.3f", ntsc.luma_cutoff), &ntsc.luma_cutoff, 0, 1, 0.005f);
				changed |= palette_slider(TextFormat("Brightness %.2f", ntsc.brightness), &ntsc.brightness, 0, 2, 0.01f);
				changed |= palette_slider(TextFormat("Hue %+.2f", ntsc.hue), &ntsc.hue, -1, 6.3f, 0.05f);
				changed |= palette_slider(TextFormat("Phase angle %.0f", ntsc.phase_angle), &ntsc.phase_angle, 1, 360, 1);
				changed |= palette_slider(TextFormat("Vertical phase %.3f", ntsc.vertical_scale), &ntsc.vertical_scale, 0, 2, 0.05555f);
				changed |= palette_slider(TextFormat("Kernel %.0f", ntsc.kernel_half), &ntsc.kernel_half, 1, 16, 1);
				nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
				changed |= fx_toggle("Dot crawl", &ntsc.dot_crawl);
				if (nk_button_label(ctx, "Reset NTSC")) {
					ntsc = ntsc_defaults;
					changed = true;
				}
				nk_tree_pop(ctx);
			}
			if (changed)
				config_dirty = true;
		} else {
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			nk_label(ctx, "NTSC filter unavailable", NK_TEXT_LEFT);
		}
	}
	nk_end(ctx);
}

// favorite songs: an nsf, a track, and the sound tweaks it had when it was saved
#define MAX_FAVORITES 256
#define TWEAKS_SIZE 8192

typedef struct favorite {
	char path[1024];
	int song;
	char name[200];
	char* tweaks; // the tweaks as config lines
} Favorite;

Favorite favorites[MAX_FAVORITES];
int favorite_count = 0;
int favorite_playing = -1; // the favorite being played through, -1 when not
char favorite_status[160];

static char* tweaks_capture(void)
{
	char* text = calloc(TWEAKS_SIZE, 1);
	if (text)
		config_write_effects(text, TWEAKS_SIZE, true);
	return text;
}

// puts a favorite's tweaks back; anything it doesn't mention goes back to normal
static void tweaks_apply(const char* text)
{
	audio_reset_tweaks();
	for (int w=0; w<wave_channel_count; w++) {
		audio_set_wave_sample(w, NULL, 0);
		wave_file[w][0] = '\0';
	}
	const char* line = text;
	while (line && *line) {
		if (strncmp(line, "wave_file_", 10) == 0 && '0' <= line[10] && line[10] < '0' + wave_channel_count && line[11] == '=') {
			char path[1024];
			config_path_value(line + 12, path, sizeof(path));
			if (wave_load_file(line[10] - '0', path) != 0)
				snprintf(favorite_status, sizeof(favorite_status), "couldn't load %s", GetFileName(path));
		} else {
			config_effect(line);
		}
		const char* next = strchr(line, '\n');
		line = next ? next + 1 : NULL;
	}
	config_dirty = true;
}

static const char* favorites_path(void)
{
	return TextFormat("%sfavorites.txt", GetApplicationDirectory());
}

static void favorites_save(void)
{
	FILE* file = fopen(favorites_path(), "wb");
	if (file == NULL) {
		snprintf(favorite_status, sizeof(favorite_status), "couldn't save favorites");
		return;
	}
	for (int i=0; i<favorite_count; i++) {
		fprintf(file, "favorite_path=%s\nfavorite_song=%d\nfavorite_name=%s\n%sfavorite_end\n",
			favorites[i].path, favorites[i].song, favorites[i].name, favorites[i].tweaks ? favorites[i].tweaks : "");
	}
	fclose(file);
}

static void favorites_load(void)
{
	char* text = LoadFileText(favorites_path());
	if (text == NULL)
		return;
	Favorite current = { 0 };
	char* tweaks = calloc(TWEAKS_SIZE, 1);
	const char* line = text;
	while (line && *line && tweaks) {
		if (strncmp(line, "favorite_path=", 14) == 0) {
			config_path_value(line + 14, current.path, sizeof(current.path));
		} else if (strncmp(line, "favorite_name=", 14) == 0) {
			config_path_value(line + 14, current.name, sizeof(current.name));
		} else if (sscanf(line, "favorite_song=%d", &current.song) == 1) {
		} else if (strncmp(line, "favorite_end", 12) == 0) {
			if (current.path[0] && favorite_count < MAX_FAVORITES) {
				current.tweaks = tweaks;
				favorites[favorite_count++] = current;
				ui_need_text(current.name);
				tweaks = calloc(TWEAKS_SIZE, 1);
			} else {
				tweaks[0] = '\0';
			}
			memset(&current, 0, sizeof(current));
		} else {
			// everything else in a block is one of its tweaks
			const char* end = strchr(line, '\n');
			size_t length = end ? (size_t)(end - line + 1) : strlen(line);
			size_t used = strlen(tweaks);
			if (used + length + 1 < TWEAKS_SIZE)
				memcpy(tweaks + used, line, length);
		}
		const char* next = strchr(line, '\n');
		line = next ? next + 1 : NULL;
	}
	free(tweaks);
	UnloadFileText(text);
}

static void favorites_free(void)
{
	for (int i=0; i<favorite_count; i++)
		free(favorites[i].tweaks);
	favorite_count = 0;
}

static int favorite_find(const char* path, int song)
{
	for (int i=0; i<favorite_count; i++) {
		if (favorites[i].song == song && strcmp(favorites[i].path, path) == 0)
			return i;
	}
	return -1;
}

// saves the current song with the current tweaks, or updates it if it's already there
static void favorite_add(Famicom* famicom)
{
	Famicom_nsf* nsf = &famicom->nsf;
	int index = favorite_find(famicom->rom_path, nsf->song);
	if (index < 0) {
		if (MAX_FAVORITES <= favorite_count) {
			snprintf(favorite_status, sizeof(favorite_status), "favorites are full");
			return;
		}
		index = favorite_count++;
		memset(&favorites[index], 0, sizeof(Favorite));
		snprintf(favorites[index].path, sizeof(favorites[index].path), "%s", famicom->rom_path);
		favorites[index].song = nsf->song;
		const char* label = nsf->track_label[nsf->song];
		const char* title = nsf->title[0] ? nsf->title : GetFileName(famicom->rom_path);
		if (label[0])
			snprintf(favorites[index].name, sizeof(favorites[index].name), "%s - %s", title, label);
		else
			snprintf(favorites[index].name, sizeof(favorites[index].name), "%s - song %d", title, nsf->song + 1);
		ui_need_text(favorites[index].name);
		snprintf(favorite_status, sizeof(favorite_status), "added");
	} else {
		snprintf(favorite_status, sizeof(favorite_status), "updated its effects");
	}
	free(favorites[index].tweaks);
	favorites[index].tweaks = tweaks_capture();
	favorites_save();
}

static void favorite_remove(int index)
{
	free(favorites[index].tweaks);
	memmove(&favorites[index], &favorites[index + 1], (favorite_count - index - 1) * sizeof(Favorite));
	favorite_count--;
	if (favorite_playing == index)
		favorite_playing = -1;
	else if (index < favorite_playing)
		favorite_playing--;
	favorites_save();
}

// loads the favorite's nsf if another one is in, picks the track and puts its tweaks on
static void favorite_play(Famicom* famicom, int index)
{
	Favorite* fav = &favorites[index];
	if (!famicom->nsf.active || strcmp(famicom->rom_path, fav->path) != 0) {
		char path[1024];
		snprintf(path, sizeof(path), "%s", fav->path);
		if (famicom_insert_cartridge(famicom, path) != 0 || !famicom->nsf.active) {
			snprintf(favorite_status, sizeof(favorite_status), "couldn't open %s", GetFileName(path));
			favorite_playing = -1;
			return;
		}
	}
	famicom_nsf_select(famicom, fav->song);
	tweaks_apply(fav->tweaks);
	favorite_playing = index;
	snprintf(favorite_status, sizeof(favorite_status), "playing favorite %d of %d", index + 1, favorite_count);
}

void favorites_panel(Famicom* famicom)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Favorites", NK_MAXIMIZED))
		return;
	if (famicom->nsf.active) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){ 0.5f, 0.5f });
		bool saved = favorite_find(famicom->rom_path, famicom->nsf.song) >= 0;
		if (nk_button_label(ctx, saved ? "Update favorite's effects" : "Add song to favorites"))
			favorite_add(famicom);
	} else {
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
	}
	nk_label(ctx, favorite_status, NK_TEXT_LEFT);
	int remove = -1, play = -1;
	for (int i=0; i<favorite_count; i++) {
		nk_layout_row(ctx, NK_DYNAMIC, UI_ROW + 2, 2, (float[]){ 0.88f, 0.12f });
		// the one playing, and the current song if it's a favorite, stand out
		bool current = favorite_playing == i || (famicom->nsf.active
			&& famicom->nsf.song == favorites[i].song && strcmp(famicom->rom_path, favorites[i].path) == 0);
		nk_bool selected = current;
		if (nk_selectable_label(ctx, favorites[i].name, NK_TEXT_LEFT, &selected))
			play = i;
		if (nk_button_symbol(ctx, NK_SYMBOL_X))
			remove = i;
	}
	if (favorite_count == 0) {
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		nk_label(ctx, "Favorite a song to keep it here with its effects", NK_TEXT_LEFT);
	}
	if (0 <= play)
		favorite_play(famicom, play);
	else if (0 <= remove)
		favorite_remove(remove);
	nk_tree_pop(ctx);
}

static const char* fds_side_name(int side)
{
	return TextFormat("Disk %d side %c", side / 2 + 1, side % 2 ? 'B' : 'A');
}

void disk_window(Famicom* famicom)
{
	Famicom_fds* d = &famicom->fds;
	if (nk_begin(ctx, "Disk", nk_rect(250, 570, 300, 175), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE)) {
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		if (0 < d->eject_timer)
			nk_label(ctx, TextFormat("Switching to %s...", fds_side_name(d->pending_side)), NK_TEXT_LEFT);
		else if (d->inserted < 0)
			nk_label(ctx, "Drive empty", NK_TEXT_LEFT);
		else
			nk_label(ctx, TextFormat("%s inserted%s", fds_side_name(d->inserted),
						 d->motor_on ? (d->read_mode ? ", reading" : ", writing") : ""), NK_TEXT_LEFT);

		nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
		if (nk_button_label(ctx, d->inserted < 0 ? "Insert" : "Eject")) {
			if (d->inserted < 0)
				fds_insert(famicom, 0 < d->eject_timer ? d->pending_side : 0);
			else
				fds_eject(famicom);
		}
		if (nk_button_label(ctx, "Next side")) {
			int current = d->inserted < 0 ? d->pending_side : d->inserted;
			fds_switch_side(famicom, (current + 1) % d->sides);
		}

		// picking a side ejects the current one first so the bios notices the change
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		const char* names[FDS_MAX_SIDES];
		char storage[FDS_MAX_SIDES][32];
		for (int s=0; s<d->sides; s++) {
			snprintf(storage[s], sizeof(storage[s]), "%s", fds_side_name(s));
			names[s] = storage[s];
		}
		int current = d->inserted < 0 ? d->pending_side : d->inserted;
		int picked = nk_combo(ctx, names, d->sides, current, UI_BUTTON, nk_vec2(200, 200));
		if (picked != current)
			fds_switch_side(famicom, picked);

		if (d->modified) {
			nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){0.6f, 0.4f});
			nk_label(ctx, "Disk has unsaved writes", NK_TEXT_LEFT);
			if (nk_button_label(ctx, "Save now"))
				fds_save_disk(famicom, famicom->rom_path);
		}
	}
	nk_end(ctx);
}

nk_bool nsf_auto_advance = true;

static double nsf_elapsed_ms(Famicom* famicom)
{
	return famicom->nsf.frames_played * 1000.0 / famicom_frame_rate(famicom);
}

// track length plus fade, or -1 when the file doesn't give one
static int nsf_track_total(Famicom_nsf* nsf, int* fade_out)
{
	int time = nsf->track_time[nsf->song];
	int fade = nsf->track_fade[nsf->song];
	if (fade < 0)
		fade = 1000; // nsfe leaves the default fade to the player
	if (fade_out)
		*fade_out = fade;
	return time < 0 ? -1 : time + fade;
}

// a render being stopped keeps going while the sound fades, on top of any fade the
// track is already doing, and saves once it's silent
static void export_fade_step(void)
{
	if (!exporting || !export_fading)
		return;
	double t = (export_frames - export_fade_start) / (export_fps * EXPORT_FADE_SECONDS);
	if (1 <= t) {
		export_stop();
		return;
	}
	audio_volume *= (float)(1 - t);
}

static void export_begin_fade(void)
{
	if (!exporting || export_fading)
		return;
	export_fading = true;
	export_fade_start = export_frames;
}

// fades out at the end of tracks with a known length and moves on to the next one
static void nsf_track_timing(Famicom* famicom)
{
	Famicom_nsf* nsf = &famicom->nsf;
	audio_volume = 1.0f;
	if (!nsf->active || famicom_nsf_halted(famicom) || !nsf_auto_advance)
		return;
	int fade;
	int total = nsf_track_total(nsf, &fade);
	if (total < 0)
		return;
	double ms = nsf_elapsed_ms(famicom);
	int time = total - fade;
	if (time <= ms && 0 < fade) {
		float v = 1.0f - (ms - time) / fade;
		audio_volume = v < 0 ? 0 : v;
	}
	if (total <= ms) {
		audio_volume = 1.0f;
		// a render ends with the track
		if (exporting) {
			famicom_nsf_stop(famicom);
			export_stop();
			return;
		}
		// playing through the favorites goes on to the next favorite instead
		if (0 <= favorite_playing) {
			if (favorite_playing + 1 < favorite_count)
				favorite_play(famicom, favorite_playing + 1);
			else {
				famicom_nsf_stop(famicom);
				favorite_playing = -1;
			}
			return;
		}
		bool last = nsf->playlist_length
			? nsf->playlist_position == nsf->playlist_length - 1
			: nsf->song == nsf->songs - 1;
		if (last)
			famicom_nsf_stop(famicom);
		else
			famicom_nsf_step(famicom, 1);
	}
}

static const char* format_ms(double ms)
{
	long seconds = ms / 1000;
	return TextFormat("%ld:%02ld", seconds / 60, seconds % 60);
}

void nsf_window(Famicom* famicom)
{
	Famicom_nsf* nsf = &famicom->nsf;
	if (nk_begin(ctx, "NSF", nk_rect(0, 580, 240, 215), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE)) {
		// with nothing loaded the player still offers a way in, and the favorites
		if (!nsf->active) {
			nk_layout_row_dynamic(ctx, UI_ROW, 1);
			nk_label(ctx, "No NSF loaded", NK_TEXT_LEFT);
			nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
			if (nk_button_label(ctx, "Load NSF..."))
				picker_show(famicom, picker_rom);
			favorites_panel(famicom);
			nk_end(ctx);
			return;
		}
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		nk_label(ctx, TextFormat("Title: %s", nsf->title), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("Artist: %s", nsf->artist), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("Copyright: %s", nsf->copyright), NK_TEXT_LEFT);
		if (nsf->ripper[0])
			nk_label(ctx, TextFormat("Ripper: %s", nsf->ripper), NK_TEXT_LEFT);
		// nsfe playlists number tracks in play order
		int number = nsf->playlist_length ? nsf->playlist_position + 1 : nsf->song + 1;
		int count = nsf->playlist_length ? nsf->playlist_length : nsf->songs;
		const char* label = nsf->track_label[nsf->song];
		if (label[0])
			nk_label(ctx, TextFormat("%d / %d: %s", number, count, label), NK_TEXT_LEFT);
		else
			nk_label(ctx, TextFormat("Song %d / %d", number, count), NK_TEXT_LEFT);
		const char* state = nsf->stopped ? "Stopped" : nsf->paused ? "Paused" : "Playing";
		char elapsed[16];
		snprintf(elapsed, sizeof(elapsed), "%s", format_ms(nsf_elapsed_ms(famicom)));
		int total = nsf_track_total(nsf, NULL);
		if (0 <= total)
			nk_label(ctx, TextFormat("%s   %s / %s", state, elapsed, format_ms(total)), NK_TEXT_CENTERED);
		else
			nk_label(ctx, TextFormat("%s   %s", state, elapsed), NK_TEXT_CENTERED);
		nk_checkbox_label(ctx, "Auto-advance at track end", &nsf_auto_advance);
		// fds and n163 play, anything else the tune uses is named here
		byte missing = nsf->expansion & ~NSF_SUPPORTED;
		if (missing) {
			char chips[64] = "";
			const char* names[6] = { "VRC6", "VRC7", "FDS", "MMC5", "N163", "5B" };
			for (int i=0; i<6; i++) {
				if (missing & (1 << i)) {
					if (chips[0])
						strncat(chips, ", ", sizeof(chips) - strlen(chips) - 1);
					strncat(chips, names[i], sizeof(chips) - strlen(chips) - 1);
				}
			}
			nk_label_colored(ctx, TextFormat("%s not emulated", chips), NK_TEXT_LEFT, nk_rgb(255, 190, 60));
		}
		nk_layout_row_dynamic(ctx, UI_BUTTON, 3);
		if (nk_button_symbol_label(ctx, NK_SYMBOL_TRIANGLE_RIGHT, "Play", NK_TEXT_RIGHT)) {
			if (nsf->stopped)
				famicom_nsf_select(famicom, nsf->song);
			nsf->paused = false;
		}
		if (nk_button_label(ctx, "|| Pause") && !nsf->stopped)
			nsf->paused = !nsf->paused;
		if (nk_button_symbol_label(ctx, NK_SYMBOL_RECT_SOLID, "Stop", NK_TEXT_RIGHT))
			famicom_nsf_stop(famicom);
		if (nk_button_symbol_label(ctx, NK_SYMBOL_TRIANGLE_LEFT, "Prev", NK_TEXT_RIGHT))
			famicom_nsf_step(famicom, -1);
		if (nk_button_label(ctx, "Restart"))
			famicom_nsf_select(famicom, nsf->song);
		if (nk_button_symbol_label(ctx, NK_SYMBOL_TRIANGLE_RIGHT, "Next", NK_TEXT_LEFT))
			famicom_nsf_step(famicom, 1);
		favorites_panel(famicom);
	}
	nk_end(ctx);
}

void ui(Famicom* famicom)
{
	ppu_window();
	palette_window(famicom);

	if (nk_begin(ctx, "Famicom", nk_rect(0, 0, 240, 300), NK_WINDOW_MINIMIZABLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE|NK_WINDOW_SCALABLE))
	{
		nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
		if (nk_button_label(ctx, "Save")) {
			famicom_save_state(famicom);
		}
		if (nk_button_label(ctx, "Load")) {
			famicom_load_state(famicom);
		}
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		nk_label(ctx, "CPU", NK_TEXT_ALIGN_CENTERED);
		nk_layout_row_dynamic(ctx, UI_ROW, 2);
		nk_label(ctx, TextFormat("PC: %X - %s", famicom->cpu.pc, famicom->cpu.current_instruction_name), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("A: %X", famicom->cpu.reg[reg_a]), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("X: %X", famicom->cpu.reg[reg_x]), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("Y: %X", famicom->cpu.reg[reg_y]), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("P: %X", famicom->cpu.reg[reg_p]), NK_TEXT_LEFT);
		nk_label(ctx, TextFormat("SP: %X", famicom->cpu.reg[reg_sp]), NK_TEXT_LEFT);
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		if (nk_button_label(ctx, "Reset")) {
			famicom_reset(famicom, true);
		}
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		// switching region is a power cycle on real hardware, so it restarts the game
		nk_layout_row(ctx, NK_DYNAMIC, UI_BUTTON, 2, (float[]){0.35f, 0.65f});
		nk_label(ctx, "Region", NK_TEXT_LEFT);
		const char* regions[4];
		char auto_name[32];
		snprintf(auto_name, sizeof(auto_name), "Auto (%s)", famicom_has_cartridge(famicom)
			 ? famicom_region_names[famicom->loaded_rom.region] : "NTSC");
		regions[0] = auto_name;
		for (int r=0; r<region_count; r++)
			regions[r + 1] = famicom_region_names[r];
		int region = nk_combo(ctx, regions, 4, famicom_region_setting, UI_BUTTON, nk_vec2(160, 160));
		if (region != famicom_region_setting) {
			famicom_region_setting = region;
			config_save();
			if (famicom_has_cartridge(famicom))
				famicom_reset(famicom, false);
		}
		nk_layout_row_dynamic(ctx, UI_ROW, 1);
		nk_label(ctx, "Cartridge", NK_TEXT_ALIGN_CENTERED);
		if (famicom_has_cartridge(famicom)) {
			nk_label(ctx, TextFormat("File: %s", GetFileName(famicom->rom_path)), NK_TEXT_LEFT);
			if (famicom->nsf.active)
				nk_label(ctx, "Mapper: NSF", NK_TEXT_LEFT);
			else
				nk_label(ctx, TextFormat("Mapper: %d", famicom->loaded_rom.mapper), NK_TEXT_LEFT);
		} else {
			nk_label(ctx, "File: (empty)", NK_TEXT_LEFT);
			nk_label(ctx, "Mapper: -", NK_TEXT_LEFT);
		}
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		nk_layout_row_dynamic(ctx, UI_BUTTON, 2);
		if (nk_button_label(ctx, "Load ROM..."))
			picker_show(famicom, picker_rom);
		if (nk_button_label(ctx, "Settings..."))
			config_open = true;
		nk_layout_row_dynamic(ctx, UI_BUTTON, 1);
		if (nk_button_label(ctx, "Controllers..."))
			controllers_open = true;
	}
	nk_end(ctx);

	picker_window(famicom);
	config_window(famicom);
	controllers_window();
	ppu_debug_window(famicom);

	nsf_window(famicom);

	apu_window(famicom);
	piano_window();
	if (famicom->fds.active && !famicom->fds.sound_only)
		disk_window(famicom);
}

int frame = 0;
int target_fps = 60;
bool screen_only = false;

static void toggle_screen_only()
{
	screen_only = !screen_only;
	ToggleBorderlessWindowed();
	// escape leaves the picture view instead of quitting while it's up
	if (screen_only) {
		SetExitKey(KEY_NULL);
		HideCursor();
	} else {
		SetExitKey(KEY_ESCAPE);
		ShowCursor();
	}
}

void graphics_draw(Famicom* famicom)
{
	// any characters on screen this frame that the atlas doesn't have yet
	ui_need_text(GetFileName(famicom->rom_path));
	ui_need_text(palette_name);
	if (famicom->nsf.active) {
		ui_need_text(famicom->nsf.title);
		ui_need_text(famicom->nsf.artist);
		ui_need_text(famicom->nsf.copyright);
		ui_need_text(famicom->nsf.ripper);
		ui_need_text(famicom->nsf.track_label[famicom->nsf.song]);
	}
	// changed settings go to disk once nothing is being dragged
	if (config_dirty && !IsMouseButtonDown(MOUSE_BUTTON_LEFT))
		config_save();
	// font changes wait for a frame boundary so no queued text uses the old glyphs
	if (ui_font_pending) {
		set_ui_font_size(ui_font_pending);
		ui_font_pending = 0;
		config_save();
	} else if (ui_font_rebuild) {
		set_ui_font_size(ui_font_size);
	}
	// a rom dropped on the window goes in the cartridge slot
	// palettes dropped on the window are loaded too
	if (IsFileDropped()) {
		FilePathList dropped = LoadDroppedFiles();
		if (0 < dropped.count) {
			if (IsFileExtension(dropped.paths[0], ".pal"))
				config_dirty |= palette_use_file(dropped.paths[0]) == 0;
			else if (famicom_insert_cartridge(famicom, dropped.paths[0]) == 0)
				picker_close();
		}
		UnloadDroppedFiles(dropped);
	}
	// alt+enter switches to just the picture, fullscreen, and back; escape also leaves
	bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
	if ((alt && IsKeyPressed(KEY_ENTER)) || (screen_only && IsKeyPressed(KEY_ESCAPE)))
		toggle_screen_only();

	// the region setting can change under a running game, or arrive after a rom from
	// the command line was loaded, either way the machine restarts in the right timing
	if (famicom_has_cartridge(famicom)) {
		int region = famicom_region_setting == region_auto ? famicom->loaded_rom.region : famicom_region_setting - 1;
		if (region != famicom->region)
			famicom_reset(famicom, false);
	}
	// pal and dendy run at 50 frames a second
	// rendering a video runs flat out, the window only redraws now and then
	int fps = famicom_has_cartridge(famicom) && famicom->region != region_ntsc ? 50 : 60;
	if (exporting)
		fps = 0;
	if (fps != target_fps) {
		target_fps = fps;
		SetTargetFPS(fps);
	}

	if (famicom->nsf.active && !input_rebinding()) {
		if (IsKeyPressed(KEY_LEFT))
			famicom_nsf_step(famicom, -1);
		if (IsKeyPressed(KEY_RIGHT))
			famicom_nsf_step(famicom, 1);
		if (IsKeyPressed(KEY_SPACE)) {
			if (famicom->nsf.stopped)
				famicom_nsf_select(famicom, famicom->nsf.song);
			else
				famicom->nsf.paused = !famicom->nsf.paused;
		}
	}
	if (famicom->cpu.running && !famicom_nsf_halted(famicom)) {
		//input
		// nothing reaches the game while a binding is waiting for its key
		if (input_rebinding()) {
			memset(&famicom->controller_p1, 0, sizeof(famicom->controller_p1));
			memset(&famicom->controller_p2, 0, sizeof(famicom->controller_p2));
		} else {
			input_poll(&famicom->controller_p1, 0, alt);
			input_poll(&famicom->controller_p2, 1, alt);
		}
		// rendering a video isn't tied to real time, so it fits in as many frames as it can
		static double last_loop = 0;
		double start = GetTime();
		int frames = 0;
		do {
			// run until the ppu finishes a frame
			famicom->ppu.frame_complete = false;
			while (!famicom->ppu.frame_complete) {
				if (!famicom_step(famicom, false, NULL))
					break;
			}
			if (famicom->nsf.active)
				famicom->nsf.frames_played++;
			piano_sample(famicom);
			frames++;
			// a video frame for every emulated one keeps it in step with the recorded sound
			if (exporting) {
				export_frame(famicom);
				nsf_track_timing(famicom);
				export_fade_step();
			}
		} while (exporting && famicom->cpu.running && !famicom_nsf_halted(famicom) && GetTime() - start < 0.1);
		if (exporting && last_loop > 0) {
			double elapsed = GetTime() - last_loop;
			if (elapsed > 0)
				export_speed = export_speed * 0.8 + 0.2 * (frames / (elapsed * export_fps));
		}
		last_loop = exporting ? GetTime() : 0;
	} else if (export_fading) {
		// nothing's playing to fade, so there's nothing to wait for
		export_stop();
	}
	// picking another track by hand ends playing through the favorites
	if (0 <= favorite_playing && (favorite_count <= favorite_playing
	    || famicom->nsf.song != favorites[favorite_playing].song
	    || strcmp(famicom->rom_path, favorites[favorite_playing].path) != 0))
		favorite_playing = -1;
	if (!exporting)
		nsf_track_timing(famicom);
	// rebuilt every frame so palette changes show up while paused too
	update_framebuffer(famicom);
	ntsc_render(famicom);

	if (screen_only) {
		// just the picture, as large as fits with the right shape, centred on black
		Texture2D picture = (ntsc_ready && ntsc_enabled && ntsc_target.id) ? ntsc_target.texture : ppu_fb_tex.texture;
		float sw = GetScreenWidth(), sh = GetScreenHeight();
		float scale = sw / 256 < sh / 240 ? sw / 256 : sh / 240;
		Rectangle dest = { (sw - 256 * scale) / 2, (sh - 240 * scale) / 2, 256 * scale, 240 * scale };
		BeginDrawing();
		ClearBackground(BLACK);
		DrawTexturePro(picture, (Rectangle){ 0, 0, picture.width, picture.height }, dest, (Vector2){ 0, 0 }, 0, WHITE);
		EndDrawing();
		frame++;
		return;
	}

	// nuklear, the ui settles the scope size before the scopes are drawn
	ui(famicom);
	// the onscreen scopes and piano roll rest while a video renders, it goes faster
	if (!exporting) {
		draw_scope();
		piano_draw();
	}
	UpdateNuklear(ctx);

	// raylib draw
	BeginDrawing();
	ClearBackground(background_color);
	DrawNuklear(ctx);
	EndDrawing();
	frame++;
}
