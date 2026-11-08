#include <stdio.h>
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

// the irq counter shared by konami's vrc chips: counts scanlines through a
// prescaler, or cpu cycles, and fires when it wraps past $FF
static void vrc_irq_control(Famicom_mapper* m, byte value)
{
	m->irq_enable_after_ack = value & 0x01;
	m->irq_enable = value & 0x02;
	m->irq_cycle_mode = value & 0x04;
	if (m->irq_enable) {
		m->irq_counter = m->irq_latch;
		m->irq_prescaler = 341;
	}
	m->irq_pending = false;
}

static void vrc_irq_ack(Famicom_mapper* m)
{
	m->irq_pending = false;
	m->irq_enable = m->irq_enable_after_ack;
}

static void vrc_irq_clock(Famicom_mapper* m)
{
	if (!m->irq_enable)
		return;
	if (!m->irq_cycle_mode) {
		// a scanline is 113.67 cpu cycles, the prescaler counts it in thirds
		m->irq_prescaler -= 3;
		if (0 < m->irq_prescaler)
			return;
		m->irq_prescaler += 341;
	}
	if (m->irq_counter == 0xFF) {
		m->irq_counter = m->irq_latch;
		m->irq_pending = true;
	} else {
		m->irq_counter++;
	}
}

static int vrc_mirroring(byte value)
{
	switch (value & 0x03) {
	case 0: return mirror_vertical;
	case 1: return mirror_horizontal;
	case 2: return mirror_single_lower;
	default: return mirror_single_upper;
	}
}

// bank numbers wrap around the rom size, negative banks count from the end
static void set_prg_8k(Famicom* f, int slot, int bank)
{
	int count = f->prg_size / 0x2000;
	bank %= count;
	if (bank < 0)
		bank += count;
	f->mapper.prg_banks[slot] = bank * 0x2000;
}

static void set_prg_16k(Famicom* f, int slot, int bank)
{
	set_prg_8k(f, slot*2, bank*2);
	set_prg_8k(f, slot*2 + 1, bank*2 + 1);
}

static void set_prg_32k(Famicom* f, int bank)
{
	for (int i=0; i<4; i++)
		set_prg_8k(f, i, bank*4 + i);
}

static void set_chr_1k(Famicom* f, int slot, int bank)
{
	int count = f->chr_size / 0x400;
	bank %= count;
	if (bank < 0)
		bank += count;
	f->mapper.chr_banks[slot] = bank * 0x400;
}

static void set_chr_4k(Famicom* f, int slot, int bank)
{
	for (int i=0; i<4; i++)
		set_chr_1k(f, slot*4 + i, bank*4 + i);
}

static void set_chr_8k(Famicom* f, int bank)
{
	for (int i=0; i<8; i++)
		set_chr_1k(f, i, bank*8 + i);
}

static void mmc1_update(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	byte control = m->regs[0];
	switch (control & 0x03) {
	case 0: m->mirroring = mirror_single_lower; break;
	case 1: m->mirroring = mirror_single_upper; break;
	case 2: m->mirroring = mirror_vertical; break;
	case 3: m->mirroring = mirror_horizontal; break;
	}
	// SUROM uses a chr register bit to pick the 256KB prg half
	int outer = 0;
	if (f->prg_size == 0x80000)
		outer = m->regs[1] & 0x10;
	int prg = (m->regs[3] & 0x0F) | outer;
	switch ((control >> 2) & 0x03) {
	case 0:
	case 1:
		set_prg_16k(f, 0, prg & ~1);
		set_prg_16k(f, 1, prg | 1);
		break;
	case 2:
		set_prg_16k(f, 0, outer);
		set_prg_16k(f, 1, prg);
		break;
	case 3:
		set_prg_16k(f, 0, prg);
		set_prg_16k(f, 1, 0x0F | outer);
		break;
	}
	if (control & 0x10) {
		set_chr_4k(f, 0, m->regs[1]);
		set_chr_4k(f, 1, m->regs[2]);
	} else {
		set_chr_8k(f, m->regs[1] >> 1);
	}
}

