#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include "../chips/n163.h"
#include "../chips/vrc6.h"
#include "../chips/vrc7.h"
#include "../audio.h"
#include "../zip.h"

const int memsize_famicom = 0x0800;
// the file being loaded, the fds loader looks for the bios and saves next to it
static const char* loading_path = "";

int famicom_region_setting = region_auto;
const char* famicom_region_names[region_count] = { "NTSC", "PAL", "Dendy" };
static const double region_cpu_hz[region_count] = { 1789773.0, 1662607.0, 1773448.0 };
static const double region_frame_rate[region_count] = { 60.0988, 50.0070, 50.0070 };

static int region_index(Famicom* famicom)
{
	return (0 <= famicom->region && famicom->region < region_count) ? famicom->region : region_ntsc;
}

double famicom_cpu_hz (Famicom* famicom)
{
	return region_cpu_hz[region_index(famicom)];
}

double famicom_frame_rate (Famicom* famicom)
{
	return region_frame_rate[region_index(famicom)];
}

// pal has 70 lines of vblank after line 240, dendy idles 50 lines first and has 20
int famicom_vblank_line (Famicom* famicom)
{
	return famicom->region == region_dendy ? 291 : 241;
}

int famicom_last_line (Famicom* famicom)
{
	return famicom->region == region_ntsc ? 261 : 311;
}

void famicom_apply_region (Famicom* famicom)
{
	if (famicom_region_setting == region_auto)
		famicom->region = famicom->loaded_rom.region;
	else
		famicom->region = famicom_region_setting - 1;
	famicom->region = region_index(famicom);
	famicom->ppu_phase = 0;
}
const int prg_ram_size = 0x2000;

int famicom_create (Famicom* famicom)
{
	memset(famicom, 0, sizeof(*famicom));
	famicom->mem = calloc(memsize_famicom, sizeof(byte));
	famicom->prg_ram = calloc(prg_ram_size, sizeof(byte));
	if (famicom->mem == NULL || famicom->prg_ram == NULL) {
		printf("couldn't allocate memory\n");
		famicom_destroy(famicom);
		return -1;
	}
	famicom->cpu.running = false;
	famicom->cpu.current_instruction_name = " ";
	return 0;
}

static void nsf_start_song(Famicom* f);

void famicom_reset_controller(Famicom* f)
{
	f->controller_p1.right = false;
	f->controller_p1.left = false;
	f->controller_p1.up = false;
	f->controller_p1.down = false;
	f->controller_p1.select = false;
	f->controller_p1.start = false;
	f->controller_p1.button_b = false;
	f->controller_p1.button_a = false;
	f->controller_p2 = f->controller_p1;
	f->controller_strobe = false;
	f->controller_shift[0] = 0;
	f->controller_shift[1] = 0;
}

void famicom_reset (Famicom* famicom, bool warm)
{
	System system; system.s = famicom_system; system.h = famicom;
	// nothing to run with an empty cartridge slot
	if (!famicom_has_cartridge(famicom)) {
		famicom->cpu.running = false;
		return;
	}
	famicom_apply_region(famicom);
	if (!warm) {
		memset(famicom->mem, 0, sizeof(byte) * memsize_famicom);
		memset(famicom->prg_ram, 0, sizeof(byte) * prg_ram_size);
		if (famicom->chr_is_ram)
			memset(famicom->chr, 0, famicom->chr_size);
	}
	ppu_reset(&famicom->ppu, warm);
	apu_reset(&famicom->apu);
	mapper_init(famicom);
	// only the disk system has a drive and bios to reset, fds nsfs set up per song
	if (famicom->fds.active && famicom->fds_disk)
		fds_reset(famicom, warm);
	famicom_reset_controller(famicom);
	famicom->dma_stall = 0;
	famicom->cpu_cycles = 0;
	cpu_reset(&famicom->cpu, system);
	if (famicom->nsf.active)
		nsf_start_song(famicom);
}

static void free_rom(Famicom* famicom)
{
	free(famicom->prg);
	free(famicom->chr);
	free(famicom->fds_disk);
	free(famicom->nsf_ram);
	famicom->prg = NULL;
	famicom->chr = NULL;
	famicom->fds_disk = NULL;
	famicom->nsf_ram = NULL;
	famicom->prg_size = 0;
	famicom->chr_size = 0;
}

void famicom_destroy (Famicom* famicom)
{
	// anything the game wrote to its disk goes to a save file first
	fds_save_disk(famicom, famicom->rom_path);
	free_rom(famicom);
	free(famicom->mem);
	free(famicom->prg_ram);
	famicom->mem = NULL;
	famicom->prg_ram = NULL;
}

static int famicom_load_nsf (Famicom* famicom, FILE* rom, byte* start);
static int famicom_load_nsfe (Famicom* famicom, FILE* rom);

