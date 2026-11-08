#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "../types.h"
#include "../bitmath.h"
#include "../systems/system.h"
#include "2C02.h"
#include "6502.h"
#include "../systems/famicom.h"
#include "../systems/mappers.h"

#define CTRL_INCREMENT     0x04
#define CTRL_SPRITE_TABLE  0x08
#define CTRL_BG_TABLE      0x10
#define CTRL_SPRITE_SIZE   0x20
#define CTRL_NMI           0x80
#define MASK_GREYSCALE     0x01
#define MASK_BG_LEFT       0x02
#define MASK_SPRITES_LEFT  0x04
#define MASK_BG            0x08
#define MASK_SPRITES       0x10
#define STATUS_OVERFLOW    0x20
#define STATUS_SPRITE_ZERO 0x40
#define STATUS_VBLANK      0x80

enum ppu_mmapped_register {
	PPUCTRL,
	PPUMASK,
	PPUSTATUS,
	OAMADDR,
	OAMDATA,
	PPUSCROLL,
	PPUADDR,
	PPUDATA,
};

void ppu_reset(Famicom_ppu* ppu, bool warm)
{
	if (!warm)
		memset(ppu, 0, sizeof(*ppu));
	ppu->ctrl = 0;
	ppu->mask = 0;
	ppu->write_latch = false;
	ppu->ppudata_buffer = 0;
	ppu->t = 0;
	ppu->fine_x = 0;
	ppu->dot = 0;
	ppu->scanline = 0;
	ppu->odd_frame = false;
	ppu->nmi_pending = false;
	ppu->frame_complete = false;
	ppu->sprite_count = 0;
}

static word nametable_address(Famicom* f, word addr)
{
	addr = (addr - 0x2000) & 0x0FFF;
	int table = addr / 0x400;
	int page;
	switch (f->mapper.mirroring) {
	default:
	case mirror_horizontal: page = table >> 1; break;
	case mirror_vertical: page = table & 1; break;
	case mirror_single_lower: page = 0; break;
	case mirror_single_upper: page = 1; break;
	case mirror_four_screen: page = table; break;
	}
	return page * 0x400 + (addr & 0x3FF);
}

static byte palette_address(word addr)
{
	addr &= 0x1F;
	// $3F10/$3F14/$3F18/$3F1C mirror the background entries
	if ((addr & 0x13) == 0x10)
		addr &= ~0x10;
	return addr;
}

byte ppu_read(Famicom* f, word addr)
{
	addr &= 0x3FFF;
	if (addr < 0x2000)
		return mapper_chr_read(f, addr);
	if (addr < 0x3F00) {
		byte value;
		if (mapper_nametable(f, addr, &value, false))
			return value;
		return f->ppu.vram[nametable_address(f, addr)];
	}
	return f->ppu.palettes[palette_address(addr)];
}

// like ppu_read but never disturbs mapper state, for debug views
byte ppu_peek(Famicom* f, word addr)
{
	addr &= 0x3FFF;
	if (addr < 0x2000)
		return mapper_chr_peek(f, addr);
	return ppu_read(f, addr);
}

void ppu_write(Famicom* f, word addr, byte value)
{
	addr &= 0x3FFF;
	if (addr < 0x2000)
		mapper_chr_write(f, addr, value);
	else if (addr < 0x3F00) {
		if (!mapper_nametable(f, addr, &value, true))
			f->ppu.vram[nametable_address(f, addr)] = value;
	}
	else
		f->ppu.palettes[palette_address(addr)] = value & 0x3F;
}

static void increment_vram_address(Famicom_ppu* ppu)
{
	ppu->v = (ppu->v + ((ppu->ctrl & CTRL_INCREMENT) ? 32 : 1)) & 0x7FFF;
}

byte ppu_register_read(Famicom* f, word addr)
{
	Famicom_ppu* ppu = &f->ppu;
	byte value = ppu->open_bus;
	switch (addr & 0x07) {
	case PPUSTATUS:
		value = (ppu->status & 0xE0) | (ppu->open_bus & 0x1F);
		ppu->status &= ~STATUS_VBLANK;
		ppu->write_latch = false;
		break;
	case OAMDATA:
		value = ppu->oam[ppu->oam_address];
		break;
	case PPUDATA:
		if ((ppu->v & 0x3FFF) < 0x3F00) {
			value = ppu->ppudata_buffer;
			ppu->ppudata_buffer = ppu_read(f, ppu->v);
		} else {
			// palette reads are immediate, the buffer gets the nametable underneath
			value = ppu_read(f, ppu->v) | (ppu->open_bus & 0xC0);
			ppu->ppudata_buffer = ppu_read(f, ppu->v - 0x1000);
		}
		increment_vram_address(ppu);
		break;
	default:
		break;
	}
	ppu->open_bus = value;
	return value;
}