static void mmc3_update(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	byte* r = m->regs;
	if (m->bank_select & 0x40) {
		set_prg_8k(f, 0, -2);
		set_prg_8k(f, 2, r[6]);
	} else {
		set_prg_8k(f, 0, r[6]);
		set_prg_8k(f, 2, -2);
	}
	set_prg_8k(f, 1, r[7]);
	set_prg_8k(f, 3, -1);
	int inv = (m->bank_select & 0x80) ? 4 : 0;
	set_chr_1k(f, 0 ^ inv, r[0] & 0xFE);
	set_chr_1k(f, 1 ^ inv, r[0] | 1);
	set_chr_1k(f, 2 ^ inv, r[1] & 0xFE);
	set_chr_1k(f, 3 ^ inv, r[1] | 1);
	set_chr_1k(f, 4 ^ inv, r[2]);
	set_chr_1k(f, 5 ^ inv, r[3]);
	set_chr_1k(f, 6 ^ inv, r[4]);
	set_chr_1k(f, 7 ^ inv, r[5]);
}

static void mmc2_update(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	set_chr_4k(f, 0, m->latch[0] == 0xFD ? m->regs[0] : m->regs[1]);
	set_chr_4k(f, 1, m->latch[1] == 0xFD ? m->regs[2] : m->regs[3]);
}

int mapper_init(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	memset(m, 0, sizeof(*m));
	if (f->prg_size < 0x2000 || f->chr_size < 0x400)
		return -1;
	m->id = f->loaded_rom.mapper;
	m->mirroring = f->loaded_rom.mirroring;
	m->prg_ram_bank = -1;
	// expansion sound is off unless the board or the nsf has the chip
	f->n163.active = false;
	f->vrc6.active = false;
	f->vrc7.active = false;
	set_prg_32k(f, 0);
	set_chr_8k(f, 0);
	switch (m->id) {
	case 0: // NROM
	case 2: // UxROM
	case 71: // Camerica
	case 94: // UN1ROM
		set_prg_16k(f, 0, 0);
		set_prg_16k(f, 1, -1);
		break;
	case 180: // UNROM with the fixed bank at $8000
		set_prg_16k(f, 0, 0);
		set_prg_16k(f, 1, 0);
		break;
	case 1: // MMC1
		m->shift = 0x10;
		m->regs[0] = 0x0C;
		mmc1_update(f);
		break;
	case 4: // MMC3
	case 206: // Namco 108
		m->regs[0] = 0; m->regs[1] = 2;
		m->regs[2] = 4; m->regs[3] = 5;
		m->regs[4] = 6; m->regs[5] = 7;
		m->regs[6] = 0; m->regs[7] = 1;
		mmc3_update(f);
		break;
	case 7: // AxROM
		m->mirroring = mirror_single_lower;
		break;
	case 9: // MMC2
		set_prg_8k(f, 0, 0);
		set_prg_8k(f, 1, -3);
		set_prg_8k(f, 2, -2);
		set_prg_8k(f, 3, -1);
		m->latch[0] = 0xFE;
		m->latch[1] = 0xFE;
		mmc2_update(f);
		break;
	case 10: // MMC4
		set_prg_16k(f, 0, 0);
		set_prg_16k(f, 1, -1);
		m->latch[0] = 0xFE;
		m->latch[1] = 0xFE;
		mmc2_update(f);
		break;
	case 22: // VRC2a
		set_prg_8k(f, 0, 0);
		set_prg_8k(f, 1, 1);
		set_prg_8k(f, 2, -2);
		set_prg_8k(f, 3, -1);
		break;
	case 40: // SMB2j conversion, prg bank 6 shows up at $6000
		m->prg_ram_bank = (6 % (f->prg_size / 0x2000)) * 0x2000;
		set_prg_8k(f, 0, 4);
		set_prg_8k(f, 1, 5);
		set_prg_8k(f, 2, 0);
		set_prg_8k(f, 3, 7);
		break;
	case MAPPER_NSF:
		// plain nsfs get their 32KB image mapped straight, banked ones are set up by the player
		for (int i=0; i<8; i++)
			m->nsf_banks[i] = (i % (f->prg_size / 0x1000)) * 0x1000;
		f->n163.active = f->nsf.expansion & NSF_N163;
		n163_reset(&f->n163);
		f->vrc6.active = f->nsf.expansion & NSF_VRC6;
		vrc6_reset(&f->vrc6);
		f->vrc7.active = f->nsf.expansion & NSF_VRC7;
		vrc7_reset(&f->vrc7);
		// fds tunes get the fds sound chip without the drive, set up per song
		memset(&f->fds, 0, sizeof(f->fds));
		if ((f->nsf.expansion & NSF_FDS) && f->nsf_ram) {
			f->fds.active = true;
			f->fds.sound_only = true;
			f->fds.inserted = -1;
		}
		break;
	case MAPPER_FDS:
		break;
	case 24: // vrc6a
	case 26: // vrc6b
		f->vrc6.active = true;
		vrc6_reset(&f->vrc6);
		set_prg_16k(f, 0, 0);
		set_prg_8k(f, 2, -2);
		set_prg_8k(f, 3, -1);
		break;
	case 85: // vrc7
		f->vrc7.active = true;
		vrc7_reset(&f->vrc7);
		set_prg_8k(f, 0, 0);
		set_prg_8k(f, 1, 1);
		set_prg_8k(f, 2, -2);
		set_prg_8k(f, 3, -1);
		break;
	case 19: // namco 163
		memset(&f->n163, 0, sizeof(f->n163));
		f->n163.active = true;
		n163_reset(&f->n163);
		set_prg_8k(f, 0, 0);
		set_prg_8k(f, 1, 1);
		set_prg_8k(f, 2, 2);
		set_prg_8k(f, 3, -1);
		for (int i=0; i<4; i++)
			f->n163.nt_regs[i] = 0xE0 | (m->mirroring == mirror_vertical ? i & 1 : i >> 1);
		break;
	case 3: // CNROM
	case 11: // Color Dreams
	case 13: // CPROM
	case 34: // BNROM / NINA-001
	case 66: // GxROM
	case 79: // NINA-03/06
		break;
	default:
		return -1;
	}
	return 0;
}