int famicom_load_rom (Famicom* famicom, FILE* rom)
{
	byte header[16];
	if (fread(header, sizeof(byte), 16, rom) != 16) {
		printf("not an NES rom\n");
		fclose(rom);
		return -1;
	}
	if (memcmp(header, "NESM\x1A", 5) == 0)
		return famicom_load_nsf(famicom, rom, header);
	if (memcmp(header, "NSFE", 4) == 0)
		return famicom_load_nsfe(famicom, rom);
	// .fds with or without the fwNES header, a bare image starts with the disk info block
	if (memcmp(header, "FDS\x1A", 4) == 0 || (header[0] == 0x01 && memcmp(header + 1, "*NINTENDO-HVC*", 14) == 0))
		return fds_load(famicom, rom, header, loading_path);
	if (memcmp(header, "NES\x1A", 4) != 0) {
		printf("not an NES rom\n");
		fclose(rom);
		return -1;
	}
	int prg_size = header[4] * 16384;
	int chr_size = header[5] * 8192;
	byte mapper_lo = (header[6] & 0xF0) >> 4;
	byte mapper_hi = header[7] & 0xF0;
	// old dumps have junk like "DiskDude!" in bytes 7-15
	if ((header[7] & 0x0C) != 0x08 && (header[12] | header[13] | header[14] | header[15]) != 0)
		mapper_hi = 0;
	int mapper = (mapper_hi | mapper_lo);
	int mirroring;
	if (header[6] & 0x08)
		mirroring = mirror_four_screen;
	else if (header[6] & 0x01)
		mirroring = mirror_vertical;
	else
		mirroring = mirror_horizontal;
	famicom->loaded_rom.mapper = mapper;
	famicom->loaded_rom.mirroring = mirroring;
	// nes 2.0 says the region in byte 12, plain ines only has a rarely set pal bit
	famicom->loaded_rom.region = region_ntsc;
	if ((header[7] & 0x0C) == 0x08) {
		int timing = header[12] & 0x03;
		famicom->loaded_rom.region = timing == 1 ? region_pal : timing == 3 ? region_dendy : region_ntsc;
	} else if ((header[12] | header[13] | header[14] | header[15]) == 0 && (header[9] & 0x01)) {
		famicom->loaded_rom.region = region_pal;
	}
	printf("NES rom, mapper: %d, \nprg size: %d, chr size: %d, mirroring: %d\n", mapper, prg_size, chr_size, mirroring);
	if (prg_size == 0) {
		printf("rom has no prg\n");
		fclose(rom);
		return -1;
	}
	// skip the trainer
	if (header[6] & 0x04)
		fseek(rom, 512, SEEK_CUR);

	free_rom(famicom);
	famicom->prg = malloc(prg_size);
	if (famicom->prg == NULL) {
		printf("error allocating prg\n");
		goto fail;
	}
	famicom->prg_size = prg_size;
	if (fread(famicom->prg, sizeof(byte), prg_size, rom) != (size_t)prg_size) {
		printf("rom is truncated\n");
		goto fail;
	}
	if (chr_size) {
		famicom->chr_is_ram = false;
		famicom->chr = malloc(chr_size);
		if (famicom->chr == NULL) {
			printf("error allocating chr\n");
			goto fail;
		}
		famicom->chr_size = chr_size;
		if (fread(famicom->chr, sizeof(byte), chr_size, rom) != (size_t)chr_size) {
			printf("rom is truncated\n");
			goto fail;
		}
	} else {
		famicom->chr_is_ram = true;
		chr_size = (mapper == 13) ? 0x4000 : 0x2000;
		famicom->chr = calloc(chr_size, sizeof(byte));
		if (famicom->chr == NULL) {
			printf("error allocating chr ram\n");
			goto fail;
		}
		famicom->chr_size = chr_size;
	}
	if (mapper_init(famicom) == -1) {
		printf("unsupported mapper\n");
		goto fail;
	}
	fclose(rom);
	return 0;
fail:
	free_rom(famicom);
	fclose(rom);
	return -1;
}

static void nsf_string(char* out, const byte* field)
{
	memcpy(out, field, 32);
	out[32] = '\0';
}

static void nsf_defaults(Famicom_nsf* nsf)
{
	memset(nsf, 0, sizeof(*nsf));
	for (int t=0; t<NSF_MAX_TRACKS; t++) {
		nsf->track_time[t] = -1;
		nsf->track_fade[t] = -1;
	}
	nsf->speed[region_ntsc] = 16639;
	nsf->speed[region_pal] = 19997;
	nsf->speed[region_dendy] = 19997;
}

// header flags: bit 0 pal, bit 1 plays on both, which we take as ntsc
static int nsf_region(byte flags)
{
	return (flags & 0x03) == 0x01 ? region_pal : region_ntsc;
}

// shared by nsf and nsfe once the header is parsed: lays the music data out as prg
static int nsf_build(Famicom* famicom, const byte* data, long data_size)
{
	Famicom_nsf* nsf = &famicom->nsf;
	// 8 bit bank registers can only reach 1MB
	if (0x100000 < data_size)
		data_size = 0x100000;
	bool fds = nsf->expansion & NSF_FDS;
	printf("%s: %s - %s, %d songs\n", nsf->nsfe ? "NSFe" : "NSF", nsf->title, nsf->artist, nsf->songs);
	if (nsf->expansion & ~NSF_SUPPORTED)
		printf("uses expansion audio that isn't emulated, those parts won't play\n");
	// fds tunes can load into ram from $6000, everything else lives at $8000 and up
	word lowest = fds ? 0x6000 : 0x8000;
	if (data_size <= 0 || nsf->load_address < lowest) {
		printf("unsupported nsf\n");
		nsf_defaults(nsf);
		return -1;
	}

	// banked nsfs are padded so the load address lands inside a 4KB bank
	int padding, size;
	if (nsf->bankswitched) {
		padding = nsf->load_address & 0x0FFF;
		size = (padding + data_size + 0x1FFF) & ~0x1FFF;
	} else {
		padding = nsf->load_address - lowest;
		size = fds ? NSF_FDS_RAM : 0x8000;
		if (size - padding < data_size)
			data_size = size - padding;
	}
	free_rom(famicom);
	famicom->prg = calloc(size, sizeof(byte));
	famicom->chr = calloc(0x2000, sizeof(byte));
	if (fds)
		famicom->nsf_ram = calloc(NSF_FDS_RAM, sizeof(byte));
	if (famicom->prg == NULL || famicom->chr == NULL || (fds && famicom->nsf_ram == NULL)) {
		printf("error allocating nsf\n");
		goto fail;
	}
	famicom->prg_size = size;
	famicom->chr_size = 0x2000;
	famicom->chr_is_ram = true;
	memcpy(famicom->prg + padding, data, data_size);
	famicom->loaded_rom.mapper = MAPPER_NSF;
	famicom->loaded_rom.mirroring = mirror_vertical;
	nsf->active = true;
	if (mapper_init(famicom) == -1)
		goto fail;
	return 0;
fail:
	free_rom(famicom);
	nsf_defaults(nsf);
	return -1;
}

