typedef struct famicom_debug {
	bool nmi;
	bool irq;
} Famicom_debug;

enum famicom_region {
	region_ntsc,
	region_pal,
	region_dendy, // pal timing with ntsc style vblank and apu, used by many famiclones
	region_count,
};

// the region setting, auto follows the rom header
enum famicom_region_setting {
	region_auto,
	region_force_ntsc,
	region_force_pal,
	region_force_dendy,
};

typedef struct famicom_rom {
	char* name;
	int mapper;
	int mirroring;
	int region; // what the header says the game is for
} Famicom_rom;

typedef struct famicom_envelope {
	bool start;
	bool loop;
	bool constant;
	byte volume;
	byte divider;
	byte decay;
} Famicom_envelope;

typedef struct famicom_pulse {
	byte duty;
	byte sequence;
	word timer_period;
	word timer;
	byte length;
	Famicom_envelope envelope;
	bool sweep_enabled;
	bool sweep_negate;
	bool sweep_reload;
	byte sweep_period;
	byte sweep_shift;
	byte sweep_divider;
	bool ones_complement; // pulse 1 negates with ones' complement
} Famicom_pulse;

typedef struct famicom_triangle {
	bool control;
	bool linear_reload_flag;
	byte linear_reload;
	byte linear_counter;
	word timer_period;
	word timer;
	byte sequence;
	byte length;
} Famicom_triangle;

typedef struct famicom_noise {
	bool mode;
	word timer_period;
	word timer;
	word shift;
	byte length;
	Famicom_envelope envelope;
} Famicom_noise;

typedef struct famicom_dmc {
	bool irq_enable;
	bool loop;
	word rate;
	word timer;
	byte output;
	word sample_address;
	word sample_length;
	word current_address;
	word bytes_remaining;
	byte sample_buffer;
	bool buffer_empty;
	byte shift;
	byte bits_remaining;
	bool silence;
} Famicom_dmc;

typedef struct famicom_apu {
	Famicom_pulse pulse1;
	Famicom_pulse pulse2;
	Famicom_triangle triangle;
	Famicom_noise noise;
	Famicom_dmc dmc;
	bool enable[5];
	bool five_step;
	bool irq_inhibit;
	bool frame_irq;
	bool dmc_irq;
	long frame_cycle;
	long cycle;
	// resampling and output filters
	double sample_sum;
	double chorus_sum; // the channels picked for the chorus, mixed on their own
	float channel_sum[12]; // per channel_count in audio.h
	int sample_count;
	double sample_clock;
	float hp1_in, hp1_out;
	float hp2_in, hp2_out;
	float lp_out;
} Famicom_apu;

#define MAPPER_NSF 0x10000 // not a real board, the nsf player's bank switching
#define MAPPER_FDS 0x10001 // famicom disk system ram adapter
#define NSF_FDS_RAM 0xA000 // fds nsfs get ram from $6000 to $FFFF

// nsf expansion chip bits
#define NSF_VRC6 0x01
#define NSF_VRC7 0x02
#define NSF_FDS 0x04
#define NSF_MMC5 0x08
#define NSF_N163 0x10
#define NSF_5B 0x20
#define NSF_SUPPORTED (NSF_VRC6 | NSF_VRC7 | NSF_FDS | NSF_N163)

#define FDS_MAX_SIDES 8
#define FDS_SIDE_BYTES 65500 // one side in a .fds file, no gaps or crcs
#define FDS_RAW_SIDE 0x18000 // room for a side once the drive's gaps and crcs are added

typedef struct fds_envelope {
	byte speed;
	byte gain;
	bool increase;
	bool off;
	int timer;
} Fds_envelope;