// JMP NSF_IDLE_ADDRESS
static const byte nsf_driver[3] = { 0x4C, NSF_IDLE_ADDRESS & 0xFF, NSF_IDLE_ADDRESS >> 8 };

byte mapper_cpu_read(Famicom* f, word addr)
{
	if (f->mapper.id == MAPPER_NSF) {
		if (f->nsf_ram) {
			// fds tunes run entirely from ram
			if (0x6000 <= addr)
				return f->nsf_ram[addr - 0x6000];
			if (0x4040 <= addr && addr < 0x4100)
				return fds_read(f, addr);
		}
		if (0x8000 <= addr)
			return f->prg[f->mapper.nsf_banks[(addr - 0x8000) >> 12] + (addr & 0x0FFF)];
		if (NSF_IDLE_ADDRESS <= addr && addr < NSF_IDLE_ADDRESS + sizeof(nsf_driver))
			return nsf_driver[addr - NSF_IDLE_ADDRESS];
		if (f->n163.active && 0x4800 <= addr && addr < 0x5000)
			return n163_read_data(&f->n163);
	}
	if (f->mapper.id == MAPPER_FDS) {
		// 32KB of ram from $6000 with the bios above it
		if (0xE000 <= addr)
			return f->prg[0x8000 + (addr - 0xE000)];
		if (0x6000 <= addr)
			return f->prg[addr - 0x6000];
		if (addr < 0x4100)
			return fds_read(f, addr);
		return addr >> 8;
	}
	if (f->mapper.id == 19) {
		switch (addr & 0xF800) {
		case 0x4800: return n163_read_data(&f->n163);
		case 0x5000: return f->n163.irq_counter & 0xFF;
		case 0x5800: return (f->n163.irq_counter >> 8) | (f->n163.irq_enabled ? 0x80 : 0);
		}
	}
	if (0x8000 <= addr)
		return f->prg[f->mapper.prg_banks[(addr - 0x8000) >> 13] + (addr & 0x1FFF)];
	if (0x6000 <= addr) {
		if (0 <= f->mapper.prg_ram_bank)
			return f->prg[f->mapper.prg_ram_bank + (addr & 0x1FFF)];
		return f->prg_ram[addr - 0x6000];
	}
	return addr >> 8; // open bus
}