static int famicom_load_nsf (Famicom* famicom, FILE* rom, byte* start)
{
	byte header[128];
	memcpy(header, start, 16);
	if (fread(header + 16, sizeof(byte), 112, rom) != 112) {
		printf("nsf is truncated\n");
		fclose(rom);
		return -1;
	}
	fseek(rom, 0, SEEK_END);
	long data_size = ftell(rom) - 128;
	fseek(rom, 128, SEEK_SET);
	if (0x100000 < data_size)
		data_size = 0x100000;

	Famicom_nsf* nsf = &famicom->nsf;
	nsf_defaults(nsf);
	nsf->songs = header[6] ? header[6] : 1;
	nsf->song = (header[7] && header[7] <= nsf->songs) ? header[7] - 1 : 0;
	nsf->load_address = header[8] | header[9] << 8;
	nsf->init_address = header[10] | header[11] << 8;
	nsf->play_address = header[12] | header[13] << 8;
	nsf_string(nsf->title, &header[0x0E]);
	nsf_string(nsf->artist, &header[0x2E]);
	nsf_string(nsf->copyright, &header[0x4E]);
	word speed = header[0x6E] | header[0x6F] << 8;
	if (speed)
		nsf->speed[region_ntsc] = speed;
	speed = header[0x78] | header[0x79] << 8;
	if (speed)
		nsf->speed[region_pal] = nsf->speed[region_dendy] = speed;
	famicom->loaded_rom.region = nsf_region(header[0x7A]);
	memcpy(nsf->bank_init, &header[0x70], 8);
	for (int b=0; b<8; b++) {
		if (nsf->bank_init[b])
			nsf->bankswitched = true;
	}
	nsf->expansion = header[0x7B];

	byte* data = malloc(data_size > 0 ? data_size : 1);
	if (data == NULL || data_size <= 0 || fread(data, sizeof(byte), data_size, rom) != (size_t)data_size) {
		printf("nsf is truncated\n");
		free(data);
		nsf_defaults(nsf);
		fclose(rom);
		return -1;
	}
	fclose(rom);
	int result = nsf_build(famicom, data, data_size);
	free(data);
	return result;
}

static unsigned int read_le32(const byte* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned int)p[3] << 24;
}

// copies the next null terminated string out of a chunk, returns where the one after starts
static long chunk_string(const byte* chunk, long length, long at, char* out, int out_size)
{
	int n = 0;
	while (at < length && chunk[at]) {
		if (n < out_size - 1)
			out[n++] = chunk[at];
		at++;
	}
	out[n] = '\0';
	return at + 1;
}

