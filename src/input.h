// the joypads: each nes button can have a key and a gamepad input, for two players

enum nes_button { nes_up, nes_down, nes_left, nes_right, nes_select, nes_start, nes_b, nes_a, nes_button_count };

enum pad_kind { pad_none, pad_button, pad_axis_plus, pad_axis_minus };

typedef struct pad_binding {
	int kind;
	int code; // a raylib gamepad button, or an axis
} Pad_binding;

typedef struct input_player {
	int keys[nes_button_count]; // raylib key codes, 0 for none
	Pad_binding pad[nes_button_count];
	int gamepad; // which connected gamepad, 0-3
	bool stick; // the left stick steers too
} Input_player;

#define INPUT_PLAYERS 2
#define INPUT_GAMEPADS 4
extern Input_player input_players[INPUT_PLAYERS];
extern const char* nes_button_names[nes_button_count];

void input_defaults(int player);
// reads one player's bindings into the controller; alt+enter is the fullscreen key, not start
void input_poll(Famicom_controller* c, int player, bool alt_held);

const char* input_key_name(int key);
const char* input_pad_name(Pad_binding b);

// rebinding: call every frame while waiting, true once something was pressed
void input_capture_begin(int gamepad);
bool input_capture_key(int* key);
bool input_capture_pad(int gamepad, Pad_binding* out);

// config lines; write appends to text, line returns true if it was one of ours
void input_config_write(char* text, size_t size);
bool input_config_line(const char* line);
