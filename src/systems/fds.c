#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../types.h"
#include "../bitmath.h"
#include "system.h"
#include "../chips/2C02.h"
#include "../chips/6502.h"
#include "famicom.h"
#include "mappers.h"
#include "fds.h"
#include "../audio.h"

// set from the configuration window, empty means search the usual places
char fds_bios_path[1024] = "";

#define FDS_BIOS_SIZE 0x2000
#define FDS_RAM_SIZE 0x8000 // $6000-$DFFF, the bios sits after it in prg
#define FDS_LEAD_GAP (28300 / 8)
#define FDS_BLOCK_GAP (976 / 8)
#define FDS_TAIL 1024
#define FDS_BYTE_CYCLES 150 // cpu cycles for the disk to move one byte under the head

static byte* side_data(Famicom* f, int side)
{
	return f->fds_disk + side * FDS_RAW_SIDE;
}

static void crc_update(word* crc, byte value)
{
	for (int bit=0x01; bit<=0x80; bit<<=1) {
		bool carry = *crc & 1;
		*crc >>= 1;
		if (carry)
			*crc ^= 0x8408;
		if (value & bit)
			*crc ^= 0x8000;
	}
}

// .fds sides only hold the blocks, the bios expects the gaps, gap end markers and
// crcs a real disk has between them
static int add_gaps(const byte* src, byte* raw)
{
	memset(raw, 0, FDS_RAW_SIDE);
	int out = FDS_LEAD_GAP;
	int i = 0;
	while (i < FDS_SIDE_BYTES) {
		int length;
		switch (src[i]) {
		case 1: length = 56; break;
		case 2: length = 2; break;
		case 3: length = 16; break;
		case 4:
			// the file size is in the header block just before
			if (i < 3)
				goto done;
			length = 1 + (src[i - 3] | src[i - 2] << 8);
			break;
		default:
			goto done;
		}
		if (FDS_SIDE_BYTES < i + length || FDS_RAW_SIDE < out + 1 + length + 2 + FDS_BLOCK_GAP + FDS_TAIL)
			goto done;
		word crc = 0;
		raw[out++] = 0x80;
		crc_update(&crc, 0x80);
		for (int k=0; k<length; k++) {
			raw[out++] = src[i + k];
			crc_update(&crc, src[i + k]);
		}
		crc_update(&crc, 0);
		crc_update(&crc, 0);
		raw[out++] = crc & 0xFF;
		raw[out++] = crc >> 8;
		out += FDS_BLOCK_GAP;
		i += length;
	}
done:
	return out + FDS_TAIL;
}

// the other way, for saving what the game wrote back out as a .fds side
static void remove_gaps(const byte* raw, int raw_length, byte* dst)
{
	memset(dst, 0, FDS_SIDE_BYTES);
	int out = 0, i = 0, file_size = 0;
	while (i < raw_length) {
		while (i < raw_length && raw[i] == 0)
			i++;
		if (raw_length <= i || raw[i] != 0x80)
			break;
		i++;
		if (raw_length <= i)
			break;
		int length;
		switch (raw[i]) {
		case 1: length = 56; break;
		case 2: length = 2; break;
		case 3: length = 16; break;
		case 4: length = 1 + file_size; break;
		default: return;
		}
		if (raw_length < i + length || FDS_SIDE_BYTES < out + length)
			return;
		if (raw[i] == 3)
			file_size = raw[i + 13] | raw[i + 14] << 8;
		memcpy(dst + out, raw + i, length);
		out += length;
		i += length + 2; // skip the crc
	}
}

static bool read_bios(const char* path, byte* out)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	bool ok = fread(out, 1, FDS_BIOS_SIZE, file) == FDS_BIOS_SIZE;
	fclose(file);
	return ok;
}

// the bios is nintendo's and has to come from the user
static bool find_bios(const char* rom_path, byte* out)
{
	char path[1100];
	if (fds_bios_path[0] && read_bios(fds_bios_path, out))
		return true;
	snprintf(path, sizeof(path), "%s", rom_path);
	char* slash = strrchr(path, '/');
	char* backslash = strrchr(path, '\\');
	if (backslash && (!slash || slash < backslash))
		slash = backslash;
	if (slash) {
		slash[1] = '\0';
		strncat(path, "disksys.rom", sizeof(path) - strlen(path) - 1);
		if (read_bios(path, out))
			return true;
	}
	return read_bios("disksys.rom", out) || read_bios("bios/disksys.rom", out);
}