// nsfe is a list of chunks: a little endian length, a 4 letter id, then the data
static int famicom_load_nsfe (Famicom* famicom, FILE* rom)
{
	fseek(rom, 0, SEEK_END);
	long size = ftell(rom);
	fseek(rom, 0, SEEK_SET);
	if (size < 12 || 0x200000 < size) {
		printf("nsfe is the wrong size\n");
		fclose(rom);
		return -1;
	}
	byte* file = malloc(size);
	if (file == NULL || fread(file, 1, size, rom) != (size_t)size) {
		printf("nsfe is truncated\n");
		free(file);
		fclose(rom);
		return -1;
	}
	fclose(rom);

	Famicom_nsf* nsf = &famicom->nsf;
	nsf_defaults(nsf);
	nsf->nsfe = true;
	nsf->songs = 1;
	bool have_info = false, have_end = false;
	const byte* data = NULL;
	long data_size = 0;
	long pos = 4;
	while (pos + 8 <= size) {
		unsigned int length = read_le32(file + pos);
		char id[5];
		memcpy(id, file + pos + 4, 4);
		id[4] = '\0';
		pos += 8;
		if ((long)length > size - pos) {
			printf("nsfe chunk %s runs past the end of the file\n", id);
			goto fail;
		}
		const byte* c = file + pos;
		if (strcmp(id, "INFO") == 0) {
			if (length < 8)
				goto fail;
			nsf->load_address = c[0] | c[1] << 8;
			nsf->init_address = c[2] | c[3] << 8;
			nsf->play_address = c[4] | c[5] << 8;
			famicom->loaded_rom.region = nsf_region(c[6]);
			nsf->expansion = c[7];
			if (9 <= length && c[8])
				nsf->songs = c[8];
			if (10 <= length && c[9] < nsf->songs)
				nsf->song = c[9];
			have_info = true;
		} else if (strcmp(id, "DATA") == 0) {
			data = c;
			data_size = length;
		} else if (strcmp(id, "BANK") == 0) {
			memcpy(nsf->bank_init, c, length < 8 ? length : 8);
			nsf->bankswitched = true;
		} else if (strcmp(id, "RATE") == 0) {
			// ntsc, then pal, then dendy, each optional
			if (2 <= length && (c[0] | c[1] << 8))
				nsf->speed[region_ntsc] = c[0] | c[1] << 8;
			if (4 <= length && (c[2] | c[3] << 8))
				nsf->speed[region_pal] = nsf->speed[region_dendy] = c[2] | c[3] << 8;
			if (6 <= length && (c[4] | c[5] << 8))
				nsf->speed[region_dendy] = c[4] | c[5] << 8;
		} else if (strcmp(id, "NEND") == 0) {
			have_end = true;
			break;
		} else if (strcmp(id, "auth") == 0) {
			long at = 0;
			at = chunk_string(c, length, at, nsf->title, sizeof(nsf->title));
			at = chunk_string(c, length, at, nsf->artist, sizeof(nsf->artist));
			at = chunk_string(c, length, at, nsf->copyright, sizeof(nsf->copyright));
			chunk_string(c, length, at, nsf->ripper, sizeof(nsf->ripper));
		} else if (strcmp(id, "tlbl") == 0) {
			long at = 0;
			for (int t=0; t<NSF_MAX_TRACKS && at < (long)length; t++)
				at = chunk_string(c, length, at, nsf->track_label[t], sizeof(nsf->track_label[t]));
		} else if (strcmp(id, "time") == 0) {
			for (unsigned int t=0; t<length/4 && t<NSF_MAX_TRACKS; t++)
				nsf->track_time[t] = (int)read_le32(c + t*4);
		} else if (strcmp(id, "fade") == 0) {
			for (unsigned int t=0; t<length/4 && t<NSF_MAX_TRACKS; t++)
				nsf->track_fade[t] = (int)read_le32(c + t*4);
		} else if (strcmp(id, "plst") == 0) {
			nsf->playlist_length = 0;
			for (unsigned int i=0; i<length && nsf->playlist_length < NSF_MAX_TRACKS; i++)
				nsf->playlist[nsf->playlist_length++] = c[i];
		} else if (strcmp(id, "VRC7") == 0) {
			nsf->expansion |= 0x02;
		} else if (strcmp(id, "NSF2") == 0) {
			printf("nsfe has NSF2 features, they're ignored\n");
		} else if ('A' <= id[0] && id[0] <= 'Z') {
			// the spec says chunks starting with a capital can't be skipped
			printf("nsfe needs chunk %s, which isn't supported\n", id);
			goto fail;
		}
		// lowercase chunks we don't know are optional, skip them
		pos += length;
	}
	if (!have_info || data == NULL || !have_end) {
		printf("nsfe is missing its INFO, DATA or NEND chunk\n");
		goto fail;
	}
	// a playlist decides the order and the first track, drop entries that don't exist
	int kept = 0;
	for (int i=0; i<nsf->playlist_length; i++) {
		if (nsf->playlist[i] < nsf->songs)
			nsf->playlist[kept++] = nsf->playlist[i];
	}
	nsf->playlist_length = kept;
	if (0 < kept)
		nsf->song = nsf->playlist[0];

	int result = nsf_build(famicom, data, data_size);
	free(file);
	return result;
fail:
	nsf_defaults(nsf);
	free(file);
	return -1;
}

static void nsf_call(Famicom* f, word address)
{
	// push a return address so the routine's RTS lands on the idle loop
	word ret = NSF_IDLE_ADDRESS - 1;
	f->mem[0x100 + f->cpu.reg[reg_sp]] = ret >> 8;
	f->cpu.reg[reg_sp]--;
	f->mem[0x100 + f->cpu.reg[reg_sp]] = ret & 0xFF;
	f->cpu.reg[reg_sp]--;
	f->cpu.pc = address;
}

// sets the machine up the way the nsf spec asks and calls the init routine
static void nsf_start_song(Famicom* f)
{
	Famicom_nsf* nsf = &f->nsf;
	memset(f->mem, 0, memsize_famicom);
	memset(f->prg_ram, 0, prg_ram_size);
	apu_reset(&f->apu);
	for (word addr=0x4000; addr<=0x4013; addr++)
		apu_write(f, addr, 0);
	apu_write(f, 0x4015, 0x0F);
	apu_write(f, 0x4017, 0x40);
	if (f->n163.active)
		n163_reset(&f->n163);
	if (f->vrc6.active)
		vrc6_reset(&f->vrc6);
	if (f->vrc7.active)
		vrc7_reset(&f->vrc7);
	if (f->fds.sound_only) {
		// fresh fds sound, and ram filled from the tune's data
		Famicom_fds* d = &f->fds;
		memset(d, 0, sizeof(*d));
		d->active = true;
		d->sound_only = true;
		d->inserted = -1;
		d->disk_enabled = true;
		d->sound_enabled = true;
		d->master_speed = 0xE8;
		d->mod_disabled = true;
		fds_write(f, 0x4080, 0x80);
		fds_write(f, 0x4084, 0x80);
		fds_write(f, 0x4087, 0x80);
		fds_write(f, 0x4089, 0x00);
		fds_write(f, 0x408A, 0xE8);
		memset(f->nsf_ram, 0, NSF_FDS_RAM);
		if (!nsf->bankswitched)
			memcpy(f->nsf_ram, f->prg, NSF_FDS_RAM);
	}
	if (nsf->bankswitched) {
		// fds tunes also bank $6000 and $7000, from the last two header bytes
		if (f->fds.sound_only) {
			mapper_cpu_write(f, 0x5FF6, nsf->bank_init[6]);
			mapper_cpu_write(f, 0x5FF7, nsf->bank_init[7]);
		}
		for (int b=0; b<8; b++)
			mapper_cpu_write(f, 0x5FF8 + b, nsf->bank_init[b]);
	}
	memset(f->cpu.reg, 0, sizeof(f->cpu.reg));
	f->cpu.reg[reg_a] = nsf->song;
	f->cpu.reg[reg_x] = f->region == region_ntsc ? 0 : 1; // tells the tune which tables to use
	nsf->play_period = nsf->speed[region_index(f)] * famicom_cpu_hz(f) / 1000000.0;
	f->cpu.reg[reg_sp] = 0xFD;
	f->cpu.reg[reg_p] = 0x24;
	nsf_call(f, nsf->init_address);
	f->cpu.running = true;
	nsf->next_play = f->cpu_cycles + nsf->play_period;
}

