#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/6502.h"
#include "chips/2C02.h"
#include "systems/famicom.h"
#include "chips/vrc7.h"
#include "audio.h"

// the running machine's cpu clock, which depends on the region
static double cpu_hz = 1789773.0;
#define CPU_HZ cpu_hz
#define RING_SIZE 16384
#define RING_TARGET 2048 // samples of latency the rate control aims for
#define SCOPE_SIZE 4096

static const byte length_table[32] = {
	10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
	12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

static const byte duty_table[4][8] = {
	{0, 1, 0, 0, 0, 0, 0, 0},
	{0, 1, 1, 0, 0, 0, 0, 0},
	{0, 1, 1, 1, 1, 0, 0, 0},
	{1, 0, 0, 1, 1, 1, 1, 1},
};

static const byte triangle_table[32] = {
	15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

// in cpu cycles, ntsc then pal; dendy uses the ntsc ones at its own clock
static const word noise_tables[2][16] = {
	{ 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 },
	{ 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 },
};

static const word dmc_tables[2][16] = {
	{ 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 },
	{ 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 },
};
#define noise_table noise_tables[0]
#define dmc_table dmc_tables[0]

// frame counter steps: quarter, half, quarter, end of the 4 step sequence, end of 5 step
static const long frame_steps[2][5] = {
	{ 7457, 14913, 22371, 29829, 37281 },
	{ 8313, 16627, 24939, 33253, 41565 },
};

static int apu_table(Famicom* f)
{
	return f->region == region_pal ? 1 : 0;
}

static float pulse_mix[31];
static float tnd_mix[203];

// written by the emulator, read by the audio thread
static float ring[RING_SIZE][2]; // left and right
static atomic_uint ring_write;
static atomic_uint ring_read;

// draws the triangle as an ideal smooth wave instead of the 4 bit staircase
bool audio_perfect_triangle = false;
float audio_volume = 1.0f;
bool audio_mute[channel_count];

// leftover fractions of a cycle for each 2A03 timer, see audio_reload_period
static double pitch_error[5];

// last value and write count since the ui last looked, for the register view
static byte register_values[APU_REGISTER_COUNT];
static int register_writes[APU_REGISTER_COUNT];

static float scope[SCOPE_SIZE];
static float channel_scope[channel_count][SCOPE_SIZE];
static int scope_index;

void apu_reset(Famicom_apu* apu)
{
	memset(apu, 0, sizeof(*apu));
	apu->pulse1.ones_complement = true;
	apu->noise.shift = 1;
	apu->noise.timer_period = noise_table[0];
	apu->dmc.rate = dmc_table[0];
	apu->dmc.buffer_empty = true;
	apu->dmc.bits_remaining = 8;
	apu->dmc.silence = true;
	pulse_mix[0] = 0;
	for (int i=1; i<31; i++)
		pulse_mix[i] = 95.52 / (8128.0 / i + 100.0);
	tnd_mix[0] = 0;
	for (int i=1; i<203; i++)
		tnd_mix[i] = 163.67 / (24329.0 / i + 100.0);
}

static void clock_envelope(Famicom_envelope* e)
{
	if (e->start) {
		e->start = false;
		e->decay = 15;
		e->divider = e->volume;
	} else if (e->divider == 0) {
		e->divider = e->volume;
		if (0 < e->decay)
			e->decay--;
		else if (e->loop)
			e->decay = 15;
	} else {
		e->divider--;
	}
}

static byte envelope_output(Famicom_envelope* e)
{
	return (e->constant ? e->volume : e->decay) & 0x0F;
}

static int sweep_target(Famicom_pulse* p)
{
	int change = p->timer_period >> p->sweep_shift;
	if (!p->sweep_negate)
		return p->timer_period + change;
	int target = p->timer_period - change - (p->ones_complement ? 1 : 0);
	return target < 0 ? 0 : target;
}

static void clock_sweep(Famicom_pulse* p)
{
	int target = sweep_target(p);
	if (p->sweep_divider == 0 && p->sweep_enabled && p->sweep_shift && 8 <= p->timer_period && target <= 0x7FF)
		p->timer_period = target;
	if (p->sweep_divider == 0 || p->sweep_reload) {
		p->sweep_divider = p->sweep_period;
		p->sweep_reload = false;
	} else {
		p->sweep_divider--;
	}
}

static void quarter_frame(Famicom_apu* apu)
{
	clock_envelope(&apu->pulse1.envelope);
	clock_envelope(&apu->pulse2.envelope);
	clock_envelope(&apu->noise.envelope);
	Famicom_triangle* t = &apu->triangle;
	if (t->linear_reload_flag)
		t->linear_counter = t->linear_reload;
	else if (0 < t->linear_counter)
		t->linear_counter--;
	if (!t->control)
		t->linear_reload_flag = false;
}

static void half_frame(Famicom_apu* apu)
{
	if (!apu->pulse1.envelope.loop && apu->pulse1.length)
		apu->pulse1.length--;
	if (!apu->pulse2.envelope.loop && apu->pulse2.length)
		apu->pulse2.length--;
	if (!apu->triangle.control && apu->triangle.length)
		apu->triangle.length--;
	if (!apu->noise.envelope.loop && apu->noise.length)
		apu->noise.length--;
	clock_sweep(&apu->pulse1);
	clock_sweep(&apu->pulse2);
}

static void write_pulse(Famicom_apu* apu, Famicom_pulse* p, bool enabled, int reg, byte value)
{
	switch (reg) {
	case 0:
		p->duty = value >> 6;
		p->envelope.loop = value & 0x20;
		p->envelope.constant = value & 0x10;
		p->envelope.volume = value & 0x0F;
		break;
	case 1:
		p->sweep_enabled = value & 0x80;
		p->sweep_period = (value >> 4) & 0x07;
		p->sweep_negate = value & 0x08;
		p->sweep_shift = value & 0x07;
		p->sweep_reload = true;
		break;
	case 2:
		p->timer_period = (p->timer_period & 0x0700) | value;
		break;
	case 3:
		p->timer_period = (p->timer_period & 0x00FF) | ((value & 0x07) << 8);
		if (enabled)
			p->length = length_table[value >> 3];
		p->sequence = 0;
		p->envelope.start = true;
		break;
	}
}

void apu_write(Famicom* f, word addr, byte value)
{
	Famicom_apu* apu = &f->apu;
	if (0x4000 <= addr && addr < 0x4000 + APU_REGISTER_COUNT) {
		register_values[addr - 0x4000] = value;
		register_writes[addr - 0x4000]++;
		// $4000-$4013 are four registers to each channel, the rest are shared
		int channel = addr < 0x4014 ? (addr - 0x4000) / 4 : -1;
		audio_log_write(chip_2a03, channel, addr, value);
	}
	switch (addr) {
	case 0x4000: case 0x4001: case 0x4002: case 0x4003:
		write_pulse(apu, &apu->pulse1, apu->enable[0], addr - 0x4000, value);
		break;
	case 0x4004: case 0x4005: case 0x4006: case 0x4007:
		write_pulse(apu, &apu->pulse2, apu->enable[1], addr - 0x4004, value);
		break;
	case 0x4008:
		apu->triangle.control = value & 0x80;
		apu->triangle.linear_reload = value & 0x7F;
		break;
	case 0x400A:
		apu->triangle.timer_period = (apu->triangle.timer_period & 0x0700) | value;
		break;
	case 0x400B:
		apu->triangle.timer_period = (apu->triangle.timer_period & 0x00FF) | ((value & 0x07) << 8);
		if (apu->enable[2])
			apu->triangle.length = length_table[value >> 3];
		apu->triangle.linear_reload_flag = true;
		break;
	case 0x400C:
		apu->noise.envelope.loop = value & 0x20;
		apu->noise.envelope.constant = value & 0x10;
		apu->noise.envelope.volume = value & 0x0F;
		break;
	case 0x400E:
		apu->noise.mode = value & 0x80;
		apu->noise.timer_period = noise_tables[apu_table(f)][value & 0x0F];
		break;
	case 0x400F:
		if (apu->enable[3])
			apu->noise.length = length_table[value >> 3];
		apu->noise.envelope.start = true;
		break;
	case 0x4010:
		apu->dmc.irq_enable = value & 0x80;
		if (!apu->dmc.irq_enable)
			apu->dmc_irq = false;
		apu->dmc.loop = value & 0x40;
		apu->dmc.rate = dmc_tables[apu_table(f)][value & 0x0F];
		break;
	case 0x4011:
		apu->dmc.output = value & 0x7F;
		break;
	case 0x4012:
		apu->dmc.sample_address = 0xC000 + value * 64;
		break;
	case 0x4013:
		apu->dmc.sample_length = value * 16 + 1;
		break;
	case 0x4015:
		for (int i=0; i<5; i++)
			apu->enable[i] = value & (1 << i);
		if (!apu->enable[0]) apu->pulse1.length = 0;
		if (!apu->enable[1]) apu->pulse2.length = 0;
		if (!apu->enable[2]) apu->triangle.length = 0;
		if (!apu->enable[3]) apu->noise.length = 0;
		if (!apu->enable[4]) {
			apu->dmc.bytes_remaining = 0;
		} else if (apu->dmc.bytes_remaining == 0) {
			apu->dmc.current_address = apu->dmc.sample_address;
			apu->dmc.bytes_remaining = apu->dmc.sample_length;
		}
		apu->dmc_irq = false;
		break;
	case 0x4017:
		apu->five_step = value & 0x80;
		apu->irq_inhibit = value & 0x40;
		if (apu->irq_inhibit)
			apu->frame_irq = false;
		apu->frame_cycle = 0;
		if (apu->five_step) {
			quarter_frame(apu);
			half_frame(apu);
		}
		break;
	}
}

byte apu_read_status(Famicom* f)
{
	Famicom_apu* apu = &f->apu;
	byte status = 0;
	status |= apu->pulse1.length ? 0x01 : 0;
	status |= apu->pulse2.length ? 0x02 : 0;
	status |= apu->triangle.length ? 0x04 : 0;
	status |= apu->noise.length ? 0x08 : 0;
	status |= apu->dmc.bytes_remaining ? 0x10 : 0;
	status |= apu->frame_irq ? 0x40 : 0;
	status |= apu->dmc_irq ? 0x80 : 0;
	apu->frame_irq = false;
	return status;
}

bool apu_irq(Famicom* f)
{
	return f->apu.frame_irq || f->apu.dmc_irq;
}

static void clock_pulse_timer(Famicom_pulse* p, int channel)
{
	if (p->timer == 0) {
		p->timer = audio_reload_period(channel, p->timer_period + 1, &pitch_error[channel]) - 1;
		p->sequence = (p->sequence + 1) & 0x07;
	} else {
		p->timer--;
	}
}

// loaded .wav cycles for each tonal channel
static float* wave_samples[wave_channel_count];
static int wave_sample_length[wave_channel_count];

void audio_set_wave_sample(int wave_channel, const float* data, int length)
{
	if (wave_channel < 0 || wave_channel_count <= wave_channel)
		return;
	free(wave_samples[wave_channel]);
	wave_samples[wave_channel] = NULL;
	wave_sample_length[wave_channel] = 0;
	if (data == NULL || length < 2)
		return;
	if (AUDIO_MAX_WAVE_SAMPLES < length)
		length = AUDIO_MAX_WAVE_SAMPLES;
	float* copy = malloc(length * sizeof(float));
	if (copy == NULL)
		return;
	// stretch whatever level the file has to fill 0..1
	float low = data[0], high = data[0];
	for (int i=1; i<length; i++) {
		if (data[i] < low) low = data[i];
		if (high < data[i]) high = data[i];
	}
	float range = high - low;
	for (int i=0; i<length; i++)
		copy[i] = range > 0.000001f ? (data[i] - low) / range : 0.5f;
	wave_samples[wave_channel] = copy;
	wave_sample_length[wave_channel] = length;
}

bool audio_has_wave_sample(int wave_channel)
{
	return 0 <= wave_channel && wave_channel < wave_channel_count && wave_samples[wave_channel];
}

// the loaded cycle at a phase, interpolated between samples
static float sample_shape(int wave_channel, float phase)
{
	int length = wave_sample_length[wave_channel];
	float position = phase * length;
	int i = (int)position;
	float frac = position - i;
	const float* s = wave_samples[wave_channel];
	return s[i % length] + (s[(i + 1) % length] - s[i % length]) * frac;
}

// the duty squeezes the first half of the cycle into the high part of the pulse and
// stretches the rest over the low part, so a square comes out at the real duty
static const float duty_fractions[4] = { 0.125f, 0.25f, 0.5f, 0.75f };

static float duty_warp(float phase, float duty)
{
	return phase < duty ? 0.5f * phase / duty : 0.5f + 0.5f * (phase - duty) / (1.0f - duty);
}

// 0..1 for a point in a cycle of one of the replacement waveforms
static float wave_shape(int waveform, float phase)
{
	switch (waveform) {
	case wave_square: return phase < 0.5f ? 1.0f : 0.0f;
	case wave_triangle: return phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
	case wave_sine: return 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
	case wave_saw: return phase;
	default: return 0;
	}
}

// the pulse's level, 0..15; other waveforms follow the pulse's own phase, so pitch,
// envelopes, sweeps and length counters all still apply
// how far through its current step a timer is, 0..1, for reloads of the given length
static float timer_fraction(int timer, int reload)
{
	float f = (float)(reload - timer) / (reload + 1);
	return f < 0 ? 0 : 1 < f ? 1 : f;
}

static float pulse_output(Famicom_pulse* p, int waveform, int channel)
{
	if (p->length == 0 || p->timer_period < 8 || 0x7FF < sweep_target(p))
		return 0;
	float volume = envelope_output(&p->envelope);
	if (waveform == wave_default)
		return duty_table[p->duty & 0x03][p->sequence & 0x07] ? volume : 0;
	// a sample waveform with nothing loaded falls back to the real pulse
	if (waveform == wave_sample && !audio_has_wave_sample(channel))
		return duty_table[p->duty & 0x03][p->sequence & 0x07] ? volume : 0;
	int reload = audio_transposed_period(channel, p->timer_period + 1) - 1;
	float phase = ((p->sequence & 0x07) + timer_fraction(p->timer, reload)) / 8.0f;
	phase -= floorf(phase);
	if (audio_fx.follow_duty)
		phase = duty_warp(phase, duty_fractions[p->duty & 0x03]);
	if (waveform == wave_sample)
		return volume * sample_shape(channel, phase);
	return volume * wave_shape(waveform, phase);
}

static void clock_dmc(Famicom* f)
{
	Famicom_dmc* d = &f->apu.dmc;
	// refill the sample buffer, this steals cpu cycles
	if (d->buffer_empty && d->bytes_remaining) {
		d->sample_buffer = mmap_famicom(f, d->current_address, 0, false);
		d->buffer_empty = false;
		f->dma_stall += 4;
		d->current_address = (d->current_address == 0xFFFF) ? 0x8000 : d->current_address + 1;
		d->bytes_remaining--;
		if (d->bytes_remaining == 0) {
			if (d->loop) {
				d->current_address = d->sample_address;
				d->bytes_remaining = d->sample_length;
			} else if (d->irq_enable) {
				f->apu.dmc_irq = true;
			}
		}
	}
	if (d->timer != 0) {
		d->timer--;
		return;
	}
	d->timer = audio_reload_period(channel_dmc, d->rate, &pitch_error[channel_dmc]) - 1;
	if (!d->silence) {
		if (d->shift & 1) {
			if (d->output <= 125)
				d->output += 2;
		} else {
			if (2 <= d->output)
				d->output -= 2;
		}
	}
	d->shift >>= 1;
	if (--d->bits_remaining == 0) {
		d->bits_remaining = 8;
		if (d->buffer_empty) {
			d->silence = true;
		} else {
			d->silence = false;
			d->shift = d->sample_buffer;
			d->buffer_empty = true;
		}
	}
}

// effects, run at the output rate on the filtered mix

float audio_gain[channel_count] = { [0 ... channel_count - 1] = 1.0f };
int audio_transpose[channel_count];
float audio_pitch_shift = 0;
double audio_pitch_scale[channel_count] = { [0 ... channel_count - 1] = 1.0 };

// recomputes the per channel multipliers when an octave or the global shift changes
static void update_pitch_scales(void)
{
	static float last_shift = 0;
	static int last_transpose[channel_count];
	if (last_shift == audio_pitch_shift && memcmp(last_transpose, audio_transpose, sizeof(last_transpose)) == 0)
		return;
	last_shift = audio_pitch_shift;
	memcpy(last_transpose, audio_transpose, sizeof(last_transpose));
	for (int c=0; c<channel_count; c++)
		audio_pitch_scale[c] = pow(2.0, audio_transpose[c] + audio_pitch_shift / 12.0);
}

void audio_update_pitch(void)
{
	update_pitch_scales();
}

const Audio_effects audio_fx_defaults = {
	.limiter_threshold = -3.0f,
	.chorus_mix = 0.5f,
	.chorus_rate = 0.8f,
	.chorus_depth = 3.0f,
	.chorus_channels = { [0 ... channel_count - 1] = true },
	.reverb_mix = 0.25f,
	.reverb_size = 0.6f,
	.reverb_damping = 0.5f,
	.width = 1.5f,
	.surround = 0.3f,
	.follow_duty = true,
	.echo_time = 350,
	.echo_feedback = 0.35f,
	.echo_mix = 0.3f,
	.echo_pingpong = true,
	.eq_freq = { 80, 300, 1000, 3500, 10000 },
};

float audio_master_volume = 1.0f;

// everything the user can change about the sound back to how the hardware does it
void audio_reset_tweaks(void)
{
	audio_fx = audio_fx_defaults;
	audio_master_volume = 1.0f;
	audio_pitch_shift = 0;
	for (int c=0; c<channel_count; c++) {
		audio_gain[c] = 1.0f;
		audio_transpose[c] = 0;
		audio_mute[c] = false;
	}
	audio_perfect_triangle = false;
}

Audio_effects audio_fx = {
	.limiter_threshold = -3.0f,
	.chorus_mix = 0.5f,
	.chorus_rate = 0.8f,
	.chorus_depth = 3.0f,
	.chorus_channels = { [0 ... channel_count - 1] = true },
	.reverb_mix = 0.25f,
	.reverb_size = 0.6f,
	.reverb_damping = 0.5f,
	.width = 1.5f,
	.surround = 0.3f,
	.follow_duty = true,
	.echo_time = 350,
	.echo_feedback = 0.35f,
	.echo_mix = 0.3f,
	.echo_pingpong = true,
	.eq_freq = { 80, 300, 1000, 3500, 10000 },
};
const char* audio_eq_band_names[EQ_BANDS] = { "Low", "Low mid", "Mid", "High mid", "High" };
const char* audio_waveform_names[wave_count] = { "NES", "Square", "Triangle", "Sine", "Saw", "Sample" };

#define CHORUS_SIZE 4096
static float chorus_line[CHORUS_SIZE];
static int chorus_position;
static double chorus_lfo;
static float chorus_dc;

// freeverb's tunings, made for 44.1kHz; the right side is spread a little longer
static const int comb_tuning[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int allpass_tuning[4] = { 556, 441, 341, 225 };
#define REVERB_SPREAD 23
static float comb_buffer[2][8][1617 + REVERB_SPREAD];
static int comb_position[2][8];
static float comb_filter[2][8];
static float allpass_buffer[2][4][556 + REVERB_SPREAD];
static int allpass_position[2][4];

// echo: a second of delay for each side
#define ECHO_SIZE (AUDIO_SAMPLE_RATE + 1024)
static float echo_line[2][ECHO_SIZE];
static int echo_position;
static float echo_tone[2]; // low pass in the feedback, each repeat a little darker

// eq: one biquad per band and side, coefficients rebuilt when a band changes
typedef struct biquad {
	float b0, b1, b2, a1, a2;
} Biquad;
static Biquad eq_filters[EQ_BANDS];
static float eq_state[2][EQ_BANDS][2]; // transposed direct form II state
static float eq_built_freq[EQ_BANDS];
static float eq_built_gain[EQ_BANDS];
static bool eq_built = false;

// the rbj audio eq cookbook: shelves at the ends, peaks with a Q of 1 between
static Biquad eq_design(int band, float frequency, float gain)
{
	double a = pow(10.0, gain / 40.0);
	double w = 2.0 * M_PI * frequency / AUDIO_SAMPLE_RATE;
	double c = cos(w), s = sin(w);
	double b0, b1, b2, a0, a1, a2;
	if (band == 0 || band == EQ_BANDS - 1) {
		double alpha = s / 2.0 * sqrt(2.0);
		double root = 2.0 * sqrt(a) * alpha;
		if (band == 0) {
			b0 = a * ((a + 1) - (a - 1) * c + root);
			b1 = 2 * a * ((a - 1) - (a + 1) * c);
			b2 = a * ((a + 1) - (a - 1) * c - root);
			a0 = (a + 1) + (a - 1) * c + root;
			a1 = -2 * ((a - 1) + (a + 1) * c);
			a2 = (a + 1) + (a - 1) * c - root;
		} else {
			b0 = a * ((a + 1) + (a - 1) * c + root);
			b1 = -2 * a * ((a - 1) + (a + 1) * c);
			b2 = a * ((a + 1) + (a - 1) * c - root);
			a0 = (a + 1) - (a - 1) * c + root;
			a1 = 2 * ((a - 1) - (a + 1) * c);
			a2 = (a + 1) - (a - 1) * c - root;
		}
	} else {
		double alpha = s / 2.0;
		b0 = 1 + alpha * a;
		b1 = -2 * c;
		b2 = 1 - alpha * a;
		a0 = 1 + alpha / a;
		a1 = -2 * c;
		a2 = 1 - alpha / a;
	}
	Biquad q = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
	return q;
}

static void eq_update(void)
{
	if (eq_built && memcmp(eq_built_freq, audio_fx.eq_freq, sizeof(eq_built_freq)) == 0
	    && memcmp(eq_built_gain, audio_fx.eq_gain, sizeof(eq_built_gain)) == 0)
		return;
	for (int b=0; b<EQ_BANDS; b++)
		eq_filters[b] = eq_design(b, audio_fx.eq_freq[b], audio_fx.eq_gain[b]);
	memcpy(eq_built_freq, audio_fx.eq_freq, sizeof(eq_built_freq));
	memcpy(eq_built_gain, audio_fx.eq_gain, sizeof(eq_built_gain));
	eq_built = true;
}

static float biquad(const Biquad* q, float* state, float in)
{
	float out = q->b0 * in + state[0];
	state[0] = q->b1 * in - q->a1 * out + state[1];
	state[1] = q->b2 * in - q->a2 * out;
	return out;
}

// for the eq's response curve: |H| of every band together at a frequency
float audio_eq_response(float frequency)
{
	eq_update();
	double w = 2.0 * M_PI * frequency / AUDIO_SAMPLE_RATE;
	double db = 0;
	for (int b=0; b<EQ_BANDS; b++) {
		const Biquad* q = &eq_filters[b];
		// H(e^jw) = (b0 + b1 e^-jw + b2 e^-2jw) / (1 + a1 e^-jw + a2 e^-2jw)
		double nr = q->b0 + q->b1 * cos(w) + q->b2 * cos(2 * w);
		double ni = -q->b1 * sin(w) - q->b2 * sin(2 * w);
		double dr = 1 + q->a1 * cos(w) + q->a2 * cos(2 * w);
		double di = -q->a1 * sin(w) - q->a2 * sin(2 * w);
		db += 10.0 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
	}
	return db;
}

#define SURROUND_SIZE 1024
static float surround_line[SURROUND_SIZE];
static int surround_position;
static float limiter_gain = 1.0f;

static float delay_read(const float* line, int size, int position, float delay)
{
	float p = position - delay;
	while (p < 0)
		p += size;
	int i = (int)p;
	float frac = p - i;
	float a = line[i % size], b = line[(i + 1) % size];
	return a + (b - a) * frac;
}

static float comb(int side, int i, float in, float feedback, float damping)
{
	int length = comb_tuning[i] + side * REVERB_SPREAD;
	float* buffer = comb_buffer[side][i];
	int* position = &comb_position[side][i];
	float out = buffer[*position];
	comb_filter[side][i] = out * (1.0f - damping) + comb_filter[side][i] * damping;
	buffer[*position] = in + comb_filter[side][i] * feedback;
	if (length <= ++*position)
		*position = 0;
	return out;
}

static float allpass(int side, int i, float in)
{
	int length = allpass_tuning[i] + side * REVERB_SPREAD;
	float* buffer = allpass_buffer[side][i];
	int* position = &allpass_position[side][i];
	float delayed = buffer[*position];
	buffer[*position] = in + delayed * 0.5f;
	if (length <= ++*position)
		*position = 0;
	return delayed - in;
}

void audio_clear_effects(void)
{
	memset(chorus_line, 0, sizeof(chorus_line));
	memset(echo_line, 0, sizeof(echo_line));
	memset(echo_tone, 0, sizeof(echo_tone));
	memset(comb_buffer, 0, sizeof(comb_buffer));
	memset(comb_filter, 0, sizeof(comb_filter));
	memset(allpass_buffer, 0, sizeof(allpass_buffer));
	memset(surround_line, 0, sizeof(surround_line));
	memset(eq_state, 0, sizeof(eq_state));
	limiter_gain = 1.0f;
}

// mono in, chorus bus alongside, stereo out
static void effects(float mono, float chorus_in, float* left, float* right)
{
	float l = mono, r = mono;
	// switching the chorus on or changing its channels starts it clean, with the
	// channels' resting level already taken out, so there's no step to swish through
	static bool chorus_was_on = false;
	static int chorus_mask = -1;
	int mask = 0;
	for (int c=0; c<channel_count; c++)
		mask |= audio_fx.chorus_channels[c] ? 1 << c : 0;
	if (audio_fx.chorus && (!chorus_was_on || mask != chorus_mask)) {
		chorus_dc = chorus_in;
		memset(chorus_line, 0, sizeof(chorus_line));
	}
	chorus_was_on = audio_fx.chorus;
	chorus_mask = mask;
	if (audio_fx.chorus) {
		// two delay taps swinging in opposite directions, one for each side
		chorus_dc += (chorus_in - chorus_dc) * 0.001f;
		chorus_line[chorus_position] = chorus_in - chorus_dc;
		chorus_lfo += audio_fx.chorus_rate / AUDIO_SAMPLE_RATE;
		if (1 <= chorus_lfo)
			chorus_lfo -= 1;
		float base = 0.015f * AUDIO_SAMPLE_RATE;
		float depth = audio_fx.chorus_depth / 1000.0f * AUDIO_SAMPLE_RATE;
		float swing = sinf(2.0f * (float)M_PI * chorus_lfo);
		l += audio_fx.chorus_mix * delay_read(chorus_line, CHORUS_SIZE, chorus_position, base + depth * swing);
		r += audio_fx.chorus_mix * delay_read(chorus_line, CHORUS_SIZE, chorus_position, base - depth * swing);
		chorus_position = (chorus_position + 1) % CHORUS_SIZE;
	}
	if (audio_fx.echo) {
		// read between samples so moving the time slider glides instead of clicking
		float delay = audio_fx.echo_time / 1000.0f * AUDIO_SAMPLE_RATE;
		if (ECHO_SIZE - 2 < delay)
			delay = ECHO_SIZE - 2;
		float dl = delay_read(echo_line[0], ECHO_SIZE, echo_position, delay);
		float dr = delay_read(echo_line[1], ECHO_SIZE, echo_position, delay);
		echo_tone[0] += (dl - echo_tone[0]) * 0.5f;
		echo_tone[1] += (dr - echo_tone[1]) * 0.5f;
		float fb = audio_fx.echo_feedback;
		if (audio_fx.echo_pingpong) {
			// the dry signal goes in on the left and the repeats swap sides each time
			echo_line[0][echo_position] = (l + r) * 0.5f + echo_tone[1] * fb;
			echo_line[1][echo_position] = echo_tone[0] * fb;
		} else {
			echo_line[0][echo_position] = l + echo_tone[0] * fb;
			echo_line[1][echo_position] = r + echo_tone[1] * fb;
		}
		echo_position = (echo_position + 1) % ECHO_SIZE;
		l += dl * audio_fx.echo_mix;
		r += dr * audio_fx.echo_mix;
	}
	if (audio_fx.reverb) {
		float in = (l + r) * 0.015f;
		float feedback = 0.7f + audio_fx.reverb_size * 0.28f;
		float damping = audio_fx.reverb_damping * 0.4f;
		float wl = 0, wr = 0;
		for (int i=0; i<8; i++) {
			wl += comb(0, i, in, feedback, damping);
			wr += comb(1, i, in, feedback, damping);
		}
		for (int i=0; i<4; i++) {
			wl = allpass(0, i, wl);
			wr = allpass(1, i, wr);
		}
		l += wl * 1.5f * audio_fx.reverb_mix;
		r += wr * 1.5f * audio_fx.reverb_mix;
	}
	if (audio_fx.widen) {
		// mid/side: surround adds a delayed copy of the middle to the sides, which
		// spreads even a mono game across the speakers, then width scales the sides
		float mid = (l + r) * 0.5f;
		float side = (l - r) * 0.5f;
		surround_line[surround_position] = mid;
		side += audio_fx.surround * 0.5f * surround_line[(surround_position + SURROUND_SIZE - 661) % SURROUND_SIZE];
		surround_position = (surround_position + 1) % SURROUND_SIZE;
		side *= audio_fx.width;
		l = mid + side;
		r = mid - side;
	}
	if (audio_fx.eq) {
		eq_update();
		for (int b=0; b<EQ_BANDS; b++) {
			l = biquad(&eq_filters[b], eq_state[0][b], l);
			r = biquad(&eq_filters[b], eq_state[1][b], r);
		}
	}
	if (audio_fx.limiter) {
		// instant attack, about 50ms release
		float threshold = powf(10.0f, audio_fx.limiter_threshold / 20.0f);
		float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
		float target = threshold < peak ? threshold / peak : 1.0f;
		if (target < limiter_gain)
			limiter_gain = target;
		else
			limiter_gain += (target - limiter_gain) * 0.0005f;
		l *= limiter_gain;
		r *= limiter_gain;
	}
	*left = l;
	*right = r;
}

// recording for video export
static FILE* record_file = NULL;
static unsigned long record_count = 0;

static void put_le32(FILE* file, unsigned int v)
{
	byte b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF };
	fwrite(b, 1, 4, file);
}

static void put_le16(FILE* file, unsigned int v)
{
	byte b[2] = { v & 0xFF, (v >> 8) & 0xFF };
	fwrite(b, 1, 2, file);
}

static void write_wav_header(FILE* file, unsigned long frames, int rate)
{
	unsigned int data = frames * 4;
	fwrite("RIFF", 1, 4, file);
	put_le32(file, 36 + data);
	fwrite("WAVEfmt ", 1, 8, file);
	put_le32(file, 16);
	put_le16(file, 1); // pcm
	put_le16(file, 2); // stereo
	put_le32(file, rate);
	put_le32(file, rate * 4);
	put_le16(file, 4);
	put_le16(file, 16);
	fwrite("data", 1, 4, file);
	put_le32(file, data);
}

bool audio_record_start(const char* path)
{
	if (record_file)
		return false;
	record_file = fopen(path, "wb");
	if (record_file == NULL)
		return false;
	record_count = 0;
	write_wav_header(record_file, 0, AUDIO_SAMPLE_RATE); // filled in properly at the end
	return true;
}

unsigned long audio_record_stop(int sample_rate)
{
	if (record_file == NULL)
		return 0;
	fseek(record_file, 0, SEEK_SET);
	write_wav_header(record_file, record_count, sample_rate);
	fclose(record_file);
	record_file = NULL;
	return record_count;
}

unsigned long audio_record_frames(void)
{
	return record_count;
}

static void record_sample(float left, float right)
{
	short pair[2];
	float v[2] = { left, right };
	for (int i=0; i<2; i++) {
		float s = v[i] * 32767.0f;
		pair[i] = s < -32768 ? -32768 : 32767 < s ? 32767 : (short)s;
	}
	fwrite(pair, sizeof(short), 2, record_file);
	record_count++;
}

static void push_sample(Famicom_apu* apu, float sample, float chorus_in)
{
	// nes output filters: two high passes and a low pass
	const float dt = 1.0f / AUDIO_SAMPLE_RATE;
	const float hp1 = 1.0f / (2.0f * M_PI * 90.0f);
	const float hp2 = 1.0f / (2.0f * M_PI * 440.0f);
	const float lp = 1.0f / (2.0f * M_PI * 14000.0f);
	float out = hp1 / (hp1 + dt) * (apu->hp1_out + sample - apu->hp1_in);
	apu->hp1_in = sample;
	apu->hp1_out = out;
	sample = out;
	out = hp2 / (hp2 + dt) * (apu->hp2_out + sample - apu->hp2_in);
	apu->hp2_in = sample;
	apu->hp2_out = out;
	apu->lp_out += dt / (lp + dt) * (out - apu->lp_out);
	out = apu->lp_out;

	// each channel's level averaged over the sample, scaled to 0..1
	scope[scope_index] = out;
	for (int c=0; c<channel_count; c++) {
		channel_scope[c][scope_index] = apu->channel_sum[c] / apu->sample_count;
		apu->channel_sum[c] = 0;
	}
	scope_index = (scope_index + 1) % SCOPE_SIZE;

	float left, right;
	effects(out, chorus_in, &left, &right);
	// every sample made goes to a recording, even ones the speakers' buffer drops
	if (record_file)
		record_sample(left * audio_volume * audio_master_volume, right * audio_volume * audio_master_volume);

	unsigned int w = atomic_load_explicit(&ring_write, memory_order_relaxed);
	unsigned int r = atomic_load_explicit(&ring_read, memory_order_acquire);
	if (w - r < RING_SIZE) {
		float volume = audio_volume * audio_master_volume;
		ring[w % RING_SIZE][0] = left * volume;
		ring[w % RING_SIZE][1] = right * volume;
		atomic_store_explicit(&ring_write, w + 1, memory_order_release);
	}
}

// one cpu cycle
void apu_step(Famicom* f)
{
	Famicom_apu* apu = &f->apu;
	cpu_hz = famicom_cpu_hz(f);
	update_pitch_scales();

	Famicom_triangle* t = &apu->triangle;
	int tri_reload = audio_transposed_period(channel_triangle, t->timer_period + 1) - 1;
	if (t->timer == 0) {
		t->timer = audio_reload_period(channel_triangle, t->timer_period + 1, &pitch_error[channel_triangle]) - 1;
		// periods below 2 are ultrasonic, hold the output instead of popping
		if (t->length && t->linear_counter && 2 <= t->timer_period)
			t->sequence = (t->sequence + 1) & 0x1F;
	} else {
		t->timer--;
	}

	if (apu->cycle & 1) {
		clock_pulse_timer(&apu->pulse1, channel_pulse1);
		clock_pulse_timer(&apu->pulse2, channel_pulse2);
	}

	Famicom_noise* n = &apu->noise;
	if (n->timer == 0) {
		n->timer = audio_reload_period(channel_noise, n->timer_period, &pitch_error[channel_noise]) - 1;
		word feedback = (n->shift & 1) ^ ((n->shift >> (n->mode ? 6 : 1)) & 1);
		n->shift = (n->shift >> 1) | (feedback << 14);
	} else {
		n->timer--;
	}

	clock_dmc(f);

	// frame counter
	const long* steps = frame_steps[apu_table(f)];
	long c = apu->frame_cycle;
	if (c == steps[0] || c == steps[2]) {
		quarter_frame(apu);
	} else if (c == steps[1]) {
		quarter_frame(apu);
		half_frame(apu);
	} else if (c == steps[3] && !apu->five_step) {
		quarter_frame(apu);
		half_frame(apu);
		if (!apu->irq_inhibit)
			apu->frame_irq = true;
	} else if (c == steps[4] && apu->five_step) {
		quarter_frame(apu);
		half_frame(apu);
	}
	apu->frame_cycle++;
	if (apu->frame_cycle > (apu->five_step ? steps[4] : steps[3]))
		apu->frame_cycle = 0;
	apu->cycle++;

	// mix
	float p1 = pulse_output(&apu->pulse1, audio_fx.waveform[wave_pulse1], channel_pulse1);
	float p2 = pulse_output(&apu->pulse2, audio_fx.waveform[wave_pulse2], channel_pulse2);
	byte noise = (n->length && !(n->shift & 1)) ? envelope_output(&n->envelope) : 0;
	byte dmc = apu->dmc.output & 0x7F;
	float tri;
	int tri_wave = audio_fx.waveform[wave_triangle_channel];
	if (tri_wave == wave_sample && !audio_has_wave_sample(wave_triangle_channel))
		tri_wave = wave_default;
	bool smooth_tri = audio_perfect_triangle || tri_wave != wave_default;
	if (smooth_tri) {
		// follow the timer between sequencer steps so the wave has no steps
		float frac = 0;
		if (t->length && t->linear_counter && 2 <= t->timer_period)
			frac = timer_fraction(t->timer, tri_reload);
		float phase = ((t->sequence & 0x1F) + frac) / 32.0f;
		if (tri_wave == wave_default)
			tri = 15.0f * fabsf(1.0f - 2.0f * phase);
		else if (tri_wave == wave_sample)
			tri = 15.0f * sample_shape(wave_triangle_channel, phase - floorf(phase));
		else
			tri = 15.0f * wave_shape(tri_wave, phase - floorf(phase));
	} else {
		tri = triangle_table[t->sequence & 0x1F];
	}
	float fds = f->fds.active ? f->fds.filtered / 63.0f : 0;
	float n163 = f->n163.active ? f->n163.output : 0;
	float vrc6 = f->vrc6.active ? f->vrc6.output / 61.0f : 0;
	float vrc7 = f->vrc7.active ? f->vrc7.output : 0;

	// the scopes see every channel, the mix leaves the muted ones out
	apu->channel_sum[channel_pulse1] += p1 / 15.0f;
	apu->channel_sum[channel_pulse2] += p2 / 15.0f;
	apu->channel_sum[channel_triangle] += tri / 15.0f;
	apu->channel_sum[channel_noise] += noise / 15.0f;
	apu->channel_sum[channel_dmc] += dmc / 127.0f;
	apu->channel_sum[channel_fds] += fds;
	apu->channel_sum[channel_n163] += 0.5f + n163 * 0.5f;
	apu->channel_sum[channel_vrc6] += vrc6;
	apu->channel_sum[channel_vrc7] += 0.5f + vrc7 / 6.0f;
	if (audio_mute[channel_pulse1]) p1 = 0;
	if (audio_mute[channel_pulse2]) p2 = 0;
	if (audio_mute[channel_triangle]) tri = 0;
	if (audio_mute[channel_noise]) noise = 0;
	if (audio_mute[channel_dmc]) dmc = 0;
	if (audio_mute[channel_fds]) fds = 0;
	if (audio_mute[channel_n163]) n163 = 0;
	if (audio_mute[channel_vrc6]) vrc6 = 0;
	if (audio_mute[channel_vrc7]) vrc7 = 0;

	// mixer volumes scale each channel's level going into the mix
	p1 *= audio_gain[channel_pulse1];
	p2 *= audio_gain[channel_pulse2];
	tri *= audio_gain[channel_triangle];
	float noise_level = noise * audio_gain[channel_noise];
	float dmc_level = dmc * audio_gain[channel_dmc];
	fds *= audio_gain[channel_fds];
	n163 *= audio_gain[channel_n163];
	vrc6 *= audio_gain[channel_vrc6];
	vrc7 *= audio_gain[channel_vrc7];

	// the 2A03's own nonlinear mixer, worked out directly since levels can be fractional
	float pulses = p1 + p2;
	float pulse = (0 < pulses) ? 95.52f / (8128.0f / pulses + 100.0f) : 0;
	float tnd_in = 3*tri + 2*noise_level + dmc_level;
	float tnd = (0 < tnd_in) ? 163.67f / (24329.0f / tnd_in + 100.0f) : 0;
	// at full volume the fds wave is about 2.4 times a full 2A03 square
	// a full vrc6 pulse is about as loud as a full 2A03 square, vrc7 channels a bit louder
	float extra[channel_count] = { 0 };
	extra[channel_fds] = fds * 0.36f;
	extra[channel_n163] = n163 * 0.25f;
	extra[channel_vrc6] = vrc6 * 0.6f;
	extra[channel_vrc7] = vrc7 * 0.1f;
	apu->sample_sum += pulse + tnd + extra[channel_fds] + extra[channel_n163] + extra[channel_vrc6] + extra[channel_vrc7];

	// the chorus gets its own mix of just the channels picked for it, using the
	// mixer's linear approximation for the 2A03 ones; always gathered, so the sample
	// the chorus is switched on during is already whole
	{
		float bus = 0;
		if (audio_fx.chorus_channels[channel_pulse1]) bus += p1 * 0.00752f;
		if (audio_fx.chorus_channels[channel_pulse2]) bus += p2 * 0.00752f;
		if (audio_fx.chorus_channels[channel_triangle]) bus += tri * 0.00851f;
		if (audio_fx.chorus_channels[channel_noise]) bus += noise_level * 0.00494f;
		if (audio_fx.chorus_channels[channel_dmc]) bus += dmc_level * 0.00335f;
		for (int ch=channel_fds; ch<channel_count; ch++) {
			if (audio_fx.chorus_channels[ch])
				bus += extra[ch];
		}
		apu->chorus_sum += bus;
	}
	apu->sample_count++;

	// resample to the output rate, nudging the rate to keep the ring buffer near its target
	apu->sample_clock += 1.0;
	unsigned int fill = atomic_load_explicit(&ring_write, memory_order_relaxed)
		- atomic_load_explicit(&ring_read, memory_order_relaxed);
	double period = (CPU_HZ / AUDIO_SAMPLE_RATE) * (1.0 + 0.005 * ((double)fill - RING_TARGET) / RING_TARGET);
	// a recording gets exactly the output rate, it isn't racing a sound card
	if (record_file)
		period = CPU_HZ / AUDIO_SAMPLE_RATE;
	if (period <= apu->sample_clock) {
		apu->sample_clock -= period;
		push_sample(apu, apu->sample_sum / apu->sample_count, apu->chorus_sum / apu->sample_count);
		apu->sample_sum = 0;
		apu->chorus_sum = 0;
		apu->sample_count = 0;
	}
}

bool audio_output_muted = false;

// audio thread, fills count stereo frames
void audio_pull(float* out, unsigned int count)
{
	static float last[2] = { 0, 0 };
	unsigned int r = atomic_load_explicit(&ring_read, memory_order_relaxed);
	unsigned int w = atomic_load_explicit(&ring_write, memory_order_acquire);
	if (audio_output_muted) {
		// silence, and whatever was waiting is thrown away
		memset(out, 0, count * 2 * sizeof(float));
		last[0] = last[1] = 0;
		atomic_store_explicit(&ring_read, w, memory_order_release);
		return;
	}
	for (unsigned int i=0; i<count; i++) {
		if (r != w) {
			last[0] = ring[r % RING_SIZE][0];
			last[1] = ring[r % RING_SIZE][1];
			r++;
		}
		out[i * 2] = last[0];
		out[i * 2 + 1] = last[1];
	}
	atomic_store_explicit(&ring_read, r, memory_order_release);
}

static void pulse_voice(Famicom_pulse* p, Audio_voice* v)
{
	if (p->length == 0 || p->timer_period < 8 || 0x7FF < sweep_target(p))
		return;
	byte volume = envelope_output(&p->envelope);
	if (volume == 0)
		return;
	v->frequency = CPU_HZ / (16.0 * (p->timer_period + 1));
	v->volume = volume / 15.0f;
}

int audio_voices(Famicom* f, Audio_voice voices[AUDIO_MAX_VOICES])
{
	Famicom_apu* apu = &f->apu;
	cpu_hz = famicom_cpu_hz(f);
	memset(voices, 0, sizeof(Audio_voice) * AUDIO_MAX_VOICES);
	voices[0].channel = channel_pulse1;
	voices[1].channel = channel_pulse2;
	voices[2].channel = channel_triangle;
	voices[3].channel = channel_noise;
	voices[4].channel = channel_dmc;
	voices[5].channel = channel_fds;
	for (int i=6; i<14; i++)
		voices[i].channel = channel_n163;
	for (int i=14; i<17; i++)
		voices[i].channel = channel_vrc6;
	for (int i=17; i<AUDIO_MAX_VOICES; i++)
		voices[i].channel = channel_vrc7;

	pulse_voice(&apu->pulse1, &voices[0]);
	pulse_voice(&apu->pulse2, &voices[1]);

	Famicom_triangle* t = &apu->triangle;
	if (t->length && t->linear_counter && 2 <= t->timer_period) {
		voices[2].frequency = CPU_HZ / (32.0 * (t->timer_period + 1));
		voices[2].volume = 1.0f;
	}

	// noise has no real pitch, its rate stands in for one; the short loop mode is tonal
	Famicom_noise* n = &apu->noise;
	byte noise_volume = envelope_output(&n->envelope);
	if (n->length && noise_volume && n->timer_period) {
		voices[3].frequency = CPU_HZ / (n->timer_period * (n->mode ? 93.0 : 32.0));
		voices[3].volume = noise_volume / 15.0f;
	}

	// same for samples, placed by their playback rate
	Famicom_dmc* d = &apu->dmc;
	if (d->bytes_remaining || !d->silence) {
		voices[4].frequency = CPU_HZ / (d->rate * 32.0);
		voices[4].volume = 0.8f;
	}

	// the fds wave steps when a 16 bit accumulator overflows, 64 steps a cycle;
	// the modulator bends it, which the trail follows
	Famicom_fds* fds = &f->fds;
	if (fds->active && !fds->halt_wave && fds->volume.gain) {
		int step = fds->frequency + fds->mod_output;
		if (0 < step) {
			voices[5].frequency = CPU_HZ * step / (65536.0 * 64.0);
			voices[5].volume = (fds->volume.gain < 32 ? fds->volume.gain : 32) / 32.0f;
		}
	}

	// each n163 channel gets a turn every 15 cycles times the number enabled
	Famicom_n163* chip = &f->n163;
	if (chip->active && !chip->sound_disabled) {
		int count = ((chip->ram[0x7F] >> 4) & 0x07) + 1;
		for (int i=0; i<count; i++) {
			byte* r = &chip->ram[0x40 + (7 - i) * 8];
			int frequency = r[0] | r[2] << 8 | (r[4] & 0x03) << 16;
			int length = 256 - (r[4] & 0xFC);
			int volume = r[7] & 0x0F;
			if (frequency && volume) {
				voices[6 + i].frequency = (CPU_HZ / (15.0 * count)) * frequency / 65536.0 / length;
				voices[6 + i].volume = volume / 15.0f;
			}
		}
	}

	// vrc6 pulses step through 16 positions, the saw through 14
	Famicom_vrc6* v6 = &f->vrc6;
	if (v6->active && !v6->halt) {
		for (int i=0; i<2; i++) {
			Vrc6_pulse* p = &v6->pulse[i];
			if (p->enabled && p->volume) {
				voices[14 + i].frequency = CPU_HZ / (16.0 * ((p->period >> v6->shift) + 1));
				voices[14 + i].volume = p->volume / 15.0f;
			}
		}
		if (v6->saw_enabled && v6->saw_rate) {
			voices[16].frequency = CPU_HZ / (14.0 * ((v6->saw_period >> v6->shift) + 1));
			voices[16].volume = v6->saw_rate < 42 ? v6->saw_rate / 42.0f : 1.0f;
		}
	}

	Famicom_vrc7* v7 = &f->vrc7;
	if (v7->active) {
		for (int c=0; c<6; c++) {
			float level = vrc7_channel_level(v7, c);
			if (0.01f < level) {
				voices[17 + c].frequency = vrc7_channel_frequency(v7, c);
				voices[17 + c].volume = level;
			}
		}
	}
	// the mixer's octave shifts move what's heard, so they move the trails too
	for (int i=0; i<AUDIO_MAX_VOICES; i++)
		voices[i].frequency *= audio_transpose_scale(voices[i].channel);
	return AUDIO_MAX_VOICES;
}

static Audio_write write_log[AUDIO_WRITE_LOG];
static unsigned long write_log_count, write_log_taken;

void audio_log_write(int chip, int channel, word reg, byte value)
{
	write_log[write_log_count % AUDIO_WRITE_LOG] = (Audio_write){ reg, value, (byte)chip, (signed char)channel };
	write_log_count++;
}

int audio_take_writes(Audio_write* out, int max)
{
	unsigned long from = write_log_taken;
	if (from + AUDIO_WRITE_LOG < write_log_count)
		from = write_log_count - AUDIO_WRITE_LOG; // older ones were written over
	if (from + max < write_log_count)
		from = write_log_count - max;
	int n = 0;
	for (unsigned long i=from; i<write_log_count; i++)
		out[n++] = write_log[i % AUDIO_WRITE_LOG];
	write_log_taken = write_log_count;
	return n;
}

void audio_take_register_writes(byte values[APU_REGISTER_COUNT], int writes[APU_REGISTER_COUNT])
{
	memcpy(values, register_values, sizeof(register_values));
	memcpy(writes, register_writes, sizeof(register_writes));
	memset(register_writes, 0, sizeof(register_writes));
}

void audio_scope_channel(int channel, float* out, int count)
{
	if (channel < 0 || channel_count <= channel)
		return;
	if (SCOPE_SIZE < count)
		count = SCOPE_SIZE;
	for (int i=0; i<count; i++)
		out[i] = channel_scope[channel][(scope_index - count + i + SCOPE_SIZE) % SCOPE_SIZE];
}

void audio_scope(float* out, int count)
{
	if (SCOPE_SIZE < count)
		count = SCOPE_SIZE;
	for (int i=0; i<count; i++)
		out[i] = scope[(scope_index - count + i + SCOPE_SIZE) % SCOPE_SIZE];
}