typedef struct famicom_fds {
	bool active;
	bool sound_only; // an nsf using the fds sound chip, no drive or bios
	int sides;
	int side_length[FDS_MAX_SIDES]; // raw bytes per side, gaps included
	int inserted; // side in the drive, -1 when empty
	int pending_side; // side to put in once the eject timer runs out
	long eject_timer; // cpu cycles left with the drive empty while switching sides
	bool modified; // the game wrote to the disk
	// drive
	bool disk_enabled;
	bool sound_enabled;
	bool motor_on;
	bool reset_transfer;
	bool read_mode;
	bool crc_control;
	bool disk_ready;
	bool disk_irq_enabled;
	bool scanning;
	bool end_of_head;
	bool gap_ended;
	bool previous_crc_control;
	bool transfer_complete;
	int position;
	int delay;
	word crc;
	byte read_data;
	byte write_data;
	bool timer_irq;
	bool disk_irq;
	bool irq_enabled;
	bool irq_repeat;
	word irq_reload;
	word irq_counter;
	// sound
	byte wave[64];
	bool wave_write;
	byte master_volume;
	bool halt_wave;
	bool disable_envelopes;
	int wave_position;
	word wave_overflow;
	word frequency;
	byte master_speed;
	Fds_envelope volume;
	Fds_envelope mod_envelope;
	byte mod_table[64];
	int mod_position;
	int mod_counter;
	word mod_frequency;
	bool mod_disabled;
	word mod_overflow;
	int mod_output;
	int output; // 0-63
	float filtered;
} Famicom_fds;
#define NSF_IDLE_ADDRESS 0x5FF0 // the player parks the cpu in a loop here between calls, clear of the n163 port

typedef struct vrc6_pulse {
	bool enabled;
	bool constant; // mode bit, ignores the duty and outputs the volume
	byte duty;
	byte volume;
	word period;
	int timer;
	byte step;
} Vrc6_pulse;

typedef struct famicom_vrc6 {
	bool active;
	Vrc6_pulse pulse[2];
	bool saw_enabled;
	byte saw_rate;
	word saw_period;
	int saw_timer;
	byte saw_step;
	byte saw_accumulator;
	bool halt; // $9003 bit 0
	int shift; // $9003 frequency scaling
	int output; // 0..61
} Famicom_vrc6;

typedef struct vrc7_operator {
	double phase; // in cycles
	float envelope; // attenuation in dB, 48 and up is silent
	int state;
	float out[2]; // the modulator's last two outputs, for feedback
} Vrc7_operator;

typedef struct famicom_vrc7 {
	bool active;
	bool silenced; // $E000 bit 6
	byte address;
	byte regs[0x40];
	bool key[6];
	Vrc7_operator op[6][2];
	double clock; // runs off its own 3.58MHz crystal, not the cpu
	double am_phase;
	double vib_phase;
	float output;
} Famicom_vrc7;

typedef struct famicom_n163 {
	bool active;
	byte ram[128]; // wave samples and channel registers
	byte address;
	bool auto_increment;
	bool sound_disabled;
	int channel; // the one the chip updates next, it serves one every 15 cycles
	int timer;
	int channel_output[8];
	float output; // -1..1, averaged over the enabled channels
	// mapper 19
	word irq_counter;
	bool irq_enabled;
	bool irq_pending;
	byte chr_regs[8];
	byte nt_regs[4];
	bool ciram_low_disabled;
	bool ciram_high_disabled;
} Famicom_n163;

#define NSF_MAX_TRACKS 256

typedef struct famicom_nsf {
	bool active;
	bool bankswitched;
	bool nsfe;
	char title[128];
	char artist[128];
	char copyright[128];
	char ripper[128];
	// nsfe extras, times are milliseconds and -1 when the file doesn't say
	char track_label[NSF_MAX_TRACKS][64];
	int track_time[NSF_MAX_TRACKS];
	int track_fade[NSF_MAX_TRACKS];
	byte playlist[NSF_MAX_TRACKS];
	int playlist_length;
	int playlist_position;
	int songs;
	int song; // 0 based
	word load_address;
	word init_address;
	word play_address;
	byte bank_init[8];
	byte expansion; // expansion sound chips, not emulated
	long play_period; // cpu cycles between play calls
	word speed[region_count]; // microseconds between play calls for each region
	long next_play;
	bool paused;
	bool stopped;
	long frames_played;
} Famicom_nsf;