void ppu_register_write(Famicom* f, word addr, byte value)
{
	Famicom_ppu* ppu = &f->ppu;
	ppu->open_bus = value;
	switch (addr & 0x07) {
	case PPUCTRL:
		// turning nmi on during vblank fires one straight away
		if (!(ppu->ctrl & CTRL_NMI) && (value & CTRL_NMI) && (ppu->status & STATUS_VBLANK))
			ppu->nmi_pending = true;
		ppu->ctrl = value;
		ppu->t = (ppu->t & 0xF3FF) | ((value & 0x03) << 10);
		break;
	case PPUMASK:
		ppu->mask = value;
		break;
	case OAMADDR:
		ppu->oam_address = value;
		break;
	case OAMDATA:
		ppu->oam[ppu->oam_address++] = value;
		break;
	case PPUSCROLL:
		if (!ppu->write_latch) {
			ppu->fine_x = value & 0x07;
			ppu->t = (ppu->t & 0xFFE0) | (value >> 3);
		} else {
			ppu->t = (ppu->t & 0x8C1F) | ((value & 0x07) << 12) | ((value & 0xF8) << 2);
		}
		ppu->write_latch = !ppu->write_latch;
		break;
	case PPUADDR:
		if (!ppu->write_latch) {
			ppu->t = (ppu->t & 0x80FF) | ((value & 0x3F) << 8);
		} else {
			ppu->t = (ppu->t & 0xFF00) | value;
			ppu->v = ppu->t;
		}
		ppu->write_latch = !ppu->write_latch;
		break;
	case PPUDATA:
		ppu_write(f, ppu->v, value);
		increment_vram_address(ppu);
		break;
	default:
		break;
	}
}

static void increment_x(Famicom_ppu* ppu)
{
	if ((ppu->v & 0x001F) == 31) {
		ppu->v &= ~0x001F;
		ppu->v ^= 0x0400;
	} else {
		ppu->v++;
	}
}

static void increment_y(Famicom_ppu* ppu)
{
	if ((ppu->v & 0x7000) != 0x7000) {
		ppu->v += 0x1000;
		return;
	}
	ppu->v &= ~0x7000;
	int y = (ppu->v & 0x03E0) >> 5;
	if (y == 29) {
		y = 0;
		ppu->v ^= 0x0800;
	} else if (y == 31) {
		y = 0;
	} else {
		y++;
	}
	ppu->v = (ppu->v & ~0x03E0) | (y << 5);
}

static void load_bg_shifters(Famicom_ppu* ppu)
{
	ppu->bg_shift_lo = (ppu->bg_shift_lo & 0xFF00) | ppu->bg_lo_latch;
	ppu->bg_shift_hi = (ppu->bg_shift_hi & 0xFF00) | ppu->bg_hi_latch;
	ppu->at_shift_lo = (ppu->at_shift_lo & 0xFF00) | ((ppu->at_latch & 1) ? 0xFF : 0x00);
	ppu->at_shift_hi = (ppu->at_shift_hi & 0xFF00) | ((ppu->at_latch & 2) ? 0xFF : 0x00);
}

static void fetch_bg(Famicom* f)
{
	Famicom_ppu* ppu = &f->ppu;
	word table = (ppu->ctrl & CTRL_BG_TABLE) ? 0x1000 : 0;
	word fine_y = (ppu->v >> 12) & 0x07;
	byte at;
	ppu->bg_shift_lo <<= 1;
	ppu->bg_shift_hi <<= 1;
	ppu->at_shift_lo <<= 1;
	ppu->at_shift_hi <<= 1;
	switch ((ppu->dot - 1) % 8) {
	case 0:
		load_bg_shifters(ppu);
		ppu->nt_latch = ppu_read(f, 0x2000 | (ppu->v & 0x0FFF));
		break;
	case 2:
		at = ppu_read(f, 0x23C0 | (ppu->v & 0x0C00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 0x07));
		if (ppu->v & 0x0040)
			at >>= 4;
		if (ppu->v & 0x0002)
			at >>= 2;
		ppu->at_latch = at & 0x03;
		break;
	case 4:
		ppu->bg_lo_latch = ppu_read(f, table + ppu->nt_latch * 16 + fine_y);
		break;
	case 6:
		ppu->bg_hi_latch = ppu_read(f, table + ppu->nt_latch * 16 + fine_y + 8);
		break;
	case 7:
		increment_x(ppu);
		break;
	}
}

