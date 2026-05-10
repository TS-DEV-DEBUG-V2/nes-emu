#include "nes.h"

const uint32_t nes_palette[64] = {
    0xFF4A4A4A, 0xFF002A88, 0xFF1412A7, 0xFF4A0090,
    0xFF700060, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08,
    0xFF00404D, 0xFF080808, 0xFF080808, 0xFF080808,

    0xFF8A8A8A, 0xFF155FD9, 0xFF4240FF, 0xFF8A30E0,
    0xFFB02090, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32,
    0xFF007C8D, 0xFF080808, 0xFF080808, 0xFF080808,

    0xFFF0F0F0, 0xFF64B0FF, 0xFF9290FF, 0xFFD070F0,
    0xFFFF70D0, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082,
    0xFF48CDDE, 0xFF303030, 0xFF080808, 0xFF080808,

    0xFFF8F8F8, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFF0C0FF,
    0xFFFFB0F0, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
    0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC,
    0xFFB5EBF2, 0xFF909090, 0xFF080808, 0xFF080808,
};

static uint8_t ppu_bus_read(NES *n, uint16_t addr) {
    addr &= 0x3FFF;
    if (addr < 0x2000) return cart_ppu_read(n, addr);
    if (addr < 0x3F00) return n->ppu.nametable[cart_mirror_addr(n, addr)];
    uint16_t pa = addr & 0x1F;
    if ((pa & 0x13) == 0x10) pa &= 0x0F;
    return n->ppu.palette[pa];
}

static void ppu_bus_write(NES *n, uint16_t addr, uint8_t val) {
    addr &= 0x3FFF;
    if (addr < 0x2000) { cart_ppu_write(n, addr, val); return; }
    if (addr < 0x3F00) { n->ppu.nametable[cart_mirror_addr(n, addr)] = val; return; }
    uint16_t pa = addr & 0x1F;
    if ((pa & 0x13) == 0x10) pa &= 0x0F;
    n->ppu.palette[pa] = val;
}

void ppu_init(NES *n) {
    memset(&n->ppu, 0, sizeof(PPU));
    n->ppu.scanline = 261;
}

void ppu_reset(NES *n) {
    PPU *p = &n->ppu;
    p->ctrl = 0; p->mask = 0;
    p->w = false;
    p->data_buf = 0;
    p->scanline = 261;
    p->dot = 0;
    p->odd_frame = false;
    p->frame_complete = false;
    p->nmi_output = false;
    p->nmi_occurred = false;
}

static inline bool rendering(PPU *p) {
    return (p->mask & 0x18) != 0;
}

static void inc_x(PPU *p) {
    if ((p->v & 0x001F) == 31) {
        p->v &= ~0x001F;
        p->v ^= 0x0400;
    } else {
        p->v++;
    }
}

static void inc_y(PPU *p) {
    if ((p->v & 0x7000) != 0x7000) {
        p->v += 0x1000;
    } else {
        p->v &= ~0x7000;
        int y = (p->v & 0x03E0) >> 5;
        if (y == 29) { y = 0; p->v ^= 0x0800; }
        else if (y == 31) { y = 0; }
        else { y++; }
        p->v = (p->v & ~0x03E0) | (y << 5);
    }
}

static void copy_x(PPU *p) {
    p->v = (p->v & 0xFBE0) | (p->t & 0x041F);
}

static void copy_y(PPU *p) {
    p->v = (p->v & 0x841F) | (p->t & 0x7BE0);
}

static void load_bg_shifters(PPU *p) {
    p->bg_shift_lo = (p->bg_shift_lo & 0xFF00) | p->bg_lo_latch;
    p->bg_shift_hi = (p->bg_shift_hi & 0xFF00) | p->bg_hi_latch;
    p->at_shift_lo = (p->at_shift_lo & 0xFF00) | ((p->at_latch & 1) ? 0xFF : 0);
    p->at_shift_hi = (p->at_shift_hi & 0xFF00) | ((p->at_latch & 2) ? 0xFF : 0);
}

static void shift_bg(PPU *p) {
    p->bg_shift_lo <<= 1;
    p->bg_shift_hi <<= 1;
    p->at_shift_lo <<= 1;
    p->at_shift_hi <<= 1;
}

