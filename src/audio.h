#define AUDIO_SAMPLE_RATE 44100

enum audio_channel {
	channel_pulse1,
	channel_pulse2,
	channel_triangle,
	channel_noise,
	channel_dmc,
	channel_fds,
	channel_n163,
	channel_vrc6,
	channel_vrc7,
	channel_count,
};

extern bool audio_mute[channel_count]; // muted channels still show on the scopes

// shapes the pulse and triangle channels can play instead of their own
enum audio_waveform {
	wave_default,
	wave_square,
	wave_triangle,
	wave_sine,
	wave_saw,
	wave_sample, // a loaded .wav, played as one cycle
	wave_count,
};

enum audio_wave_channel {
	wave_pulse1,
	wave_pulse2,
	wave_triangle_channel,
	wave_channel_count,
};

#define EQ_BANDS 5 // low shelf, three peaks, high shelf

typedef struct audio_effects {
	bool echo;
	float echo_time; // ms
	float echo_feedback; // 0..0.95
	float echo_mix; // 0..1
	bool echo_pingpong; // repeats bounce between the speakers
	bool eq;
	float eq_freq[EQ_BANDS]; // Hz
	float eq_gain[EQ_BANDS]; // dB
	bool limiter;
	float limiter_threshold; // dB
	bool chorus;
	float chorus_mix; // 0..1
	float chorus_rate; // Hz
	float chorus_depth; // ms
	bool chorus_channels[channel_count];
	bool reverb;
	float reverb_mix; // 0..1
	float reverb_size; // 0..1
	float reverb_damping; // 0..1
	bool widen;
	float width; // 1 leaves the stereo image alone
	float surround; // 0..1, adds a delayed decorrelated side signal
	int waveform[wave_channel_count];
	bool follow_duty; // squeeze the other waveforms by the pulse's duty, like pulse width
} Audio_effects;

extern Audio_effects audio_fx;
extern const Audio_effects audio_fx_defaults;
extern const char* audio_waveform_names[wave_count];
extern const char* audio_eq_band_names[EQ_BANDS];
#define EQ_MAX_GAIN 12.0f
float audio_eq_response(float frequency); // the eq's total boost or cut in dB at a frequency

// sample waveforms: data is copied, NULL clears; levels are normalised to 0..1
#define AUDIO_MAX_WAVE_SAMPLES 65536
void audio_set_wave_sample(int wave_channel, const float* data, int length);
bool audio_has_wave_sample(int wave_channel);

// mixer: a gain and an octave shift for each channel
#define AUDIO_MAX_TRANSPOSE 3
extern float audio_gain[channel_count];
extern float audio_master_volume; // 0..2, the very last step before the speakers
extern int audio_transpose[channel_count];
void audio_reset_tweaks(void);

// global pitch shift in semitones, on top of each channel's octave
#define AUDIO_MAX_PITCH_SHIFT 12.0f
extern float audio_pitch_shift;

// each channel's frequency multiplier from its octave and the global shift, kept
// up to date by the apu so the hot paths don't redo the maths every cycle
extern double audio_pitch_scale[channel_count];
void audio_update_pitch(void); // refreshes audio_pitch_scale after a pitch setting changes

// the same as a frequency scale, for chips that add a frequency every step
static inline double audio_transpose_scale(int channel)
{
	return audio_pitch_scale[channel];
}

// a channel's period with its pitch changes applied, rounded; periods count from 1
static inline int audio_transposed_period(int channel, int period)
{
	int p = (int)(period / audio_pitch_scale[channel] + 0.5);
	return p < 1 ? 1 : p;
}

// for a timer reloading: carries the fraction that doesn't fit a whole cycle over to
// the next reload, so fine pitch shifts come out right on average
static inline int audio_reload_period(int channel, int period, double* error)
{
	double exact = period / audio_pitch_scale[channel] + *error;
	int p = (int)exact;
	if (p < 1)
		p = 1;
	*error = exact - p;
	if (*error < -1 || 1 < *error)
		*error = 0;
	return p;
}

void apu_reset(Famicom_apu* apu);
void apu_write(Famicom* f, word addr, byte value);
byte apu_read_status(Famicom* f);
void apu_step(Famicom* f);
bool apu_irq(Famicom* f);
void audio_pull(float* out, unsigned int count);

// records what's heard to a 16 bit stereo .wav, for video export; stop writes the header
// with the sample rate given, so the caller can match it to how long the video ran
bool audio_record_start(const char* path);
unsigned long audio_record_stop(int sample_rate);
unsigned long audio_record_frames(void);

// while rendering a video nothing goes to the speakers, so emulation can run flat out
extern bool audio_output_muted;
void audio_clear_effects(void); // empties the echo, reverb and other effect tails
void audio_scope(float* out, int count);
void audio_scope_channel(int channel, float* out, int count);

// what each voice is playing right now, for the piano roll; volume 0 means silent
typedef struct audio_voice {
	int channel;
	float frequency; // hz
	float volume; // 0..1
} Audio_voice;
#define AUDIO_MAX_VOICES 23 // 2A03, fds, eight n163, three vrc6 and six vrc7 channels
int audio_voices(Famicom* f, Audio_voice voices[AUDIO_MAX_VOICES]);

// every write to a sound register, in order, for the video export's register view
enum audio_chip { chip_2a03, chip_fds, chip_n163, chip_vrc6, chip_vrc7, chip_count };
typedef struct audio_write {
	word reg; // the cpu address, or the chip's own register for n163 ram and vrc7
	byte value;
	byte chip;
	signed char channel; // whose colour it takes, -1 for registers shared by the whole chip
} Audio_write;
#define AUDIO_WRITE_LOG 8192
void audio_log_write(int chip, int channel, word reg, byte value);
// the writes since the last call, oldest first; if more than max came, the newest max
int audio_take_writes(Audio_write* out, int max);

#define APU_REGISTER_COUNT 0x18 // $4000-$4017
extern bool audio_perfect_triangle;
extern float audio_volume; // master volume, the nsf player fades tracks out with it
void audio_take_register_writes(byte values[APU_REGISTER_COUNT], int writes[APU_REGISTER_COUNT]);