// finds the first 8 sprites on the next scanline and fetches their patterns
static void evaluate_sprites(Famicom* f)
{
	Famicom_ppu* ppu = &f->ppu;
	int height = (ppu->ctrl & CTRL_SPRITE_SIZE) ? 16 : 8;
	ppu->sprite_count = 0;
	ppu->sprite_zero_on_line = false;
	for (int i=0; i<64; i++) {
		byte* sprite = &ppu->oam[i*4];
		int row = ppu->scanline - sprite[0];
		if (row < 0 || height <= row)
			continue;
		if (ppu->sprite_count == 8) {
			ppu->status |= STATUS_OVERFLOW;
			break;
		}
		byte tile = sprite[1];
		byte attr = sprite[2];
		if (attr & 0x80)
			row = height - 1 - row;
		word addr;
		if (height == 16) {
			addr = ((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16;
			if (8 <= row) {
				addr += 16;
				row -= 8;
			}
		} else {
			addr = ((ppu->ctrl & CTRL_SPRITE_TABLE) ? 0x1000 : 0) + tile * 16;
		}
		byte lo = ppu_read(f, addr + row);
		byte hi = ppu_read(f, addr + row + 8);
		if (attr & 0x40) {
			lo = reverse_byte_order(lo);
			hi = reverse_byte_order(hi);
		}
		int n = ppu->sprite_count++;
		ppu->sprite_x[n] = sprite[3];
		ppu->sprite_attr[n] = attr;
		ppu->sprite_lo[n] = lo;
		ppu->sprite_hi[n] = hi;
		if (i == 0)
			ppu->sprite_zero_on_line = true;
	}
}

static void render_pixel(Famicom* f)
{
	Famicom_ppu* ppu = &f->ppu;
	int x = ppu->dot - 1;
	int y = ppu->scanline;

	byte bg_pixel = 0;
	byte bg_palette = 0;
	if ((ppu->mask & MASK_BG) && (8 <= x || (ppu->mask & MASK_BG_LEFT))) {
		word bit = 0x8000 >> (ppu->fine_x & 0x07);
		bg_pixel = ((ppu->bg_shift_lo & bit) ? 1 : 0) | ((ppu->bg_shift_hi & bit) ? 2 : 0);
		bg_palette = ((ppu->at_shift_lo & bit) ? 1 : 0) | ((ppu->at_shift_hi & bit) ? 2 : 0);
	}

	// the lowest numbered opaque sprite wins, even if it's behind the background
	byte sp_pixel = 0;
	byte sp_palette = 0;
	bool sp_behind = false;
	bool sp_zero = false;
	if ((ppu->mask & MASK_SPRITES) && (8 <= x || (ppu->mask & MASK_SPRITES_LEFT))) {
		for (int i=0; i<ppu->sprite_count; i++) {
			int offset = x - ppu->sprite_x[i];
			if (offset < 0 || 7 < offset)
				continue;
			byte p = (((ppu->sprite_lo[i] << offset) & 0x80) >> 7) | (((ppu->sprite_hi[i] << offset) & 0x80) >> 6);
			if (p == 0)
				continue;
			sp_pixel = p;
			sp_palette = (ppu->sprite_attr[i] & 0x03) + 4;
			sp_behind = ppu->sprite_attr[i] & 0x20;
			sp_zero = (i == 0 && ppu->sprite_zero_on_line);
			break;
		}
	}

	byte color;
	if (bg_pixel == 0 && sp_pixel == 0) {
		color = 0;
	} else if (bg_pixel == 0) {
		color = sp_palette * 4 + sp_pixel;
	} else if (sp_pixel == 0) {
		color = bg_palette * 4 + bg_pixel;
	} else {
		if (sp_zero && x != 255)
			ppu->status |= STATUS_SPRITE_ZERO;
		if (sp_behind)
			color = bg_palette * 4 + bg_pixel;
		else
			color = sp_palette * 4 + sp_pixel;
	}
	byte index = ppu->palettes[palette_address(color)];
	if (ppu->mask & MASK_GREYSCALE)
		index &= 0x30;
	ppu->framebuffer[y][x] = index & 0x3F;
}

void ppu_step(Famicom* f)
{
	Famicom_ppu* ppu = &f->ppu;
	bool rendering = ppu->mask & (MASK_BG | MASK_SPRITES);
	bool visible = ppu->scanline < 240;
	// pal and dendy frames are 312 lines, dendy also starts vblank later
	int last_line = famicom_last_line(f);
	bool prerender = ppu->scanline == last_line;
	int dot = ppu->dot;

	if (visible || prerender) {
		if (rendering) {
			if ((2 <= dot && dot <= 257) || (321 <= dot && dot <= 337))
				fetch_bg(f);
			if (dot == 256)
				increment_y(ppu);
			if (dot == 257) {
				load_bg_shifters(ppu);
				ppu->v = (ppu->v & ~0x041F) | (ppu->t & 0x041F);
				if (visible)
					evaluate_sprites(f);
				else
					ppu->sprite_count = 0;
			}
			if (prerender && 280 <= dot && dot <= 304)
				ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
			if (dot == 260)
				mapper_scanline(f);
		}
		if (visible && 1 <= dot && dot <= 256)
			render_pixel(f);
	}

	if (ppu->scanline == famicom_vblank_line(f) && dot == 1) {
		ppu->status |= STATUS_VBLANK;
		if (ppu->ctrl & CTRL_NMI)
			ppu->nmi_pending = true;
		ppu->frame_complete = true;
	}
	if (prerender && dot == 1)
		ppu->status &= ~(STATUS_VBLANK | STATUS_SPRITE_ZERO | STATUS_OVERFLOW);

	ppu->dot++;
	// odd frames skip the last dot of the prerender line when rendering, only on ntsc
	if (prerender && ppu->dot == 340 && ppu->odd_frame && rendering && f->region == region_ntsc)
		ppu->dot = 341;
	if (340 < ppu->dot) {
		ppu->dot = 0;
		ppu->scanline++;
		if (last_line < ppu->scanline) {
			ppu->scanline = 0;
			ppu->odd_frame = !ppu->odd_frame;
			ppu->frame++;
		}
	}
}
