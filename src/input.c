#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <raylib.h>
#include "types.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "input.h"

Input_player input_players[INPUT_PLAYERS];
const char* nes_button_names[nes_button_count] = { "Up", "Down", "Left", "Right", "Select", "Start", "B", "A" };

void input_defaults(int player)
{
	Input_player* p = &input_players[player];
	memset(p, 0, sizeof(*p));
	// player one plays on the keyboard as before, player two has no keys to start with
	if (player == 0) {
		int keys[nes_button_count] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_S, KEY_ENTER, KEY_Z, KEY_X };
		memcpy(p->keys, keys, sizeof(keys));
	}
	// the nes pad's b and a sit where an xbox pad's a and b are, at the bottom and right
	int buttons[nes_button_count] = {
		GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
		GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
	};
	for (int b=0; b<nes_button_count; b++)
		p->pad[b] = (Pad_binding){ pad_button, buttons[b] };
	p->gamepad = player;
	p->stick = true;
}

#define STICK_THRESHOLD 0.5f

static bool pad_down(int gamepad, Pad_binding b)
{
	switch (b.kind) {
	case pad_button:
		return IsGamepadButtonDown(gamepad, b.code);
	case pad_axis_plus:
		return GetGamepadAxisMovement(gamepad, b.code) > STICK_THRESHOLD;
	case pad_axis_minus:
		return GetGamepadAxisMovement(gamepad, b.code) < -STICK_THRESHOLD;
	}
	return false;
}

void input_poll(Famicom_controller* c, int player, bool alt_held)
{
	Input_player* p = &input_players[player];
	bool pad = IsGamepadAvailable(p->gamepad);
	bool down[nes_button_count];
	for (int b=0; b<nes_button_count; b++) {
		int key = p->keys[b];
		down[b] = key && IsKeyDown(key) && !(alt_held && key == KEY_ENTER);
		if (pad)
			down[b] |= pad_down(p->gamepad, p->pad[b]);
	}
	if (pad && p->stick) {
		float x = GetGamepadAxisMovement(p->gamepad, GAMEPAD_AXIS_LEFT_X);
		float y = GetGamepadAxisMovement(p->gamepad, GAMEPAD_AXIS_LEFT_Y);
		down[nes_left] |= x < -STICK_THRESHOLD;
		down[nes_right] |= STICK_THRESHOLD < x;
		down[nes_up] |= y < -STICK_THRESHOLD;
		down[nes_down] |= STICK_THRESHOLD < y;
	}
	// a real pad can't press opposite directions at once, and some games break if it does
	if (down[nes_up] && down[nes_down])
		down[nes_up] = down[nes_down] = false;
	if (down[nes_left] && down[nes_right])
		down[nes_left] = down[nes_right] = false;
	c->up = down[nes_up];
	c->down = down[nes_down];
	c->left = down[nes_left];
	c->right = down[nes_right];
	c->select = down[nes_select];
	c->start = down[nes_start];
	c->button_b = down[nes_b];
	c->button_a = down[nes_a];
}

const char* input_key_name(int key)
{
	static char name[16];
	if (key == 0)
		return "-";
	if ((KEY_A <= key && key <= KEY_Z) || (KEY_ZERO <= key && key <= KEY_NINE)) {
		snprintf(name, sizeof(name), "%c", key);
		return name;
	}
	if (KEY_F1 <= key && key <= KEY_F12) {
		snprintf(name, sizeof(name), "F%d", key - KEY_F1 + 1);
		return name;
	}
	if (KEY_KP_0 <= key && key <= KEY_KP_9) {
		snprintf(name, sizeof(name), "Num %d", key - KEY_KP_0);
		return name;
	}
	switch (key) {
	case KEY_SPACE: return "Space";
	case KEY_ENTER: return "Enter";
	case KEY_TAB: return "Tab";
	case KEY_BACKSPACE: return "Backspace";
	case KEY_INSERT: return "Insert";
	case KEY_HOME: return "Home";
	case KEY_END: return "End";
	case KEY_PAGE_UP: return "Page up";
	case KEY_PAGE_DOWN: return "Page down";
	case KEY_UP: return "Up";
	case KEY_DOWN: return "Down";
	case KEY_LEFT: return "Left";
	case KEY_RIGHT: return "Right";
	case KEY_LEFT_SHIFT: return "Left shift";
	case KEY_RIGHT_SHIFT: return "Right shift";
	case KEY_LEFT_CONTROL: return "Left ctrl";
	case KEY_RIGHT_CONTROL: return "Right ctrl";
	case KEY_LEFT_ALT: return "Left alt";
	case KEY_RIGHT_ALT: return "Right alt";
	case KEY_CAPS_LOCK: return "Caps lock";
	case KEY_APOSTROPHE: return "'";
	case KEY_COMMA: return ",";
	case KEY_MINUS: return "-";
	case KEY_PERIOD: return ".";
	case KEY_SLASH: return "/";
	case KEY_SEMICOLON: return ";";
	case KEY_EQUAL: return "=";
	case KEY_LEFT_BRACKET: return "[";
	case KEY_BACKSLASH: return "\\";
	case KEY_RIGHT_BRACKET: return "]";
	case KEY_GRAVE: return "`";
	case KEY_KP_DECIMAL: return "Num .";
	case KEY_KP_DIVIDE: return "Num /";
	case KEY_KP_MULTIPLY: return "Num *";
	case KEY_KP_SUBTRACT: return "Num -";
	case KEY_KP_ADD: return "Num +";
	case KEY_KP_ENTER: return "Num enter";
	}
	snprintf(name, sizeof(name), "Key %d", key);
	return name;
}