// calls play at the nsf's rate, whenever the last call has returned
static void nsf_update(Famicom* f)
{
	Famicom_nsf* nsf = &f->nsf;
	if (f->cpu.pc != NSF_IDLE_ADDRESS || f->cpu_cycles < nsf->next_play)
		return;
	nsf->next_play += nsf->play_period;
	if (nsf->next_play < f->cpu_cycles)
		nsf->next_play = f->cpu_cycles + nsf->play_period;
	nsf_call(f, nsf->play_address);
}

void famicom_nsf_select (Famicom* famicom, int song)
{
	if (!famicom->nsf.active)
		return;
	famicom->nsf.song = (song % famicom->nsf.songs + famicom->nsf.songs) % famicom->nsf.songs;
	// keep the playlist position in step when a track is picked directly
	for (int i=0; i<famicom->nsf.playlist_length; i++) {
		if (famicom->nsf.playlist[i] == famicom->nsf.song) {
			famicom->nsf.playlist_position = i;
			break;
		}
	}
	famicom->nsf.paused = false;
	famicom->nsf.stopped = false;
	famicom->nsf.frames_played = 0;
	nsf_start_song(famicom);
}

// next or previous track, following the nsfe playlist when there is one
void famicom_nsf_step (Famicom* famicom, int direction)
{
	Famicom_nsf* nsf = &famicom->nsf;
	if (!nsf->active)
		return;
	if (nsf->playlist_length == 0) {
		famicom_nsf_select(famicom, nsf->song + direction);
		return;
	}
	int position = ((nsf->playlist_position + direction) % nsf->playlist_length + nsf->playlist_length) % nsf->playlist_length;
	nsf->playlist_position = position;
	famicom_nsf_select(famicom, nsf->playlist[position]);
	nsf->playlist_position = position; // a track can be listed twice, keep the one we stepped to
}

// rewinds the current song and holds it there
void famicom_nsf_stop (Famicom* famicom)
{
	if (!famicom->nsf.active)
		return;
	famicom_nsf_select(famicom, famicom->nsf.song);
	famicom->nsf.paused = true;
	famicom->nsf.stopped = true;
}

// true when the player is paused or stopped and the machine shouldn't run
bool famicom_nsf_halted (Famicom* famicom)
{
	return famicom->nsf.active && (famicom->nsf.paused || famicom->nsf.stopped);
}

bool famicom_has_cartridge (Famicom* famicom)
{
	return famicom->prg != NULL && famicom->chr != NULL;
}

// loads a rom into the cartridge slot, a bad file leaves the current game alone
int famicom_insert_cartridge (Famicom* famicom, const char* path)
{
	FILE* rom = fopen(path, "rb");
	if (rom == NULL) {
		printf("couldn't open file\n");
		return -1;
	}
	// a zip gets its first rom pulled out into a temporary file the loaders can read
	byte magic[4] = { 0 };
	size_t got = fread(magic, 1, 4, rom);
	if (got == 4 && memcmp(magic, "PK\x03\x04", 4) == 0) {
		fclose(rom);
		int size = 0;
		char name[256];
		byte* data = zip_extract_rom(path, &size, name, sizeof(name));
		if (data == NULL)
			return -1;
		printf("loading %s from the zip\n", name);
		rom = tmpfile();
		if (rom == NULL || fwrite(data, 1, size, rom) != (size_t)size) {
			printf("couldn't unpack the zip\n");
			if (rom)
				fclose(rom);
			free(data);
			return -1;
		}
		free(data);
	}
	fseek(rom, 0, SEEK_SET);
	static Famicom cart;
	memset(&cart, 0, sizeof(cart));
	loading_path = path;
	int result = famicom_load_rom(&cart, rom);
	loading_path = "";
	if (result == -1) {
		free_rom(&cart);
		return -1;
	}
	// the old disk's saves go out before it's swapped for the new cartridge
	fds_save_disk(famicom, famicom->rom_path);
	free_rom(famicom);
	famicom->prg = cart.prg;
	famicom->chr = cart.chr;
	famicom->fds_disk = cart.fds_disk;
	famicom->nsf_ram = cart.nsf_ram;
	famicom->prg_size = cart.prg_size;
	famicom->chr_size = cart.chr_size;
	famicom->chr_is_ram = cart.chr_is_ram;
	famicom->loaded_rom = cart.loaded_rom;
	famicom->nsf = cart.nsf;
	famicom->fds = cart.fds;
	snprintf(famicom->rom_path, sizeof(famicom->rom_path), "%s", path);
	famicom->loaded_rom.name = famicom->rom_path;
	famicom_reset(famicom, false);
	return 0;
}

const word ppu_addr_start = 0x2000;
const word apu_addr_start = 0x4000;
const word unmapped_addr_start = 0x4020;

static byte controller_bits(Famicom_controller* c)
{
	return c->button_a | c->button_b << 1 | c->select << 2 | c->start << 3
		| c->up << 4 | c->down << 5 | c->left << 6 | c->right << 7;
}