void mapper_cpu_write(Famicom* f, word addr, byte value)
{
	Famicom_mapper* m = &f->mapper;
	if (m->id == MAPPER_FDS) {
		if (0x6000 <= addr && addr < 0xE000)
			f->prg[addr - 0x6000] = value;
		else if (addr < 0x4100)
			fds_write(f, addr, value);
		return;
	}
	if (0x6000 <= addr && addr < 0x8000)
		f->prg_ram[addr - 0x6000] = value;

	switch (m->id) {
	case 0:
		break;
	case 1:
		if (addr < 0x8000)
			break;
		if (value & 0x80) {
			m->shift = 0x10;
			m->regs[0] |= 0x0C;
			mmc1_update(f);
			break;
		}
		bool full = m->shift & 1;
		m->shift = (m->shift >> 1) | ((value & 1) << 4);
		if (full) {
			m->regs[(addr >> 13) & 0x03] = m->shift;
			m->shift = 0x10;
			mmc1_update(f);
		}
		break;
	case 2:
		if (0x8000 <= addr)
			set_prg_16k(f, 0, value);
		break;
	case 3:
		if (0x8000 <= addr)
			set_chr_8k(f, value);
		break;
	case 4:
	case 206:
		if (addr < 0x8000)
			break;
		if (m->id == 206 && 0x8001 < (addr & 0xE001))
			break;
		switch (addr & 0xE001) {
		case 0x8000:
			m->bank_select = (m->id == 206) ? (value & 0x07) : value;
			mmc3_update(f);
			break;
		case 0x8001:
			m->regs[m->bank_select & 0x07] = (m->id == 206) ? (value & 0x3F) : value;
			mmc3_update(f);
			break;
		case 0xA000:
			if (f->loaded_rom.mirroring != mirror_four_screen)
				m->mirroring = (value & 1) ? mirror_horizontal : mirror_vertical;
			break;
		case 0xC000:
			m->irq_latch = value;
			break;
		case 0xC001:
			m->irq_counter = 0;
			m->irq_reload = true;
			break;
		case 0xE000:
			m->irq_enable = false;
			m->irq_pending = false;
			break;
		case 0xE001:
			m->irq_enable = true;
			break;
		}
		break;
	case 7:
		if (0x8000 <= addr) {
			set_prg_32k(f, value & 0x07);
			m->mirroring = (value & 0x10) ? mirror_single_upper : mirror_single_lower;
		}
		break;
	case 9:
	case 10:
		switch (addr & 0xF000) {
		case 0xA000:
			if (m->id == 9)
				set_prg_8k(f, 0, value & 0x0F);
			else
				set_prg_16k(f, 0, value & 0x0F);
			break;
		case 0xB000: m->regs[0] = value & 0x1F; mmc2_update(f); break;
		case 0xC000: m->regs[1] = value & 0x1F; mmc2_update(f); break;
		case 0xD000: m->regs[2] = value & 0x1F; mmc2_update(f); break;
		case 0xE000: m->regs[3] = value & 0x1F; mmc2_update(f); break;
		case 0xF000:
			m->mirroring = (value & 1) ? mirror_horizontal : mirror_vertical;
			break;
		}
		break;
	case 11:
		if (0x8000 <= addr) {
			set_prg_32k(f, value & 0x03);
			set_chr_8k(f, value >> 4);
		}
		break;
	case 13:
		if (0x8000 <= addr)
			set_chr_4k(f, 1, value & 0x03);
		break;
	case 22:
		if (addr < 0x8000)
			break;
		{
			// VRC2a swaps address lines A0 and A1
			int reg = ((addr >> 1) & 1) | ((addr << 1) & 2);
			switch (addr & 0xF000) {
			case 0x8000:
				set_prg_8k(f, 0, value & 0x1F);
				break;
			case 0x9000:
				m->mirroring = (value & 1) ? mirror_horizontal : mirror_vertical;
				break;
			case 0xA000:
				set_prg_8k(f, 1, value & 0x1F);
				break;
			case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
				int slot = ((addr - 0xB000) >> 12) * 2 + (reg >> 1);
				if (reg & 1)
					m->regs[slot] = (m->regs[slot] & 0x0F) | ((value & 0x0F) << 4);
				else
					m->regs[slot] = (m->regs[slot] & 0xF0) | (value & 0x0F);
				set_chr_1k(f, slot, m->regs[slot] >> 1);
				break;
			}
			}
		}
		break;
	case 40:
		switch (addr & 0xE000) {
		case 0x8000:
			m->irq_enable = false;
			m->irq_pending = false;
			m->irq_cycles = 0;
			break;
		case 0xA000:
			m->irq_enable = true;
			break;
		case 0xE000:
			set_prg_8k(f, 2, value & 0x07);
			break;
		}
		break;
	case 34:
		if (!f->chr_is_ram && 0x2000 < f->chr_size) {
			// NINA-001 registers sit on top of prg ram
			if (addr == 0x7FFD)
				set_prg_32k(f, value & 0x01);
			else if (addr == 0x7FFE)
				set_chr_4k(f, 0, value & 0x0F);
			else if (addr == 0x7FFF)
				set_chr_4k(f, 1, value & 0x0F);
		} else if (0x8000 <= addr) {
			set_prg_32k(f, value);
		}
		break;
	case 66:
		if (0x8000 <= addr) {
			set_prg_32k(f, (value >> 4) & 0x03);
			set_chr_8k(f, value & 0x03);
		}
		break;
	case 71:
		if (0xC000 <= addr)
			set_prg_16k(f, 0, value);
		else if ((addr & 0xF000) == 0x9000)
			m->mirroring = (value & 0x10) ? mirror_single_upper : mirror_single_lower;
		break;
	case 79:
		if ((addr & 0xE100) == 0x4100) {
			set_prg_32k(f, (value >> 3) & 0x01);
			set_chr_8k(f, value & 0x07);
		}
		break;
	case 94:
		if (0x8000 <= addr)
			set_prg_16k(f, 0, value >> 2);
		break;
	case MAPPER_NSF:
		if (f->nsf_ram) {
			if (0x6000 <= addr) {
				f->nsf_ram[addr - 0x6000] = value;
			} else if (f->nsf.bankswitched && 0x5FF6 <= addr && addr <= 0x5FFF) {
				// banking copies a 4KB page into ram, $5FF6 is $6000 up to $5FFF at $F000
				int bank = value % (f->prg_size / 0x1000);
				memcpy(f->nsf_ram + (addr - 0x5FF6) * 0x1000, f->prg + bank * 0x1000, 0x1000);
			} else if (0x4040 <= addr && addr < 0x4100) {
				fds_write(f, addr, value);
			}
		}
		if (f->nsf.bankswitched && 0x5FF8 <= addr && addr <= 0x5FFF)
			m->nsf_banks[addr - 0x5FF8] = (value % (f->prg_size / 0x1000)) * 0x1000;
		if (f->n163.active) {
			if (0x4800 <= addr && addr < 0x5000)
				n163_write_data(&f->n163, value);
			else if (0xF800 <= addr)
				n163_write_address(&f->n163, value);
		}
		if (f->vrc6.active && 0x9000 <= addr && addr <= 0xB002 && (addr & 0x0FFF) <= 3)
			vrc6_write(&f->vrc6, addr, value);
		if (f->vrc7.active) {
			if (addr == 0x9010)
				vrc7_write_address(&f->vrc7, value);
			else if (addr == 0x9030)
				vrc7_write_data(&f->vrc7, value);
		}
		break;
	case 24:
	case 26: {
		if (addr < 0x8000)
			break;
		// vrc6b has the two low address lines swapped
		word a = addr;
		if (m->id == 26)
			a = (addr & 0xFFFC) | ((addr & 0x01) << 1) | ((addr & 0x02) >> 1);
		switch (a & 0xF003) {
		case 0x8000: case 0x8001: case 0x8002: case 0x8003:
			set_prg_16k(f, 0, value & 0x0F);
			break;
		case 0x9000: case 0x9001: case 0x9002: case 0x9003:
		case 0xA000: case 0xA001: case 0xA002:
		case 0xB000: case 0xB001: case 0xB002:
			vrc6_write(&f->vrc6, a & 0xF003, value);
			break;
		case 0xB003:
			m->mirroring = vrc_mirroring(value >> 2);
			break;
		case 0xC000: case 0xC001: case 0xC002: case 0xC003:
			set_prg_8k(f, 2, value & 0x1F);
			break;
		case 0xD000: case 0xD001: case 0xD002: case 0xD003:
			set_chr_1k(f, a & 0x03, value);
			break;
		case 0xE000: case 0xE001: case 0xE002: case 0xE003:
			set_chr_1k(f, 4 + (a & 0x03), value);
			break;
		case 0xF000:
			m->irq_latch = value;
			break;
		case 0xF001:
			vrc_irq_control(m, value);
			break;
		case 0xF002:
			vrc_irq_ack(m);
			break;
		}
		break;
	}
	case 85: {
		if (addr < 0x8000)
			break;
		// vrc7a picks its second registers with A4, vrc7b with A3
		bool second = addr & 0x18;
		switch (addr & 0xF000) {
		case 0x8000:
			set_prg_8k(f, second ? 1 : 0, value & 0x3F);
			break;
		case 0x9000:
			if ((addr & 0x38) == 0)
				set_prg_8k(f, 2, value & 0x3F);
			else if (addr & 0x20)
				vrc7_write_data(&f->vrc7, value);
			else
				vrc7_write_address(&f->vrc7, value);
			break;
		case 0xA000: case 0xB000: case 0xC000: case 0xD000:
			set_chr_1k(f, ((addr >> 12) - 0xA) * 2 + (second ? 1 : 0), value);
			break;
		case 0xE000:
			if (second) {
				m->irq_latch = value;
			} else {
				m->mirroring = vrc_mirroring(value);
				f->vrc7.silenced = value & 0x40;
			}
			break;
		case 0xF000:
			if (second)
				vrc_irq_ack(m);
			else
				vrc_irq_control(m, value);
			break;
		}
		break;
	}
	case 19: {
		Famicom_n163* n = &f->n163;
		switch (addr & 0xF800) {
		case 0x4800:
			n163_write_data(n, value);
			break;
		case 0x5000:
			n->irq_counter = (n->irq_counter & 0x7F00) | value;
			n->irq_pending = false;
			break;
		case 0x5800:
			n->irq_counter = (n->irq_counter & 0x00FF) | (value & 0x7F) << 8;
			n->irq_enabled = value & 0x80;
			n->irq_pending = false;
			break;
		case 0x8000: case 0x8800: case 0x9000: case 0x9800:
		case 0xA000: case 0xA800: case 0xB000: case 0xB800: {
			int slot = (addr - 0x8000) >> 11;
			n->chr_regs[slot] = value;
			set_chr_1k(f, slot, value);
			break;
		}
		case 0xC000: case 0xC800: case 0xD000: case 0xD800:
			n->nt_regs[(addr - 0xC000) >> 11] = value;
			break;
		case 0xE000:
			set_prg_8k(f, 0, value & 0x3F);
			n->sound_disabled = value & 0x40;
			break;
		case 0xE800:
			set_prg_8k(f, 1, value & 0x3F);
			n->ciram_low_disabled = value & 0x40;
			n->ciram_high_disabled = value & 0x80;
			break;
		case 0xF000:
			set_prg_8k(f, 2, value & 0x3F);
			break;
		case 0xF800:
			n163_write_address(n, value);
			break;
		}
		break;
	}
	case 180:
		if (0x8000 <= addr)
			set_prg_16k(f, 1, value & 0x07);
		break;
	}
}