int fds_load(Famicom* f, FILE* rom, byte* header, const char* path)
{
	fseek(rom, 0, SEEK_END);
	long size = ftell(rom);
	int offset = memcmp(header, "FDS\x1A", 4) == 0 ? 16 : 0;
	int sides = (size - offset) / FDS_SIDE_BYTES;
	if (sides < 1) {
		printf("fds image is too small\n");
		fclose(rom);
		return -1;
	}
	if (FDS_MAX_SIDES < sides)
		sides = FDS_MAX_SIDES;
	byte* image = malloc(sides * FDS_SIDE_BYTES);
	fseek(rom, offset, SEEK_SET);
	if (image == NULL || fread(image, 1, sides * FDS_SIDE_BYTES, rom) != (size_t)(sides * FDS_SIDE_BYTES)) {
		printf("fds image is truncated\n");
		free(image);
		fclose(rom);
		return -1;
	}
	fclose(rom);

	// a disk the game saved to earlier takes the place of the original image
	char save[1100];
	snprintf(save, sizeof(save), "%s.sav", path);
	FILE* saved = fopen(save, "rb");
	if (saved) {
		fseek(saved, 0, SEEK_END);
		if (ftell(saved) == sides * FDS_SIDE_BYTES) {
			fseek(saved, 0, SEEK_SET);
			if (fread(image, 1, sides * FDS_SIDE_BYTES, saved) == (size_t)(sides * FDS_SIDE_BYTES))
				printf("using saved disk %s\n", save);
		}
		fclose(saved);
	}

	byte bios[FDS_BIOS_SIZE];
	if (!find_bios(path, bios)) {
		printf("FDS bios (disksys.rom) not found, set it in the configuration window\n");
		free(image);
		return -1;
	}

	f->prg = calloc(FDS_RAM_SIZE + FDS_BIOS_SIZE, 1);
	f->chr = calloc(0x2000, 1);
	f->fds_disk = calloc(sides, FDS_RAW_SIDE);
	if (f->prg == NULL || f->chr == NULL || f->fds_disk == NULL) {
		printf("error allocating fds\n");
		free(image);
		free(f->prg);
		free(f->chr);
		free(f->fds_disk);
		f->prg = f->chr = f->fds_disk = NULL;
		return -1;
	}
	memcpy(f->prg + FDS_RAM_SIZE, bios, FDS_BIOS_SIZE);
	f->prg_size = FDS_RAM_SIZE + FDS_BIOS_SIZE;
	f->chr_size = 0x2000;
	f->chr_is_ram = true;

	memset(&f->fds, 0, sizeof(f->fds));
	f->fds.active = true;
	f->fds.sides = sides;
	for (int s=0; s<sides; s++)
		f->fds.side_length[s] = add_gaps(image + s * FDS_SIDE_BYTES, side_data(f, s));
	f->fds.inserted = 0;
	free(image);

	f->loaded_rom.mapper = MAPPER_FDS;
	f->loaded_rom.mirroring = mirror_vertical;
	printf("FDS disk, %d side%s\n", sides, sides == 1 ? "" : "s");
	if (mapper_init(f) == -1)
		return -1;
	return 0;
}

static void envelope_reset_timer(Fds_envelope* e, byte master)
{
	e->timer = 8 * (e->speed + 1) * master;
}

void fds_reset(Famicom* f, bool warm)
{
	Famicom_fds* d = &f->fds;
	if (!warm)
		memset(f->prg, 0, FDS_RAM_SIZE);
	// the disk stays in the drive across resets
	Famicom_fds keep = *d;
	memset(d, 0, sizeof(*d));
	d->active = true;
	d->sides = keep.sides;
	memcpy(d->side_length, keep.side_length, sizeof(d->side_length));
	d->inserted = keep.inserted;
	d->pending_side = keep.pending_side;
	d->eject_timer = keep.eject_timer;
	d->modified = keep.modified;
	d->disk_enabled = true;
	d->sound_enabled = true;
	d->end_of_head = true;
	d->master_speed = 0xE8;
	d->mod_disabled = true;
	envelope_reset_timer(&d->volume, d->master_speed);
	envelope_reset_timer(&d->mod_envelope, d->master_speed);
	f->mapper.mirroring = mirror_vertical;
}

