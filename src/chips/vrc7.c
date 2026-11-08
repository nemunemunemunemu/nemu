#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "../types.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"
#include "../audio.h"
#include "vrc7.h"

// konami vrc7 sound: a cut down yamaha opll, six two operator fm channels picking
// from fifteen built in instruments or one custom one

// the built in instruments, as measured from a real chip by nuke.ykt
static const byte patches[16][8] = {
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // custom, comes from registers 0-7
	{ 0x03, 0x21, 0x05, 0x06, 0xE8, 0x81, 0x42, 0x27 }, // buzzy bell
	{ 0x13, 0x41, 0x14, 0x0D, 0xD8, 0xF6, 0x23, 0x12 }, // guitar
	{ 0x11, 0x11, 0x08, 0x08, 0xFA, 0xB2, 0x20, 0x12 }, // wurly
	{ 0x31, 0x61, 0x0C, 0x07, 0xA8, 0x64, 0x61, 0x27 }, // flute
	{ 0x32, 0x21, 0x1E, 0x06, 0xE1, 0x76, 0x01, 0x28 }, // clarinet
	{ 0x02, 0x01, 0x06, 0x00, 0xA3, 0xE2, 0xF4, 0xF4 }, // synth
	{ 0x21, 0x61, 0x1D, 0x07, 0x82, 0x81, 0x11, 0x07 }, // trumpet
	{ 0x23, 0x21, 0x22, 0x17, 0xA2, 0x72, 0x01, 0x17 }, // organ
	{ 0x35, 0x11, 0x25, 0x00, 0x40, 0x73, 0x72, 0x01 }, // bells
	{ 0xB5, 0x01, 0x0F, 0x0F, 0xA8, 0xA5, 0x51, 0x02 }, // vibes
	{ 0x17, 0xC1, 0x24, 0x07, 0xF8, 0xF8, 0x22, 0x12 }, // vibraphone
	{ 0x71, 0x23, 0x11, 0x06, 0x65, 0x74, 0x18, 0x16 }, // tutti
	{ 0x01, 0x02, 0xD3, 0x05, 0xC9, 0x95, 0x03, 0x02 }, // fretless
	{ 0x61, 0x63, 0x0C, 0x00, 0x94, 0xC0, 0x33, 0xF6 }, // synth bass
	{ 0x21, 0x72, 0x0D, 0x00, 0xC1, 0xD5, 0x56, 0x06 }, // sweep
};

static const float multipliers[16] = { 0.5f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15 };
// key scale level for block 7 in dB, lower blocks are 6dB less each
static const float ksl_table[16] = { 0, 9, 12, 13.875f, 15, 16.125f, 16.875f, 17.625f, 18, 18.75f, 19.125f, 19.5f, 19.875f, 20.25f, 20.625f, 21 };
static const float ksl_scale[4] = { 0, 0.25f, 0.5f, 1 }; // 0, 1.5, 3 and 6dB an octave
static const float feedback_depth[8] = { 0, 3.14159265f / 16, 3.14159265f / 8, 3.14159265f / 4, 3.14159265f / 2, 3.14159265f, 3.14159265f * 2, 3.14159265f * 4 };

#define SILENT 48.0f

enum { eg_off, eg_attack, eg_decay, eg_sustain, eg_release };

static float sine[1024];
static bool tables_ready = false;

void vrc7_reset(Famicom_vrc7* v)
{
	bool active = v->active;
	memset(v, 0, sizeof(*v));
	v->active = active;
	for (int c=0; c<6; c++) {
		for (int o=0; o<2; o++)
			v->op[c][o].envelope = SILENT;
	}
	if (!tables_ready) {
		for (int i=0; i<1024; i++)
			sine[i] = sinf(i * 2.0f * 3.14159265f / 1024);
		tables_ready = true;
	}
}

static const byte* patch(Famicom_vrc7* v, int channel)
{
	int instrument = v->regs[0x30 + channel] >> 4;
	return instrument ? patches[instrument] : v->regs;
}

void vrc7_write_address(Famicom_vrc7* v, byte value)
{
	v->address = value & 0x3F;
}

void vrc7_write_data(Famicom_vrc7* v, byte value)
{
	v->regs[v->address] = value;
	audio_log_write(chip_vrc7, channel_vrc7, v->address, value);
	if (0x20 <= v->address && v->address <= 0x25) {
		int c = v->address - 0x20;
		bool key = value & 0x10;
		if (key && !v->key[c]) {
			// key on restarts both phases and attacks from wherever the envelope is
			for (int o=0; o<2; o++) {
				v->op[c][o].phase = 0;
				v->op[c][o].state = eg_attack;
			}
			v->op[c][0].out[0] = v->op[c][0].out[1] = 0;
		} else if (!key && v->key[c]) {
			for (int o=0; o<2; o++) {
				if (v->op[c][o].state != eg_off)
					v->op[c][o].state = eg_release;
			}
		}
		v->key[c] = key;
	}
}

static int channel_fnum(Famicom_vrc7* v, int c)
{
	return v->regs[0x10 + c] | (v->regs[0x20 + c] & 0x01) << 8;
}

static int channel_block(Famicom_vrc7* v, int c)
{
	return (v->regs[0x20 + c] >> 1) & 0x07;
}

// dB a sample for a decay or release rate, rates go up by 2^(1/4) a step with key scaling
static float decay_step(int rate, int ksr)
{
	if (rate == 0)
		return 0;
	int effective = rate * 4 + ksr;
	if (63 < effective)
		effective = 63;
	double seconds = 20.9 * pow(2.0, -(effective - 4) / 4.0);
	return SILENT / (seconds * VRC7_RATE);
}