uint8_t ppu_read_reg(NES *n, uint16_t addr) {
    PPU *p = &n->ppu;
    uint8_t result = p->open_bus;
    switch (addr & 7) {
    case 2: {
        result = (p->open_bus & 0x1F) | (p->status & 0xE0);
        if (p->nmi_occurred) result |= 0x80;
        else result &= ~0x80;
        if (p->scanline == 241 && p->dot == 0)
            result &= ~0x80;
        p->nmi_occurred = false;
        p->w = false;
        if (p->scanline == 241 && p->dot <= 2)
            n->cpu.nmi_pending = false;
    } break;
    case 4:
        result = p->oam[p->oam_addr];
        break;
    case 7: {
        uint16_t a = p->v & 0x3FFF;
        if (a >= 0x3F00) {
            result = ppu_bus_read(n, a);
            p->data_buf = ppu_bus_read(n, a - 0x1000);
        } else {
            result = p->data_buf;
            p->data_buf = ppu_bus_read(n, a);
        }
        if (rendering(p) && (p->scanline < 240 || p->scanline == 261)) {
            inc_x(p);
            inc_y(p);
        } else {
            p->v += (p->ctrl & 0x04) ? 32 : 1;
        }
    } break;
    }
    p->open_bus = result;
    return result;
}

void ppu_write_reg(NES *n, uint16_t addr, uint8_t val) {
    PPU *p = &n->ppu;
    p->open_bus = val;
    switch (addr & 7) {
    case 0: {
        bool was_clear = !p->nmi_output;
        p->ctrl = val;
        p->t = (p->t & 0xF3FF) | ((val & 3) << 10);
        p->nmi_output = (val & 0x80) != 0;
        if (p->nmi_output && was_clear && p->nmi_occurred)
            n->cpu.nmi_pending = true;
        if (!p->nmi_output)
            n->cpu.nmi_pending = false;
    } break;
    case 1:
        p->mask = val;
        break;
    case 3:
        p->oam_addr = val;
        break;
    case 4:
        if (rendering(p) && (p->scanline < 240 || p->scanline == 261)) {
            p->oam_addr += 4;
        } else {
            p->oam[p->oam_addr++] = val;
        }
        break;
    case 5:
        if (!p->w) {
            p->t = (p->t & 0xFFE0) | (val >> 3);
            p->fine_x = val & 7;
        } else {
            p->t = (p->t & 0x8C1F) | ((val & 0xF8) << 2) | ((val & 7) << 12);
        }
        p->w = !p->w;
        break;
    case 6:
        if (!p->w) {
            p->t = (p->t & 0x00FF) | ((val & 0x3F) << 8);
        } else {
            p->t = (p->t & 0xFF00) | val;
            p->v = p->t;
        }
        p->w = !p->w;
        break;
    case 7:
        ppu_bus_write(n, p->v & 0x3FFF, val);
        if (rendering(p) && (p->scanline < 240 || p->scanline == 261)) {
            inc_x(p);
            inc_y(p);
        } else {
            p->v += (p->ctrl & 0x04) ? 32 : 1;
        }
        break;
    }
}

static void eval_sprites(NES *n) {
    PPU *p = &n->ppu;
    int h = (p->ctrl & 0x20) ? 16 : 8;
    memset(p->soam, 0xFF, sizeof(p->soam));
    p->sprite_count = 0;
    p->spr_zero_on_line = false;

    for (int i = 0; i < 64; i++) {
        int y = p->oam[i * 4];
        int diff = p->scanline - y;
        if (diff >= 0 && diff < h) {
            if (i == 0) p->spr_zero_on_line = true;
            if (p->sprite_count < 8) {
                int s = p->sprite_count;
                p->soam[s * 4 + 0] = p->oam[i * 4 + 0];
                p->soam[s * 4 + 1] = p->oam[i * 4 + 1];
                p->soam[s * 4 + 2] = p->oam[i * 4 + 2];
                p->soam[s * 4 + 3] = p->oam[i * 4 + 3];
                p->spr_index[s] = i;
            }
            p->sprite_count++;
        }
    }

    if (p->sprite_count > 8) {
        p->status |= 0x20;
        p->sprite_count = 8;
    }
}

