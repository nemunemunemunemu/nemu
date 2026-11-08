#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../types.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"
#include "../audio.h"
#include "n163.h"

// namco 163 wavetable sound: 128 bytes of ram hold 4 bit samples and the registers
// for up to 8 channels, which live at the top of ram and count down from $78

void n163_reset(Famicom_n163* n)
{
	memset(n->ram, 0, sizeof(n->ram));
	memset(n->channel_output, 0, sizeof(n->channel_output));
	n->address = 0;
	n->auto_increment = false;
	n->sound_disabled = false;
	n->channel = 0;
	n->timer = 0;
	n->output = 0;
}

byte n163_read_data(Famicom_n163* n)
{
	byte value = n->ram[n->address];
	if (n->auto_increment)
		n->address = (n->address + 1) & 0x7F;
	return value;
}

void n163_write_data(Famicom_n163* n, byte value)
{
	n->ram[n->address] = value;
	audio_log_write(chip_n163, channel_n163, n->address, value);
	if (n->auto_increment)
		n->address = (n->address + 1) & 0x7F;
}

void n163_write_address(Famicom_n163* n, byte value)
{
	n->address = value & 0x7F;
	n->auto_increment = value & 0x80;
}

static void update_channel(Famicom_n163* n, int channel)
{
	byte* r = &n->ram[0x40 + channel * 8];
	int frequency = (int)((r[0] | r[2] << 8 | (r[4] & 0x03) << 16) * audio_transpose_scale(channel_n163));
	int phase = r[1] | r[3] << 8 | r[5] << 16;
	int length = 256 - (r[4] & 0xFC);
	phase = (phase + frequency) % (length << 16);
	// the phase lives in ram, games can read it back
	r[1] = phase;
	r[3] = phase >> 8;
	r[5] = phase >> 16;
	int index = ((phase >> 16) + r[6]) & 0xFF;
	int sample = (n->ram[index >> 1] >> ((index & 1) * 4)) & 0x0F;
	n->channel_output[channel] = (sample - 8) * (r[7] & 0x0F);
}

// one cpu cycle
void n163_clock(Famicom_n163* n)
{
	if (n->sound_disabled || ++n->timer < 15)
		return;
	n->timer = 0;
	int count = ((n->ram[0x7F] >> 4) & 0x07) + 1;
	if (count <= n->channel)
		n->channel = 0;
	update_channel(n, 7 - n->channel);
	n->channel = (n->channel + 1) % count;
	// the chip switches between channels, which averages them, so just mix the average
	int sum = 0;
	for (int i=0; i<count; i++)
		sum += n->channel_output[7 - i];
	n->output = sum / (float)count / 120.0f;
}