void fds_eject(Famicom* f)
{
	f->fds.inserted = -1;
	f->fds.eject_timer = 0;
}

void fds_insert(Famicom* f, int side)
{
	Famicom_fds* d = &f->fds;
	if (side < 0 || d->sides <= side)
		return;
	d->inserted = side;
	d->eject_timer = 0;
	d->end_of_head = true;
	d->scanning = false;
}

// the bios needs to see an empty drive before it notices a new side
void fds_switch_side(Famicom* f, int side)
{
	fds_eject(f);
	f->fds.pending_side = side;
	f->fds.eject_timer = 1789773;
}

void fds_save_disk(Famicom* f, const char* rom_path)
{
	Famicom_fds* d = &f->fds;
	if (!d->active || !d->modified || f->fds_disk == NULL)
		return;
	byte* image = malloc(d->sides * FDS_SIDE_BYTES);
	if (image == NULL)
		return;
	for (int s=0; s<d->sides; s++)
		remove_gaps(side_data(f, s), d->side_length[s], image + s * FDS_SIDE_BYTES);
	char save[1100];
	snprintf(save, sizeof(save), "%s.sav", rom_path);
	FILE* file = fopen(save, "wb");
	if (file) {
		fwrite(image, 1, d->sides * FDS_SIDE_BYTES, file);
		fclose(file);
		d->modified = false;
		printf("saved disk to %s\n", save);
	} else {
		printf("couldn't save disk to %s\n", save);
	}
	free(image);
}

static void drive_clock(Famicom* f)
{
	Famicom_fds* d = &f->fds;
	if (d->inserted < 0 || !d->motor_on) {
		d->end_of_head = true;
		d->scanning = false;
		return;
	}
	if (d->reset_transfer && !d->scanning)
		return;
	if (d->end_of_head) {
		// the head travels back to the start of the disk
		d->delay = 50000;
		d->end_of_head = false;
		d->position = 0;
		d->gap_ended = false;
		return;
	}
	if (0 < d->delay) {
		d->delay--;
		return;
	}
	d->scanning = true;
	byte* disk = side_data(f, d->inserted);
	bool need_irq = d->disk_irq_enabled;
	if (d->read_mode) {
		byte data = disk[d->position];
		if (!d->previous_crc_control)
			crc_update(&d->crc, data);
		if (!d->disk_ready) {
			d->gap_ended = false;
			d->crc = 0;
		} else if (data && !d->gap_ended) {
			// the first byte after the gap is the end marker, it isn't handed over
			d->gap_ended = true;
			need_irq = false;
		}
		if (d->gap_ended) {
			d->transfer_complete = true;
			d->read_data = data;
			if (need_irq)
				d->disk_irq = true;
		}
	} else {
		byte data = 0;
		if (!d->crc_control) {
			d->transfer_complete = true;
			data = d->write_data;
			if (need_irq)
				d->disk_irq = true;
		}
		if (!d->disk_ready)
			data = 0;
		if (!d->crc_control) {
			crc_update(&d->crc, data);
		} else {
			if (!d->previous_crc_control) {
				crc_update(&d->crc, 0);
				crc_update(&d->crc, 0);
			}
			data = d->crc & 0xFF;
			d->crc >>= 8;
		}
		// the write head trails the read head by two bytes
		if (2 <= d->position) {
			disk[d->position - 2] = data;
			d->modified = true;
		}
		d->gap_ended = false;
	}
	d->previous_crc_control = d->crc_control;
	d->position++;
	if (d->side_length[d->inserted] <= d->position)
		d->motor_on = false;
	else
		d->delay = FDS_BYTE_CYCLES;
}

// sound

static const int wave_volume[4] = { 36, 24, 17, 14 };
static const int mod_steps[8] = { 0, 1, 2, 4, 0, -4, -2, -1 }; // step 4 resets the counter

static void envelope_write(Fds_envelope* e, byte value, byte master)
{
	e->speed = value & 0x3F;
	e->increase = value & 0x40;
	e->off = value & 0x80;
	if (e->off)
		e->gain = e->speed;
	envelope_reset_timer(e, master);
}