static void fetch_sprites(NES *n) {
    PPU *p = &n->ppu;
    int h = (p->ctrl & 0x20) ? 16 : 8;
    for (int i = 0; i < p->sprite_count; i++) {
        uint8_t y    = p->soam[i * 4 + 0];
        uint8_t tile = p->soam[i * 4 + 1];
        uint8_t attr = p->soam[i * 4 + 2];
        uint8_t x    = p->soam[i * 4 + 3];

        int row = p->scanline - y;
        bool flipv = (attr & 0x80) != 0;
        if (flipv) row = h - 1 - row;

        uint16_t pat_addr;
        if (h == 8) {
            pat_addr = ((p->ctrl & 0x08) ? 0x1000 : 0) + tile * 16 + row;
        } else {
            uint16_t table = (tile & 1) ? 0x1000 : 0;
            uint8_t t = tile & 0xFE;
            if (row >= 8) { t++; row -= 8; }
            pat_addr = table + t * 16 + row;
        }

        uint8_t lo = ppu_bus_read(n, pat_addr);
        uint8_t hi = ppu_bus_read(n, pat_addr + 8);

        if (attr & 0x40) {
            lo = ((lo * 0x0802LU & 0x22110LU) | (lo * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
            hi = ((hi * 0x0802LU & 0x22110LU) | (hi * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
        }

        p->spr_pattern_lo[i] = lo;
        p->spr_pattern_hi[i] = hi;
        p->spr_attr[i] = attr;
        p->spr_x[i] = x;
    }
    for (int i = p->sprite_count; i < 8; i++) {
        p->spr_pattern_lo[i] = 0;
        p->spr_pattern_hi[i] = 0;
    }
}

static void render_pixel(NES *n) {
    PPU *p = &n->ppu;
    int x = p->dot - 1;
    int y = p->scanline;
    if (x < 0 || x >= 256 || y < 0 || y >= 240) return;

    uint8_t bg_pixel = 0, bg_palette = 0;
    uint8_t sp_pixel = 0, sp_palette = 0;
    bool sp_priority = false;
    bool sp_zero = false;

    if (p->mask & 0x08) {
        if ((p->mask & 0x02) || x >= 8) {
            uint16_t mux = 0x8000 >> p->fine_x;
            uint8_t p0 = (p->bg_shift_lo & mux) ? 1 : 0;
            uint8_t p1 = (p->bg_shift_hi & mux) ? 1 : 0;
            uint8_t a0 = (p->at_shift_lo & mux) ? 1 : 0;
            uint8_t a1 = (p->at_shift_hi & mux) ? 1 : 0;
            bg_pixel = (p1 << 1) | p0;
            bg_palette = (a1 << 1) | a0;
        }
    }

    if (p->mask & 0x10) {
        if ((p->mask & 0x04) || x >= 8) {
            for (int i = 0; i < p->sprite_count; i++) {
                int xoff = x - p->spr_x[i];
                if (xoff < 0 || xoff >= 8) continue;
                uint8_t s0 = (p->spr_pattern_lo[i] >> (7 - xoff)) & 1;
                uint8_t s1 = (p->spr_pattern_hi[i] >> (7 - xoff)) & 1;
                uint8_t spx = (s1 << 1) | s0;
                if (spx == 0) continue;
                if (i == 0 && p->spr_zero_on_line) sp_zero = true;
                sp_pixel = spx;
                sp_palette = (p->spr_attr[i] & 3) + 4;
                sp_priority = (p->spr_attr[i] & 0x20) != 0;
                break;
            }
        }
    }

    if (sp_zero && bg_pixel != 0 && sp_pixel != 0 && x < 255 &&
        (p->mask & 0x18) == 0x18) {
        p->status |= 0x40;
    }

    uint8_t final_pixel, final_palette;
    if (bg_pixel == 0 && sp_pixel == 0) {
        final_pixel = 0; final_palette = 0;
    } else if (bg_pixel == 0) {
        final_pixel = sp_pixel; final_palette = sp_palette;
    } else if (sp_pixel == 0) {
        final_pixel = bg_pixel; final_palette = bg_palette;
    } else {
        if (sp_priority) { final_pixel = bg_pixel; final_palette = bg_palette; }
        else             { final_pixel = sp_pixel; final_palette = sp_palette; }
    }

    uint8_t color_idx = ppu_bus_read(n, 0x3F00 + final_palette * 4 + final_pixel) & 0x3F;
    uint32_t color = nes_palette[color_idx];

    uint8_t emphasis = (p->mask >> 5) & 7;
    if (emphasis) {
        float r = (float)((color >> 16) & 0xFF);
        float g = (float)((color >> 8) & 0xFF);
        float b = (float)(color & 0xFF);
        float atten = 0.746f;
        if (emphasis & 1) { g *= atten; b *= atten; }
        if (emphasis & 2) { r *= atten; b *= atten; }
        if (emphasis & 4) { r *= atten; g *= atten; }
        color = 0xFF000000
              | ((uint32_t)(r > 255 ? 255 : r) << 16)
              | ((uint32_t)(g > 255 ? 255 : g) << 8)
              | (uint32_t)(b > 255 ? 255 : b);
    }

    p->framebuffer[y * 256 + x] = color;
}

static void do_bg_fetch(NES *n, int dot) {
    PPU *p = &n->ppu;
    switch ((dot - 1) & 7) {
    case 1:
        p->nt_latch = ppu_bus_read(n, 0x2000 | (p->v & 0x0FFF));
        break;
    case 3: {
        uint16_t a = 0x23C0 | (p->v & 0x0C00)
                   | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 7);
        uint8_t at = ppu_bus_read(n, a);
        if (p->v & 0x0040) at >>= 4;
        if (p->v & 0x0002) at >>= 2;
        p->at_latch = at & 3;
    } break;
    case 5: {
        uint16_t pat = (p->ctrl & 0x10) ? 0x1000 : 0;
        p->bg_lo_latch = ppu_bus_read(n, pat + p->nt_latch * 16 + ((p->v >> 12) & 7));
    } break;
    case 7: {
        uint16_t pat = (p->ctrl & 0x10) ? 0x1000 : 0;
        p->bg_hi_latch = ppu_bus_read(n, pat + p->nt_latch * 16 + ((p->v >> 12) & 7) + 8);
        inc_x(p);
    } break;
    }
}

void ppu_step(NES *n) {
    PPU *p = &n->ppu;
    bool render_on = rendering(p);

    if (p->scanline < 240) {
        if (render_on) {
            if (p->dot >= 1 && p->dot <= 256) {
                render_pixel(n);
                shift_bg(p);
                if (((p->dot - 1) & 7) == 0) load_bg_shifters(p);
                do_bg_fetch(n, p->dot);
                if (p->dot == 256) inc_y(p);
            }

            if (p->dot == 257) {
                load_bg_shifters(p);
                copy_x(p);
                eval_sprites(n);
                fetch_sprites(n);
                cart_scanline(n);
            }

            if (p->dot >= 321 && p->dot <= 336) {
                shift_bg(p);
                if (((p->dot - 1) & 7) == 0) load_bg_shifters(p);
                do_bg_fetch(n, p->dot);
            }

            if (p->dot == 337) load_bg_shifters(p);

            if (p->dot == 337 || p->dot == 339) {
                ppu_bus_read(n, 0x2000 | (p->v & 0x0FFF));
            }
        } else {
            if (p->dot >= 1 && p->dot <= 256)
                render_pixel(n);
        }
    } else if (p->scanline == 261) {
        if (render_on) {
            if (p->dot >= 1 && p->dot <= 256) {
                shift_bg(p);
                if (((p->dot - 1) & 7) == 0) load_bg_shifters(p);
                do_bg_fetch(n, p->dot);
                if (p->dot == 256) inc_y(p);
            }
            if (p->dot == 257) {
                load_bg_shifters(p);
                copy_x(p);
            }
            if (p->dot >= 280 && p->dot <= 304) copy_y(p);

            if (p->dot >= 321 && p->dot <= 336) {
                shift_bg(p);
                if (((p->dot - 1) & 7) == 0) load_bg_shifters(p);
                do_bg_fetch(n, p->dot);
            }

            if (p->dot == 337) load_bg_shifters(p);

            if (p->dot == 337 || p->dot == 339) {
                ppu_bus_read(n, 0x2000 | (p->v & 0x0FFF));
            }
        }
    }

    if (p->scanline == 241 && p->dot == 1) {
        p->nmi_occurred = true;
        p->status |= 0x80;
        if (p->nmi_output)
            n->cpu.nmi_pending = true;
        p->frame_complete = true;
    }

    if (p->scanline == 261 && p->dot == 1) {
        p->nmi_occurred = false;
        p->status &= ~(0x80 | 0x40 | 0x20);
        memset(p->spr_pattern_lo, 0, sizeof(p->spr_pattern_lo));
        memset(p->spr_pattern_hi, 0, sizeof(p->spr_pattern_hi));
    }

    p->dot++;

    if (p->scanline == 261 && render_on && p->odd_frame && p->dot == 340) {
        p->dot = 0;
        p->scanline = 0;
        p->odd_frame = !p->odd_frame;
        return;
    }

    if (p->dot > 340) {
        p->dot = 0;
        p->scanline++;
        if (p->scanline > 261) {
            p->scanline = 0;
            p->odd_frame = !p->odd_frame;
        }
    }
}