static void controller_write(Famicom* f, byte value)
{
	// the shift registers reload while strobe is high and on its falling edge
	if (f->controller_strobe || (value & 1)) {
		f->controller_shift[0] = controller_bits(&f->controller_p1);
		f->controller_shift[1] = controller_bits(&f->controller_p2);
	}
	f->controller_strobe = value & 1;
}

static byte controller_read(Famicom* f, int port)
{
	if (f->controller_strobe)
		f->controller_shift[port] = controller_bits(port ? &f->controller_p2 : &f->controller_p1);
	byte bit = f->controller_shift[port] & 1;
	// official pads return 1 after the 8 buttons
	f->controller_shift[port] = (f->controller_shift[port] >> 1) | 0x80;
	return 0x40 | bit;
}

void oamdma(Famicom* f, byte value);
byte mmap_famicom(Famicom* f, word addr, byte value, bool write)
{
	 // zero page
	if (addr < ppu_addr_start) {
		if (write) {
			f->mem[addr % 0x800] = value;
			return 0;
		} else {
			return f->mem[addr % 0x800];
		}
	// PPU registers
	} else if (addr < apu_addr_start) {
		if (write) {
			ppu_register_write(f, addr, value);
			return 0;
		}
		return ppu_register_read(f, addr);
	// APU & OAMDMA
	} else if (addr < unmapped_addr_start) {
		switch (addr) {
		case 0x4014:
			if (write)
				oamdma(f, value);
			return 0;
		case 0x4015:
			if (write) {
				apu_write(f, addr, value);
				return 0;
			}
			return apu_read_status(f);
		case 0x4016:
			if (write) {
				controller_write(f, value);
				return 0;
			}
			return controller_read(f, 0);
		case 0x4017:
			if (write) {
				apu_write(f, addr, value);
				return 0;
			}
			return controller_read(f, 1);
		default:
			if (write)
				apu_write(f, addr, value);
			return 0;
		}
	// cartridge
	} else {
		if (write) {
			mapper_cpu_write(f, addr, value);
			return 0;
		}
		return mapper_cpu_read(f, addr);
	}
	return 0;
}

void oamdma(Famicom* f, byte value)
{
	word page = value << 8;
	for (int i=0; i<256; i++)
		f->ppu.oam[(f->ppu.oam_address + i) & 0xFF] = mmap_famicom(f, page + i, 0, false);
	f->dma_stall += 513 + (f->cpu_cycles & 1);
}

byte mmap_famicom_read (Famicom* famicom, word addr)
{
	return mmap_famicom(famicom, addr, 0, false);
}

void mmap_famicom_write(Famicom* famicom, word addr, byte value)
{
	mmap_famicom(famicom, addr, value, true);
}

// runs the ppu and apu alongside the cpu
static void famicom_tick(Famicom* f, int cycles)
{
	while (0 < cycles || 0 < f->dma_stall) {
		if (cycles <= 0) {
			cycles = f->dma_stall;
			f->dma_stall = 0;
		}
		if (f->fds.active)
			fds_clock(f);
		if (f->n163.active)
			n163_clock(&f->n163);
		if (f->vrc6.active)
			vrc6_clock(&f->vrc6);
		if (f->vrc7.active)
			vrc7_clock(&f->vrc7, famicom_cpu_hz(f));
		apu_step(f);
		mapper_cpu_clock(f);
		// ntsc and dendy run 3 ppu dots per cpu cycle, pal runs 3.2
		int dots = 3;
		if (f->region == region_pal) {
			f->ppu_phase += 16;
			dots = f->ppu_phase / 5;
			f->ppu_phase %= 5;
		}
		for (int d=0; d<dots; d++)
			ppu_step(f);
		f->cpu_cycles++;
		cycles--;
	}
}

Instruction i;
byte oper1;
byte oper2;
byte oper[2];
int famicom_step(Famicom* famicom, bool debug, FILE* dfh)
{
	System system;
	system.s = famicom_system;
	system.h = famicom;
	famicom->debug.nmi = false;
	famicom->debug.irq = false;
	famicom->cpu.branch_taken = false;
	i = parse(mmap_famicom(famicom, famicom->cpu.pc, 0, false));
	if (i.n == unimplemented) {
		printf("unimplemented opcode %X!\n", mmap_famicom_read(famicom, famicom->cpu.pc));
		famicom->cpu.running = false;
		return 0;
	}
	oper1 = mmap_famicom(famicom, famicom->cpu.pc + 1, 0, false);
	oper2 = 0;
	switch (i.a) {
	default:
		break;
	case absolute:
	case absolute_indirect:
	case absolute_x:
	case absolute_y:
		oper2 = mmap_famicom(famicom, famicom->cpu.pc + 2, 0, false);
		break;
	}
	oper[0] = oper1;
	oper[1] = oper2;
	//printf("%s %X %X pc: %X sp: %X\n", i.m, oper1, oper2, famicom->cpu.pc, famicom->cpu.reg[reg_sp]);
	step(system, &famicom->cpu, i, oper);
	famicom_tick(famicom, famicom->cpu.cycles_last);
	if (famicom->nsf.active)
		nsf_update(famicom);
	if (famicom->ppu.nmi_pending) {
		famicom->ppu.nmi_pending = false;
		nmi(system, &famicom->cpu);
		famicom_tick(famicom, 7);
		famicom->debug.nmi = true;
	} else if ((mapper_irq(famicom) || apu_irq(famicom)) && !get_bit(famicom->cpu.reg[reg_p], interrupt_disable)) {
		irq(system, &famicom->cpu);
		famicom_tick(famicom, 7);
		famicom->debug.irq = true;
	}
	if (debug)
		write_cpu_state(&famicom->cpu, system, dfh);
	return 1;
}