// n163 chr banks $E0 and up show the console's nametable ram instead of chr rom
static byte* n163_ciram(Famicom* f, word addr)
{
	int slot = (addr >> 10) & 7;
	byte reg = f->n163.chr_regs[slot];
	bool disabled = slot < 4 ? f->n163.ciram_low_disabled : f->n163.ciram_high_disabled;
	if (reg < 0xE0 || disabled)
		return NULL;
	return &f->ppu.vram[(reg & 1) * 0x400 + (addr & 0x3FF)];
}

// n163 nametables can come from chr rom, returns false for the usual nametable ram
bool mapper_nametable(Famicom* f, word addr, byte* value, bool write)
{
	if (f->mapper.id != 19)
		return false;
	byte reg = f->n163.nt_regs[(addr >> 10) & 3];
	if (0xE0 <= reg) {
		byte* ram = &f->ppu.vram[(reg & 1) * 0x400 + (addr & 0x3FF)];
		if (write)
			*ram = *value;
		else
			*value = *ram;
		return true;
	}
	if (!write)
		*value = f->chr[(reg % (f->chr_size / 0x400)) * 0x400 + (addr & 0x3FF)];
	return true;
}

byte mapper_chr_read(Famicom* f, word addr)
{
	Famicom_mapper* m = &f->mapper;
	addr &= 0x1FFF;
	if (m->id == 19) {
		byte* ram = n163_ciram(f, addr);
		if (ram)
			return *ram;
	}
	byte value = f->chr[m->chr_banks[addr >> 10] + (addr & 0x3FF)];
	if (m->id == 9 || m->id == 10) {
		// the latches flip after the ppu fetches tile $FD or $FE
		byte old0 = m->latch[0];
		byte old1 = m->latch[1];
		if (m->id == 9) {
			if (addr == 0x0FD8)
				m->latch[0] = 0xFD;
			else if (addr == 0x0FE8)
				m->latch[0] = 0xFE;
		} else {
			if ((addr & 0x1FF8) == 0x0FD8)
				m->latch[0] = 0xFD;
			else if ((addr & 0x1FF8) == 0x0FE8)
				m->latch[0] = 0xFE;
		}
		if ((addr & 0x1FF8) == 0x1FD8)
			m->latch[1] = 0xFD;
		else if ((addr & 0x1FF8) == 0x1FE8)
			m->latch[1] = 0xFE;
		if (old0 != m->latch[0] || old1 != m->latch[1])
			mmc2_update(f);
	}
	return value;
}