static bool envelope_tick(Fds_envelope* e, byte master)
{
	if (e->off || master == 0)
		return false;
	if (0 < --e->timer)
		return false;
	envelope_reset_timer(e, master);
	if (e->increase && e->gain < 32)
		e->gain++;
	else if (!e->increase && 0 < e->gain)
		e->gain--;
	return true;
}

static void mod_set_counter(Famicom_fds* d, int value)
{
	if (64 <= value)
		value -= 128;
	else if (value < -64)
		value += 128;
	d->mod_counter = value;
}

// how far the modulator bends the wave's pitch, from the nesdev description
static void mod_update_output(Famicom_fds* d)
{
	int temp = d->mod_counter * d->mod_envelope.gain;
	int remainder = temp & 0x0F;
	temp >>= 4;
	if (0 < remainder && (temp & 0x80) == 0)
		temp += d->mod_counter < 0 ? -1 : 2;
	if (192 <= temp)
		temp -= 256;
	else if (temp < -64)
		temp += 256;
	temp = d->frequency * temp;
	remainder = temp & 0x3F;
	temp >>= 6;
	if (32 <= remainder)
		temp += 1;
	d->mod_output = temp;
}

static bool mod_tick(Famicom_fds* d)
{
	if (d->mod_disabled || d->mod_frequency == 0)
		return false;
	word before = d->mod_overflow;
	d->mod_overflow += d->mod_frequency;
	if (before <= d->mod_overflow)
		return false;
	int step = d->mod_table[d->mod_position];
	if (step == 4)
		d->mod_counter = 0;
	else
		mod_set_counter(d, d->mod_counter + mod_steps[step]);
	d->mod_position = (d->mod_position + 1) & 0x3F;
	return true;
}

static void sound_clock(Famicom_fds* d)
{
	if (!d->halt_wave && !d->disable_envelopes) {
		envelope_tick(&d->volume, d->master_speed);
		if (envelope_tick(&d->mod_envelope, d->master_speed))
			mod_update_output(d);
	}
	if (mod_tick(d))
		mod_update_output(d);
	if (d->halt_wave) {
		d->wave_position = 0;
	} else {
		// the fraction of a step a pitch shift leaves over is carried to the next cycle
		static double carry = 0;
		double exact = (d->frequency + d->mod_output) * audio_transpose_scale(channel_fds) + carry;
		int step = (int)exact;
		carry = (0 < step) ? exact - step : 0;
		if (0 < step && !d->wave_write) {
			word before = d->wave_overflow;
			d->wave_overflow += step;
			if (d->wave_overflow < before)
				d->wave_position = (d->wave_position + 1) & 0x3F;
		}
	}
	int gain = d->volume.gain < 32 ? d->volume.gain : 32;
	d->output = d->wave[d->wave_position] * gain * wave_volume[d->master_volume & 3] / 1152;
	// the ram adapter's output has a low pass around 2kHz
	d->filtered += (d->output - d->filtered) * 0.007f;
}

// one cpu cycle
void fds_clock(Famicom* f)
{
	Famicom_fds* d = &f->fds;
	if (d->sound_only) {
		sound_clock(d);
		return;
	}
	if (d->irq_enabled) {
		if (d->irq_counter == 0) {
			d->timer_irq = true;
			d->irq_counter = d->irq_reload;
			if (!d->irq_repeat)
				d->irq_enabled = false;
		} else {
			d->irq_counter--;
		}
	}
	if (0 < d->eject_timer && --d->eject_timer == 0)
		fds_insert(f, d->pending_side);
	drive_clock(f);
	sound_clock(d);
}

bool fds_irq(Famicom* f)
{
	return f->fds.timer_irq || f->fds.disk_irq;
}

byte fds_read(Famicom* f, word addr)
{
	Famicom_fds* d = &f->fds;
	byte value;
	switch (addr) {
	case 0x4030:
		if (!d->disk_enabled)
			return 0x40;
		value = 0;
		if (d->timer_irq) value |= 0x01;
		if (d->transfer_complete) value |= 0x02;
		if (d->end_of_head) value |= 0x40;
		d->transfer_complete = false;
		d->timer_irq = false;
		d->disk_irq = false;
		return value;
	case 0x4031:
		d->transfer_complete = false;
		d->disk_irq = false;
		return d->read_data;
	case 0x4032:
		value = 0x40;
		if (d->inserted < 0) value |= 0x01; // no disk
		if (d->inserted < 0 || !d->scanning) value |= 0x02; // not ready
		if (d->inserted < 0) value |= 0x04; // not writable
		return value;
	case 0x4033:
		return 0x80; // battery is fine
	}
	if (0x4040 <= addr && addr < 0x4080)
		return d->wave[addr & 0x3F] | 0x40;
	if (addr == 0x4090)
		return d->volume.gain | 0x40;
	if (addr == 0x4092)
		return d->mod_envelope.gain | 0x40;
	return addr >> 8;
}

