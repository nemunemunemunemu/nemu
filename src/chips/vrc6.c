#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../types.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"
#include "../audio.h"
#include "vrc6.h"

// konami vrc6 sound: two pulse channels with 8 duty settings and a sawtooth

void vrc6_reset(Famicom_vrc6* v)
{
	bool active = v->active;
	memset(v, 0, sizeof(*v));
	v->active = active;
}

// addresses arrive with mapper 26's swapped lines already put right, $9000-$B002
void vrc6_write(Famicom_vrc6* v, word addr, byte value)
{
	int reg = addr & 0x03;
	// $9xxx and $Axxx are the two pulses, $Bxxx the saw
	audio_log_write(chip_vrc6, channel_vrc6, addr, value);
	switch (addr & 0xF000) {
	case 0x9000:
	case 0xA000: {
		if (addr == 0x9003) {
			v->halt = value & 0x01;
			v->shift = (value & 0x04) ? 8 : (value & 0x02) ? 4 : 0;
			break;
		}
		Vrc6_pulse* p = &v->pulse[(addr & 0xF000) == 0xA000];
		switch (reg) {
		case 0:
			p->constant = value & 0x80;
			p->duty = (value >> 4) & 0x07;
			p->volume = value & 0x0F;
			break;
		case 1:
			p->period = (p->period & 0x0F00) | value;
			break;
		case 2:
			p->period = (p->period & 0x00FF) | (value & 0x0F) << 8;
			p->enabled = value & 0x80;
			if (!p->enabled)
				p->step = 0;
			break;
		}
		break;
	}
	case 0xB000:
		switch (reg) {
		case 0:
			v->saw_rate = value & 0x3F;
			break;
		case 1:
			v->saw_period = (v->saw_period & 0x0F00) | value;
			break;
		case 2:
			v->saw_period = (v->saw_period & 0x00FF) | (value & 0x0F) << 8;
			v->saw_enabled = value & 0x80;
			if (!v->saw_enabled) {
				v->saw_step = 0;
				v->saw_accumulator = 0;
			}
			break;
		}
		break;
	}
}

// leftover fractions of a cycle for each timer when the pitch is shifted
static double pitch_error[3];

// one cpu cycle
void vrc6_clock(Famicom_vrc6* v)
{
	if (!v->halt) {
		for (int i=0; i<2; i++) {
			Vrc6_pulse* p = &v->pulse[i];
			if (!p->enabled)
				continue;
			if (--p->timer < 0) {
				p->timer = audio_reload_period(channel_vrc6, (p->period >> v->shift) + 1, &pitch_error[i]) - 1;
				p->step = (p->step + 1) & 0x0F;
			}
		}
		if (v->saw_enabled && --v->saw_timer < 0) {
			v->saw_timer = audio_reload_period(channel_vrc6, (v->saw_period >> v->shift) + 1, &pitch_error[2]) - 1;
			// the accumulator grows every other step and starts over after 14
			v->saw_step++;
			if (v->saw_step == 14) {
				v->saw_step = 0;
				v->saw_accumulator = 0;
			} else if ((v->saw_step & 1) == 0) {
				v->saw_accumulator += v->saw_rate;
			}
		}
	}
	int out = 0;
	for (int i=0; i<2; i++) {
		Vrc6_pulse* p = &v->pulse[i];
		if (p->enabled && (p->constant || p->step <= p->duty))
			out += p->volume;
	}
	if (v->saw_enabled)
		out += v->saw_accumulator >> 3;
	v->output = out;
}