static const char save_magic[8] = "NEMUSAV5";

void famicom_save_state(Famicom* famicom)
{
	if (!famicom_has_cartridge(famicom))
		return;
	FILE* savefile = fopen("000.sav", "wb");
	if (savefile == NULL) {
		printf("couldn't save file\n");
		return;
	}
	fwrite(save_magic, sizeof(save_magic), 1, savefile);
	fwrite(&famicom->loaded_rom.mapper, sizeof(int), 1, savefile);
	fwrite(&famicom->prg_size, sizeof(int), 1, savefile);
	fwrite(&famicom->chr_size, sizeof(int), 1, savefile);
	fwrite(&famicom->cpu, sizeof(famicom->cpu), 1, savefile);
	fwrite(famicom->mem, memsize_famicom, 1, savefile);
	fwrite(&famicom->ppu, sizeof(famicom->ppu), 1, savefile);
	fwrite(&famicom->apu, sizeof(famicom->apu), 1, savefile);
	fwrite(&famicom->mapper, sizeof(famicom->mapper), 1, savefile);
	fwrite(famicom->prg_ram, prg_ram_size, 1, savefile);
	if (famicom->chr_is_ram)
		fwrite(famicom->chr, famicom->chr_size, 1, savefile);
	fwrite(&famicom->cpu_cycles, sizeof(long), 1, savefile);
	fwrite(&famicom->n163, sizeof(famicom->n163), 1, savefile);
	fwrite(&famicom->vrc6, sizeof(famicom->vrc6), 1, savefile);
	fwrite(&famicom->vrc7, sizeof(famicom->vrc7), 1, savefile);
	if (famicom->nsf_ram) {
		fwrite(&famicom->fds, sizeof(famicom->fds), 1, savefile);
		fwrite(famicom->nsf_ram, NSF_FDS_RAM, 1, savefile);
	}
	if (famicom->fds.active && famicom->fds_disk) {
		// the ram adapter's ram lives in prg, and the disk may have been written to
		fwrite(&famicom->fds, sizeof(famicom->fds), 1, savefile);
		fwrite(famicom->prg, famicom->prg_size, 1, savefile);
		fwrite(famicom->fds_disk, FDS_RAW_SIDE, famicom->fds.sides, savefile);
	}
	fclose(savefile);
	printf("saved\n");
}