void fds_write(Famicom* f, word addr, byte value)
{
	Famicom_fds* d = &f->fds;
	if (0x4040 <= addr && addr <= 0x408A)
		audio_log_write(chip_fds, channel_fds, addr, value);
	if (addr < 0x4040) {
		if (!d->disk_enabled && addr != 0x4023)
			return;
		switch (addr) {
		case 0x4020:
			d->irq_reload = (d->irq_reload & 0xFF00) | value;
			break;
		case 0x4021:
			d->irq_reload = (d->irq_reload & 0x00FF) | value << 8;
			break;
		case 0x4022:
			d->irq_repeat = value & 0x01;
			d->irq_enabled = (value & 0x02) && d->disk_enabled;
			if (d->irq_enabled)
				d->irq_counter = d->irq_reload;
			else
				d->timer_irq = false;
			break;
		case 0x4023:
			d->disk_enabled = value & 0x01;
			d->sound_enabled = value & 0x02;
			if (!d->disk_enabled) {
				d->irq_enabled = false;
				d->timer_irq = false;
				d->disk_irq = false;
			}
			break;
		case 0x4024:
			d->write_data = value;
			d->transfer_complete = false;
			d->disk_irq = false;
			break;
		case 0x4025:
			d->motor_on = value & 0x01;
			d->reset_transfer = value & 0x02;
			d->read_mode = value & 0x04;
			f->mapper.mirroring = (value & 0x08) ? mirror_horizontal : mirror_vertical;
			d->crc_control = value & 0x10;
			d->disk_ready = value & 0x40;
			d->disk_irq_enabled = value & 0x80;
			d->disk_irq = false;
			break;
		}
		return;
	}
	if (!d->sound_enabled)
		return;
	if (addr < 0x4080) {
		if (d->wave_write)
			d->wave[addr & 0x3F] = value & 0x3F;
		return;
	}
	switch (addr) {
	case 0x4080:
		envelope_write(&d->volume, value, d->master_speed);
		break;
	case 0x4082:
		d->frequency = (d->frequency & 0x0F00) | value;
		mod_update_output(d);
		break;
	case 0x4083:
		d->frequency = (d->frequency & 0x00FF) | (value & 0x0F) << 8;
		d->halt_wave = value & 0x80;
		d->disable_envelopes = value & 0x40;
		if (d->halt_wave)
			d->wave_position = 0;
		if (d->disable_envelopes) {
			envelope_reset_timer(&d->volume, d->master_speed);
			envelope_reset_timer(&d->mod_envelope, d->master_speed);
		}
		mod_update_output(d);
		break;
	case 0x4084:
		envelope_write(&d->mod_envelope, value, d->master_speed);
		mod_update_output(d);
		break;
	case 0x4085:
		mod_set_counter(d, value & 0x7F);
		mod_update_output(d);
		break;
	case 0x4086:
		d->mod_frequency = (d->mod_frequency & 0x0F00) | value;
		break;
	case 0x4087:
		d->mod_frequency = (d->mod_frequency & 0x00FF) | (value & 0x0F) << 8;
		d->mod_disabled = value & 0x80;
		if (d->mod_disabled)
			d->mod_overflow = 0;
		break;
	case 0x4088:
		// the 32 entry table is written while the modulator is halted, each entry twice
		if (d->mod_disabled) {
			d->mod_table[d->mod_position & 0x3F] = value & 0x07;
			d->mod_table[(d->mod_position + 1) & 0x3F] = value & 0x07;
			d->mod_position = (d->mod_position + 2) & 0x3F;
		}
		break;
	case 0x4089:
		d->wave_write = value & 0x80;
		d->master_volume = value & 0x03;
		break;
	case 0x408A:
		d->master_speed = value;
		envelope_reset_timer(&d->volume, d->master_speed);
		envelope_reset_timer(&d->mod_envelope, d->master_speed);
		break;
	}
}