const char* input_pad_name(Pad_binding b)
{
	static const char* buttons[] = {
		"-", "D-pad up", "D-pad right", "D-pad down", "D-pad left",
		"Y / Triangle", "B / Circle", "A / Cross", "X / Square",
		"LB / L1", "LT / L2", "RB / R1", "RT / R2",
		"Back / Select", "Guide", "Start", "Left stick press", "Right stick press",
	};
	static const char* axes[] = { "Left stick", "Left stick", "Right stick", "Right stick", "LT axis", "RT axis" };
	static char name[32];
	switch (b.kind) {
	case pad_button:
		if (0 <= b.code && b.code < (int)(sizeof(buttons) / sizeof(buttons[0])))
			return buttons[b.code];
		snprintf(name, sizeof(name), "Button %d", b.code);
		return name;
	case pad_axis_plus:
	case pad_axis_minus: {
		bool plus = b.kind == pad_axis_plus;
		if (b.code == GAMEPAD_AXIS_LEFT_TRIGGER || b.code == GAMEPAD_AXIS_RIGHT_TRIGGER)
			return axes[b.code];
		if (0 <= b.code && b.code < 4) {
			bool x = b.code == GAMEPAD_AXIS_LEFT_X || b.code == GAMEPAD_AXIS_RIGHT_X;
			snprintf(name, sizeof(name), "%s %s", axes[b.code], x ? (plus ? "right" : "left") : (plus ? "down" : "up"));
			return name;
		}
		snprintf(name, sizeof(name), "Axis %d%c", b.code, plus ? '+' : '-');
		return name;
	}
	}
	return "-";
}

// triggers rest at -1, so axes are judged against where they were when rebinding began
#define CAPTURE_AXES 6
static float capture_rest[CAPTURE_AXES];

void input_capture_begin(int gamepad)
{
	for (int a=0; a<CAPTURE_AXES; a++)
		capture_rest[a] = IsGamepadAvailable(gamepad) ? GetGamepadAxisMovement(gamepad, a) : 0;
}

bool input_capture_key(int* key)
{
	for (int k=KEY_SPACE; k<=KEY_KB_MENU; k++) {
		if (IsKeyPressed(k)) {
			*key = k;
			return true;
		}
	}
	return false;
}

bool input_capture_pad(int gamepad, Pad_binding* out)
{
	if (!IsGamepadAvailable(gamepad))
		return false;
	for (int b=GAMEPAD_BUTTON_LEFT_FACE_UP; b<=GAMEPAD_BUTTON_RIGHT_THUMB; b++) {
		if (IsGamepadButtonPressed(gamepad, b)) {
			*out = (Pad_binding){ pad_button, b };
			return true;
		}
	}
	int axes = GetGamepadAxisCount(gamepad);
	for (int a=0; a<axes && a<CAPTURE_AXES; a++) {
		float v = GetGamepadAxisMovement(gamepad, a);
		if (0.6f < fabsf(v - capture_rest[a])) {
			*out = (Pad_binding){ v > capture_rest[a] ? pad_axis_plus : pad_axis_minus, a };
			return true;
		}
	}
	return false;
}

void input_config_write(char* text, size_t size)
{
	for (int p=0; p<INPUT_PLAYERS; p++) {
		Input_player* pl = &input_players[p];
		size_t used = strlen(text);
		snprintf(text + used, size - used, "pad_%d_gamepad=%d\npad_%d_stick=%d\n", p, pl->gamepad, p, pl->stick ? 1 : 0);
		for (int b=0; b<nes_button_count; b++) {
			used = strlen(text);
			snprintf(text + used, size - used, "pad_%d_%d=%d,%d,%d\n", p, b, pl->keys[b], pl->pad[b].kind, pl->pad[b].code);
		}
	}
}

bool input_config_line(const char* line)
{
	int p, b, key, kind, code, value;
	if (sscanf(line, "pad_%d_gamepad=%d", &p, &value) == 2) {
		if (0 <= p && p < INPUT_PLAYERS && 0 <= value && value < INPUT_GAMEPADS)
			input_players[p].gamepad = value;
		return true;
	}
	if (sscanf(line, "pad_%d_stick=%d", &p, &value) == 2) {
		if (0 <= p && p < INPUT_PLAYERS)
			input_players[p].stick = value != 0;
		return true;
	}
	if (sscanf(line, "pad_%d_%d=%d,%d,%d", &p, &b, &key, &kind, &code) == 5) {
		if (0 <= p && p < INPUT_PLAYERS && 0 <= b && b < nes_button_count && 0 <= key && key < 512
		    && pad_none <= kind && kind <= pad_axis_minus) {
			input_players[p].keys[b] = key;
			input_players[p].pad[b] = (Pad_binding){ kind, code };
		}
		return true;
	}
	return false;
}