void famicom_load_state(Famicom* famicom)
{
	if (!famicom_has_cartridge(famicom))
		return;
	FILE* savefile = fopen("000.sav", "rb");
	if (savefile == NULL) {
		printf("couldn't read save\n");
		return;
	}
	char magic[8];
	int mapper, prg_size, chr_size;
	if (fread(magic, sizeof(magic), 1, savefile) != 1 || memcmp(magic, save_magic, sizeof(magic)) != 0
	    || fread(&mapper, sizeof(int), 1, savefile) != 1
	    || fread(&prg_size, sizeof(int), 1, savefile) != 1
	    || fread(&chr_size, sizeof(int), 1, savefile) != 1
	    || mapper != famicom->loaded_rom.mapper || prg_size != famicom->prg_size || chr_size != famicom->chr_size) {
		printf("save is from another rom or an older version\n");
		fclose(savefile);
		return;
	}
	// load into a copy so a short file can't leave the machine half restored
	static Famicom loaded;
	loaded = *famicom;
	byte mem[0x800];
	byte prg_ram[0x2000];
	bool ok = fread(&loaded.cpu, sizeof(loaded.cpu), 1, savefile) == 1
		&& fread(mem, memsize_famicom, 1, savefile) == 1
		&& fread(&loaded.ppu, sizeof(loaded.ppu), 1, savefile) == 1
		&& fread(&loaded.apu, sizeof(loaded.apu), 1, savefile) == 1
		&& fread(&loaded.mapper, sizeof(loaded.mapper), 1, savefile) == 1
		&& fread(prg_ram, prg_ram_size, 1, savefile) == 1;
	byte* chr = NULL;
	if (ok && famicom->chr_is_ram) {
		chr = malloc(famicom->chr_size);
		ok = chr != NULL && fread(chr, famicom->chr_size, 1, savefile) == 1;
	}
	ok = ok && fread(&loaded.cpu_cycles, sizeof(long), 1, savefile) == 1;
	ok = ok && fread(&loaded.n163, sizeof(loaded.n163), 1, savefile) == 1;
	ok = ok && fread(&loaded.vrc6, sizeof(loaded.vrc6), 1, savefile) == 1;
	ok = ok && fread(&loaded.vrc7, sizeof(loaded.vrc7), 1, savefile) == 1;
	byte* nsf_ram = NULL;
	if (ok && famicom->nsf_ram) {
		nsf_ram = malloc(NSF_FDS_RAM);
		ok = nsf_ram && fread(&loaded.fds, sizeof(loaded.fds), 1, savefile) == 1
			&& fread(nsf_ram, NSF_FDS_RAM, 1, savefile) == 1;
	}
	byte* fds_prg = NULL;
	byte* fds_disk = NULL;
	if (ok && famicom->fds.active && famicom->fds_disk) {
		fds_prg = malloc(famicom->prg_size);
		fds_disk = malloc((size_t)FDS_RAW_SIDE * famicom->fds.sides);
		ok = fds_prg && fds_disk
			&& fread(&loaded.fds, sizeof(loaded.fds), 1, savefile) == 1
			&& loaded.fds.sides == famicom->fds.sides
			&& fread(fds_prg, famicom->prg_size, 1, savefile) == 1
			&& fread(fds_disk, FDS_RAW_SIDE, famicom->fds.sides, savefile) == (size_t)famicom->fds.sides;
	}
	fclose(savefile);
	if (!ok) {
		printf("save is corrupt\n");
		free(chr);
		free(fds_prg);
		free(fds_disk);
		free(nsf_ram);
		return;
	}
	// expansion chip state comes from the file too, keep its indexes in range
	loaded.n163.active = famicom->n163.active;
	loaded.n163.address &= 0x7F;
	loaded.vrc6.active = famicom->vrc6.active;
	for (int i=0; i<2; i++)
		loaded.vrc6.pulse[i].step &= 0x0F;
	if (13 < loaded.vrc6.saw_step)
		loaded.vrc6.saw_step = 0;
	loaded.vrc7.active = famicom->vrc7.active;
	loaded.vrc7.address &= 0x3F;
	for (int c=0; c<6; c++) {
		for (int o=0; o<2; o++) {
			Vrc7_operator* op = &loaded.vrc7.op[c][o];
			if (op->state < 0 || 4 < op->state)
				op->state = 0;
			if (!(0 <= op->envelope && op->envelope <= 48))
				op->envelope = 48;
			if (!(0 <= op->phase && op->phase < 1))
				op->phase = 0;
		}
	}
	if (loaded.n163.channel < 0 || 7 < loaded.n163.channel)
		loaded.n163.channel = 0;
	if (famicom->fds.active && famicom->fds_disk) {
		Famicom_fds* d = &loaded.fds;
		d->active = true;
		if (d->inserted < -1 || d->sides <= d->inserted)
			d->inserted = -1;
		if (d->pending_side < 0 || d->sides <= d->pending_side)
			d->pending_side = 0;
		for (int s=0; s<FDS_MAX_SIDES; s++) {
			if (d->side_length[s] < 1 || FDS_RAW_SIDE < d->side_length[s])
				d->side_length[s] = FDS_RAW_SIDE;
		}
		if (d->position < 0 || FDS_RAW_SIDE <= d->position)
			d->position = 0;
		d->wave_position &= 0x3F;
		d->mod_position &= 0x3F;
		for (int i=0; i<64; i++)
			d->mod_table[i] &= 0x07;
		memcpy(famicom->prg, fds_prg, famicom->prg_size);
		memcpy(famicom->fds_disk, fds_disk, (size_t)FDS_RAW_SIDE * famicom->fds.sides);
		free(fds_prg);
		free(fds_disk);
	} else if (nsf_ram) {
		// an fds nsf: sound state and ram, no drive
		loaded.fds.active = true;
		loaded.fds.sound_only = true;
		loaded.fds.inserted = -1;
		loaded.fds.wave_position &= 0x3F;
		loaded.fds.mod_position &= 0x3F;
		for (int i=0; i<64; i++)
			loaded.fds.mod_table[i] &= 0x07;
		memcpy(famicom->nsf_ram, nsf_ram, NSF_FDS_RAM);
		free(nsf_ram);
	} else {
		loaded.fds = famicom->fds;
	}
	// bank offsets come from the file, keep them inside the rom
	for (int b=0; b<4; b++)
		loaded.mapper.prg_banks[b] = ((loaded.mapper.prg_banks[b] / 0x2000) % (prg_size / 0x2000)) * 0x2000;
	for (int b=0; b<8; b++)
		loaded.mapper.chr_banks[b] = ((loaded.mapper.chr_banks[b] / 0x400) % (chr_size / 0x400)) * 0x400;
	for (int b=0; b<4; b++) {
		if (loaded.mapper.prg_banks[b] < 0)
			loaded.mapper.prg_banks[b] = 0;
	}
	for (int b=0; b<8; b++) {
		if (loaded.mapper.chr_banks[b] < 0)
			loaded.mapper.chr_banks[b] = 0;
	}
	for (int b=0; b<8; b++) {
		loaded.mapper.nsf_banks[b] = ((loaded.mapper.nsf_banks[b] / 0x1000) % (prg_size / 0x1000)) * 0x1000;
		if (loaded.mapper.nsf_banks[b] < 0)
			loaded.mapper.nsf_banks[b] = 0;
	}
	loaded.nsf.next_play = loaded.cpu_cycles;
	if (0 <= loaded.mapper.prg_ram_bank)
		loaded.mapper.prg_ram_bank = ((loaded.mapper.prg_ram_bank / 0x2000) % (prg_size / 0x2000)) * 0x2000;
	else
		loaded.mapper.prg_ram_bank = -1;
	if (loaded.ppu.sprite_count < 0 || 8 < loaded.ppu.sprite_count)
		loaded.ppu.sprite_count = 0;
	if (loaded.ppu.scanline < 0 || famicom_last_line(famicom) < loaded.ppu.scanline || loaded.ppu.dot < 0 || 340 < loaded.ppu.dot) {
		loaded.ppu.scanline = 0;
		loaded.ppu.dot = 0;
	}
	// pointers in the file are meaningless now
	loaded.cpu.current_instruction_name = " ";
	loaded.cpu.current_instruction = parse(0xEA);
	loaded.loaded_rom = famicom->loaded_rom;
	*famicom = loaded;
	memcpy(famicom->mem, mem, memsize_famicom);
	memcpy(famicom->prg_ram, prg_ram, prg_ram_size);
	if (chr) {
		memcpy(famicom->chr, chr, famicom->chr_size);
		free(chr);
	}
	printf("loaded\n");
}