static void envelope(Vrc7_operator* op, const byte* p, int o, bool sustain_flag, int fnum, int block)
{
	byte flags = p[o];
	int attack = p[4 + o] >> 4;
	int decay = p[4 + o] & 0x0F;
	int sustain_level = p[6 + o] >> 4;
	int release = p[6 + o] & 0x0F;
	bool sustained = flags & 0x20;
	int ksr = (flags & 0x10) ? (block << 1) | (fnum >> 8) : block >> 1;
	switch (op->state) {
	case eg_attack: {
		if (attack == 0)
			break;
		int effective = attack * 4 + ksr;
		if (60 <= effective) {
			op->envelope = 0;
			op->state = eg_decay;
			break;
		}
		// exponential rise, from silent to full in the rate's attack time
		double seconds = 1.73 * pow(2.0, -(effective - 4) / 4.0);
		op->envelope *= (float)pow(0.01, 1.0 / (seconds * VRC7_RATE));
		if (op->envelope < 0.1f) {
			op->envelope = 0;
			op->state = eg_decay;
		}
		break;
	}
	case eg_decay:
		op->envelope += decay_step(decay, ksr);
		if (sustain_level * 3.0f <= op->envelope) {
			op->envelope = sustain_level * 3.0f;
			op->state = eg_sustain;
		}
		break;
	case eg_sustain:
		// percussive instruments keep fading at the release rate while held
		if (!sustained)
			op->envelope += decay_step(release, ksr);
		break;
	case eg_release:
		op->envelope += decay_step(sustain_flag ? 5 : release, ksr);
		break;
	}
	if (SILENT <= op->envelope) {
		op->envelope = SILENT;
		if (op->state == eg_release || op->state == eg_sustain)
			op->state = eg_off;
	}
}

static float wave(double phase, bool half)
{
	double cycles = phase - floor(phase);
	float s = sine[(int)(cycles * 1024) & 1023];
	return (half && s < 0) ? 0 : s;
}

static float level(float db)
{
	return db < 96 ? powf(10.0f, -db / 20.0f) : 0;
}

static float key_scale_level(int ksl, int fnum, int block)
{
	float db = ksl_table[fnum >> 5] - 6.0f * (7 - block);
	return db < 0 ? 0 : db * ksl_scale[ksl];
}

static void sample(Famicom_vrc7* v)
{
	v->am_phase += 3.7 / VRC7_RATE;
	v->vib_phase += 6.4 / VRC7_RATE;
	if (1 <= v->am_phase) v->am_phase -= 1;
	if (1 <= v->vib_phase) v->vib_phase -= 1;
	float am = (1.0f - cosf(2 * 3.14159265f * v->am_phase)) * 0.5f * 4.8f; // up to 4.8dB of tremolo
	float vib = sinf(2 * 3.14159265f * v->vib_phase) * 0.008f; // about 14 cents of vibrato

	float sum = 0;
	for (int c=0; c<6; c++) {
		const byte* p = patch(v, c);
		int fnum = channel_fnum(v, c);
		int block = channel_block(v, c);
		bool sustain_flag = v->regs[0x20 + c] & 0x20;
		int volume = v->regs[0x30 + c] & 0x0F;
		// fnum * 2^block / 2^19 cycles a sample before the operator's multiplier
		double base = fnum * (double)(1 << block) / 524288.0 * audio_transpose_scale(channel_vrc7);
		Vrc7_operator* mod = &v->op[c][0];
		Vrc7_operator* car = &v->op[c][1];
		for (int o=0; o<2; o++) {
			Vrc7_operator* op = &v->op[c][o];
			double step = base * multipliers[p[o] & 0x0F];
			if (p[o] & 0x40)
				step *= 1 + vib;
			op->phase += step;
			if (1 <= op->phase)
				op->phase -= floor(op->phase);
			envelope(op, p, o, sustain_flag, fnum, block);
		}
		if (car->state == eg_off && mod->state == eg_off)
			continue;

		// modulator, feeding back on itself
		int fb = p[3] & 0x07;
		double feedback = fb ? feedback_depth[fb] * (mod->out[0] + mod->out[1]) * 0.5 / (2 * 3.14159265) : 0;
		float mod_db = mod->envelope + (p[2] & 0x3F) * 0.75f + key_scale_level(p[2] >> 6, fnum, block) + ((p[0] & 0x80) ? am : 0);
		float mod_out = wave(mod->phase + feedback, p[3] & 0x08) * level(mod_db);
		mod->out[1] = mod->out[0];
		mod->out[0] = mod_out;

		// carrier, its phase pushed around by the modulator by up to two cycles
		float car_db = car->envelope + volume * 3.0f + key_scale_level(p[3] >> 6, fnum, block) + ((p[1] & 0x80) ? am : 0);
		sum += wave(car->phase + mod_out * 2.0, p[3] & 0x10) * level(car_db);
	}
	v->output = v->silenced ? 0 : sum;
}

// one cpu cycle; the chip has its own clock, so it samples at its own rate
void vrc7_clock(Famicom_vrc7* v, double cpu_hz)
{
	v->clock += VRC7_RATE;
	if (v->clock < cpu_hz)
		return;
	v->clock -= cpu_hz;
	sample(v);
}

// for the piano roll: the pitch a channel sounds at, set by the carrier's multiplier
float vrc7_channel_frequency(Famicom_vrc7* v, int c)
{
	const byte* p = patch(v, c);
	return VRC7_RATE * channel_fnum(v, c) * (1 << channel_block(v, c)) / 524288.0 * multipliers[p[1] & 0x0F];
}

float vrc7_channel_level(Famicom_vrc7* v, int c)
{
	if (v->silenced || v->op[c][1].state == eg_off)
		return 0;
	return level(v->op[c][1].envelope + (v->regs[0x30 + c] & 0x0F) * 3.0f);
}