typedef struct famicom_mapper {
	int id;
	int mirroring;
	int prg_banks[4]; // byte offsets into prg for each 8KB slot at $8000
	int chr_banks[8]; // byte offsets into chr for each 1KB slot at $0000
	byte regs[8];
	byte shift; // mmc1 shift register
	byte bank_select; // mmc3
	byte irq_latch;
	byte irq_counter;
	bool irq_reload;
	bool irq_enable;
	bool irq_pending;
	bool irq_enable_after_ack; // konami vrc irq
	bool irq_cycle_mode;
	int irq_prescaler;
	int irq_cycles; // mapper 40 cpu cycle counter
	int prg_ram_bank; // byte offset into prg mapped at $6000, -1 for prg ram
	byte latch[2]; // mmc2/mmc4 chr latches
	int nsf_banks[8]; // byte offsets into prg for each 4KB slot at $8000
} Famicom_mapper;

enum famicom_joypad_buttons {
	joypad_a,
	joypad_b,
	joypad_select,
	joypad_start,
	joypad_up,
	joypad_down,
	joypad_left,
	joypad_right,
};

typedef struct famicom_controller {
	bool up;
	bool down;
	bool left;
	bool right;
	bool button_a;
	bool button_b;
	bool select;
	bool start;
} Famicom_controller;

typedef struct famicom {
	Cpu_6502 cpu;
	Famicom_ppu ppu;
	Famicom_apu apu;
	Famicom_debug debug;
	Famicom_rom loaded_rom;
	Famicom_mapper mapper;
	int prg_size;
	int chr_size;
	bool chr_is_ram;
	byte* mem;
	byte* prg;
	byte* prg_ram;
	byte* chr;
	Famicom_controller controller_p1;
	Famicom_controller controller_p2;
	bool controller_strobe;
	byte controller_shift[2];
	long cpu_cycles;
	int dma_stall; // cycles the cpu is halted for oam/dmc dma
	char rom_path[1024];
	int region; // the timing actually being emulated
	int ppu_phase; // fifths of a dot, pal runs 3.2 ppu dots per cpu cycle
	Famicom_nsf nsf;
	Famicom_fds fds;
	Famicom_n163 n163;
	Famicom_vrc6 vrc6;
	Famicom_vrc7 vrc7;
	byte* fds_disk; // FDS_RAW_SIDE bytes per side, kept out of Famicom_fds so saves don't hold pointers
	byte* nsf_ram; // $6000-$FFFF for fds nsfs, which run from ram
} Famicom;

int famicom_create (Famicom* famicom);
void famicom_reset (Famicom* famicom, bool warm);
void famicom_destroy (Famicom* famicom);
int famicom_step(Famicom* famicom, bool debug, FILE* dfh);
void famicom_save_state(Famicom* famicom);
void famicom_load_state(Famicom* famicom);
int famicom_load_rom (Famicom* famicom, FILE* rom);
int famicom_insert_cartridge (Famicom* famicom, const char* path);
bool famicom_has_cartridge (Famicom* famicom);
extern int famicom_region_setting;
extern const char* famicom_region_names[region_count];
double famicom_cpu_hz (Famicom* famicom);
double famicom_frame_rate (Famicom* famicom);
int famicom_vblank_line (Famicom* famicom);
int famicom_last_line (Famicom* famicom);
void famicom_apply_region (Famicom* famicom);
void famicom_nsf_select (Famicom* famicom, int song);
void famicom_nsf_step (Famicom* famicom, int direction);
void famicom_nsf_stop (Famicom* famicom);
bool famicom_nsf_halted (Famicom* famicom);
byte mmap_famicom(Famicom* f, word addr, byte value, bool write);