// reads chr without the side effects a real ppu fetch has, for debug views
byte mapper_chr_peek(Famicom* f, word addr)
{
	addr &= 0x1FFF;
	if (f->mapper.id == 19) {
		byte* ram = n163_ciram(f, addr);
		if (ram)
			return *ram;
	}
	return f->chr[f->mapper.chr_banks[addr >> 10] + (addr & 0x3FF)];
}

void mapper_chr_write(Famicom* f, word addr, byte value)
{
	addr &= 0x1FFF;
	if (f->mapper.id == 19) {
		byte* ram = n163_ciram(f, addr);
		if (ram)
			*ram = value;
		return;
	}
	if (!f->chr_is_ram)
		return;
	f->chr[f->mapper.chr_banks[addr >> 10] + (addr & 0x3FF)] = value;
}

// called once per rendered scanline, stands in for the mmc3 watching ppu A12
void mapper_scanline(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	if (m->id != 4)
		return;
	if (m->irq_counter == 0 || m->irq_reload) {
		m->irq_counter = m->irq_latch;
		m->irq_reload = false;
	} else {
		m->irq_counter--;
	}
	if (m->irq_counter == 0 && m->irq_enable)
		m->irq_pending = true;
}

void mapper_cpu_clock(Famicom* f)
{
	Famicom_mapper* m = &f->mapper;
	if (m->id == 24 || m->id == 26 || m->id == 85) {
		vrc_irq_clock(m);
		return;
	}
	if (m->id == 19) {
		// counts up every cycle and fires when it reaches $7FFF
		Famicom_n163* n = &f->n163;
		if (n->irq_enabled && n->irq_counter < 0x7FFF && ++n->irq_counter == 0x7FFF)
			n->irq_pending = true;
		return;
	}
	if (m->id != 40 || !m->irq_enable)
		return;
	// fires 4096 cycles after being enabled
	if (++m->irq_cycles == 4096) {
		m->irq_pending = true;
		m->irq_enable = false;
	}
}

bool mapper_irq(Famicom* f)
{
	if (f->mapper.id == MAPPER_FDS)
		return fds_irq(f);
	if (f->mapper.id == 19)
		return f->n163.irq_pending;
	return f->mapper.irq_pending;
}
