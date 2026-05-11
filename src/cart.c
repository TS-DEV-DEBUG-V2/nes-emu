#include "nes.h"

bool cart_load(NES *n, const uint8_t *data, int size) {
    if (size < 16 || data[0] != 'N' || data[1] != 'E' || data[2] != 'S' || data[3] != 0x1A) {
        fprintf(stderr, "Invalid iNES header\n");
        return false;
    }

    Cartridge *c = &n->cart;
    memset(c, 0, sizeof(Cartridge));

    int prg_banks = data[4];
    int chr_banks = data[5];
    uint8_t flags6 = data[6];
    uint8_t flags7 = data[7];

    c->mapper_id = (flags6 >> 4) | (flags7 & 0xF0);
    c->mirror = (flags6 & 1) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
    if (flags6 & 0x08) c->mirror = MIRROR_FOUR_SCREEN;
    c->battery = (flags6 & 0x02) != 0;
    c->prg_banks = prg_banks;
    c->chr_banks = chr_banks;

    bool trainer = (flags6 & 0x04) != 0;
    int offset = 16 + (trainer ? 512 : 0);

    c->prg_size = prg_banks * 16384;
    c->chr_size = chr_banks * 8192;

    if (offset + c->prg_size > size) {
        fprintf(stderr, "ROM file too small for PRG data\n");
        return false;
    }

    c->prg_rom = (uint8_t *)malloc(c->prg_size);
    memcpy(c->prg_rom, data + offset, c->prg_size);
    offset += c->prg_size;

    if (c->chr_size > 0) {
        c->chr_rom = (uint8_t *)malloc(c->chr_size);
        if (offset + c->chr_size <= size)
            memcpy(c->chr_rom, data + offset, c->chr_size);
    } else {
        c->chr_ram_size = 8192;
        c->chr_ram = (uint8_t *)calloc(1, c->chr_ram_size);
    }

    c->prg_ram_size = 8192;
    c->prg_ram = (uint8_t *)calloc(1, c->prg_ram_size);

    /* mapper init */
    switch (c->mapper_id) {
    case 1:
        c->m.mmc1.sr = 0x10;
        c->m.mmc1.ctrl = 0x0C;
        break;
    case 4:
        c->m.mmc3.banks[0] = 0; c->m.mmc3.banks[1] = 2;
        c->m.mmc3.banks[2] = 4; c->m.mmc3.banks[3] = 5;
        c->m.mmc3.banks[4] = 6; c->m.mmc3.banks[5] = 7;
        c->m.mmc3.banks[6] = 0; c->m.mmc3.banks[7] = 1;
        break;
    default:
        break;
    }

    printf("Loaded: PRG=%dKB CHR=%dKB Mapper=%d Mirror=%d\n",
           c->prg_size / 1024, c->chr_size > 0 ? c->chr_size / 1024 : 8,
           c->mapper_id, c->mirror);
    return true;
}

void cart_destroy(NES *n) {
    Cartridge *c = &n->cart;
    free(c->prg_rom); c->prg_rom = NULL;
    free(c->chr_rom); c->chr_rom = NULL;
    free(c->prg_ram); c->prg_ram = NULL;
    free(c->chr_ram); c->chr_ram = NULL;
}

/* ---- NROM (mapper 0) ---- */

static uint8_t nrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000)
        return c->prg_rom[(addr - 0x8000) % c->prg_size];
    if (addr >= 0x6000)
        return c->prg_ram[addr - 0x6000];
    return 0;
}

static void nrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x6000 && addr < 0x8000)
        n->cart.prg_ram[addr - 0x6000] = val;
}

static uint8_t nrom_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) return c->chr_rom[addr % (c->chr_size ? c->chr_size : 8192)];
    return c->chr_ram[addr & 0x1FFF];
}

static void nrom_ppu_write(NES *n, uint16_t addr, uint8_t val) {
    if (n->cart.chr_ram) n->cart.chr_ram[addr & 0x1FFF] = val;
}

/* ---- MMC1 (mapper 1) ---- */

static void mmc1_update(NES *n) {
    Cartridge *c = &n->cart;
    switch (c->m.mmc1.ctrl & 3) {
    case 0: c->mirror = MIRROR_SINGLE_LO; break;
    case 1: c->mirror = MIRROR_SINGLE_HI; break;
    case 2: c->mirror = MIRROR_VERTICAL; break;
    case 3: c->mirror = MIRROR_HORIZONTAL; break;
    }
}

static uint8_t mmc1_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        int prg_mode = (c->m.mmc1.ctrl >> 2) & 3;
        int bank = c->m.mmc1.prg & 0x0F;
        int last = c->prg_banks - 1;
        uint32_t offset;
        if (addr < 0xC000) {
            switch (prg_mode) {
            case 0: case 1: offset = (bank & 0x0E) * 16384 + (addr - 0x8000); break;
            case 2: offset = addr - 0x8000; break;
            default: offset = bank * 16384 + (addr - 0x8000); break;
            }
        } else {
            switch (prg_mode) {
            case 0: case 1: offset = ((bank & 0x0E) | 1) * 16384 + (addr - 0xC000); break;
            case 2: offset = bank * 16384 + (addr - 0xC000); break;
            default: offset = last * 16384 + (addr - 0xC000); break;
            }
        }
        return c->prg_rom[offset % c->prg_size];
    }
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void mmc1_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    Cartridge *c = &n->cart;
    if (addr >= 0x6000 && addr < 0x8000) { c->prg_ram[addr - 0x6000] = val; return; }
    if (addr < 0x8000) return;

    if (val & 0x80) {
        c->m.mmc1.sr = 0x10;
        c->m.mmc1.cnt = 0;
        c->m.mmc1.ctrl |= 0x0C;
        return;
    }
    c->m.mmc1.sr = (c->m.mmc1.sr >> 1) | ((val & 1) << 4);
    c->m.mmc1.cnt++;
    if (c->m.mmc1.cnt == 5) {
        int reg = (addr >> 13) & 3;
        uint8_t v = c->m.mmc1.sr;
        switch (reg) {
        case 0: c->m.mmc1.ctrl = v; break;
        case 1: c->m.mmc1.chr0 = v; break;
        case 2: c->m.mmc1.chr1 = v; break;
        case 3: c->m.mmc1.prg = v; break;
        }
        c->m.mmc1.sr = 0x10;
        c->m.mmc1.cnt = 0;
        mmc1_update(n);
    }
}

static uint8_t mmc1_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_ram) return c->chr_ram[addr & 0x1FFF];
    bool mode4k = (c->m.mmc1.ctrl & 0x10) != 0;
    uint32_t offset;
    if (addr < 0x1000) {
        if (mode4k) offset = c->m.mmc1.chr0 * 4096 + addr;
        else        offset = (c->m.mmc1.chr0 & 0x1E) * 4096 + addr;
    } else {
        if (mode4k) offset = c->m.mmc1.chr1 * 4096 + (addr - 0x1000);
        else        offset = ((c->m.mmc1.chr0 & 0x1E) + 1) * 4096 + (addr - 0x1000);
    }
    return c->chr_rom[offset % c->chr_size];
}

static void mmc1_ppu_write(NES *n, uint16_t addr, uint8_t val) {
    if (n->cart.chr_ram) n->cart.chr_ram[addr & 0x1FFF] = val;
}

/* ---- UxROM (mapper 2) ---- */

static uint8_t uxrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0xC000)
        return c->prg_rom[(c->prg_banks - 1) * 16384 + (addr - 0xC000)];
    if (addr >= 0x8000)
        return c->prg_rom[c->m.uxrom.bank * 16384 + (addr - 0x8000)];
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void uxrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.uxrom.bank = val & 0x0F;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

/* ---- CNROM (mapper 3) ---- */

static uint8_t cnrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) return c->prg_rom[(addr - 0x8000) % c->prg_size];
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void cnrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.cnrom.bank = val & 3;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

static uint8_t cnrom_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) {
        uint32_t off = c->m.cnrom.bank * 8192 + addr;
        return c->chr_rom[off % c->chr_size];
    }
    return c->chr_ram ? c->chr_ram[addr & 0x1FFF] : 0;
}

/* ---- MMC3 (mapper 4) ---- */

static int mmc3_prg_addr(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    int bank;
    int last = (c->prg_size / 8192) - 1;
    if (c->m.mmc3.prg_mode) {
        if      (addr < 0xA000) bank = last - 1;
        else if (addr < 0xC000) bank = c->m.mmc3.banks[7];
        else if (addr < 0xE000) bank = c->m.mmc3.banks[6];
        else                    bank = last;
    } else {
        if      (addr < 0xA000) bank = c->m.mmc3.banks[6];
        else if (addr < 0xC000) bank = c->m.mmc3.banks[7];
        else if (addr < 0xE000) bank = last - 1;
        else                    bank = last;
    }
    return (bank * 8192 + (addr & 0x1FFF)) % c->prg_size;
}

static uint8_t mmc3_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) return c->prg_rom[mmc3_prg_addr(n, addr)];
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void mmc3_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    Cartridge *c = &n->cart;
    if (addr < 0x6000) return;
    if (addr < 0x8000) { c->prg_ram[addr - 0x6000] = val; return; }

    bool even = !(addr & 1);
    if (addr < 0xA000) {
        if (even) {
            c->m.mmc3.bank_sel = val & 7;
            c->m.mmc3.prg_mode = (val & 0x40) != 0;
            c->m.mmc3.chr_mode = (val & 0x80) != 0;
        } else {
            c->m.mmc3.banks[c->m.mmc3.bank_sel] = val;
        }
    } else if (addr < 0xC000) {
        if (even)
            c->mirror = (val & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL;
    } else if (addr < 0xE000) {
        if (even) c->m.mmc3.irq_latch = val;
        else      c->m.mmc3.irq_reload = true;
    } else {
        if (even) { c->m.mmc3.irq_enable = false; n->cart.irq = false; }
        else      { c->m.mmc3.irq_enable = true; }
    }
}

static uint8_t mmc3_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_ram) return c->chr_ram[addr & 0x1FFF];
    int bank;
    int a = addr & 0x1FFF;
    int slot = a / 0x400;
    if (c->m.mmc3.chr_mode) {
        switch (slot) {
        case 0: bank = c->m.mmc3.banks[2]; break;
        case 1: bank = c->m.mmc3.banks[3]; break;
        case 2: bank = c->m.mmc3.banks[4]; break;
        case 3: bank = c->m.mmc3.banks[5]; break;
        case 4: bank = c->m.mmc3.banks[0] & 0xFE; break;
        case 5: bank = c->m.mmc3.banks[0] | 1; break;
        case 6: bank = c->m.mmc3.banks[1] & 0xFE; break;
        default:bank = c->m.mmc3.banks[1] | 1; break;
        }
    } else {
        switch (slot) {
        case 0: bank = c->m.mmc3.banks[0] & 0xFE; break;
        case 1: bank = c->m.mmc3.banks[0] | 1; break;
        case 2: bank = c->m.mmc3.banks[1] & 0xFE; break;
        case 3: bank = c->m.mmc3.banks[1] | 1; break;
        case 4: bank = c->m.mmc3.banks[2]; break;
        case 5: bank = c->m.mmc3.banks[3]; break;
        case 6: bank = c->m.mmc3.banks[4]; break;
        default:bank = c->m.mmc3.banks[5]; break;
        }
    }
    uint32_t off = bank * 1024 + (addr & 0x3FF);
    return c->chr_rom[off % c->chr_size];
}

static void mmc3_ppu_write(NES *n, uint16_t addr, uint8_t val) {
    if (n->cart.chr_ram) n->cart.chr_ram[addr & 0x1FFF] = val;
}

/* ---- AxROM (mapper 7) ---- */

static uint8_t axrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        uint32_t off = (c->m.axrom.bank & 7) * 32768 + (addr - 0x8000);
        return c->prg_rom[off % c->prg_size];
    }
    return 0;
}

static void axrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        n->cart.m.axrom.bank = val & 7;
        n->cart.mirror = (val & 0x10) ? MIRROR_SINGLE_HI : MIRROR_SINGLE_LO;
    }
}

/* ---- Color Dreams (mapper 11) ---- */

static uint8_t color_dreams_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        int bank = (c->m.color_dreams.bank >> 4) & 7;
        uint32_t off = bank * 32768 + (addr - 0x8000);
        return c->prg_rom[off % c->prg_size];
    }
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void color_dreams_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.color_dreams.bank = val;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

static uint8_t color_dreams_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) {
        int bank = c->m.color_dreams.bank & 0x0F;
        uint32_t off = bank * 8192 + addr;
        return c->chr_rom[off % c->chr_size];
    }
    return c->chr_ram ? c->chr_ram[addr & 0x1FFF] : 0;
}

/* ---- BNROM (mapper 34) ---- */

static uint8_t bnrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        int bank = c->m.bnrom.bank & 7;
        uint32_t off = bank * 32768 + (addr - 0x8000);
        return c->prg_rom[off % c->prg_size];
    }
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void bnrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.bnrom.bank = val;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

/* ---- GxROM (mapper 66) ---- */

static uint8_t gxrom_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        int bank = (c->m.gxrom.reg >> 4) & 3;
        uint32_t off = bank * 32768 + (addr - 0x8000);
        return c->prg_rom[off % c->prg_size];
    }
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void gxrom_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.gxrom.reg = val;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

static uint8_t gxrom_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) {
        int bank = c->m.gxrom.reg & 0x0F;
        uint32_t off = bank * 8192 + addr;
        return c->chr_rom[off % c->chr_size];
    }
    return c->chr_ram ? c->chr_ram[addr & 0x1FFF] : 0;
}

/* ---- Camerica (mapper 71) ---- */

static uint8_t camerica_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0xC000)
        return c->prg_rom[(c->prg_banks - 1) * 16384 + (addr - 0xC000)];
    if (addr >= 0x8000)
        return c->prg_rom[(c->m.camerica.bank & 0x0F) * 16384 + (addr - 0x8000)];
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void camerica_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.camerica.bank = val;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

/* ---- Irem 74HC161/32 (mapper 78) ---- */

static uint8_t irem_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0xC000)
        return c->prg_rom[(c->prg_banks - 1) * 16384 + (addr - 0xC000)];
    if (addr >= 0x8000)
        return c->prg_rom[(c->m.irem.bank & 0x0F) * 16384 + (addr - 0x8000)];
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void irem_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) n->cart.m.irem.bank = val;
    else if (addr >= 0x6000) n->cart.prg_ram[addr - 0x6000] = val;
}

static uint8_t irem_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) {
        int bank = (c->m.irem.bank >> 4) & 0x0F;
        uint32_t off = bank * 8192 + addr;
        return c->chr_rom[off % c->chr_size];
    }
    return c->chr_ram ? c->chr_ram[addr & 0x1FFF] : 0;
}

/* ---- NINA-03/06 (mapper 79) ---- */

static uint8_t nina_cpu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (addr >= 0x8000) {
        int bank = c->m.nina.prg & 3;
        uint32_t off = bank * 32768 + (addr - 0x8000);
        return c->prg_rom[off % c->prg_size];
    }
    if (addr >= 0x6000) return c->prg_ram[addr - 0x6000];
    return 0;
}

static void nina_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    Cartridge *c = &n->cart;
    if (addr >= 0x4100 && addr < 0x5000) {
        int reg = (addr >> 8) & 7;
        if (reg == 0) c->m.nina.prg = val;
        else if (reg >= 1 && reg <= 4) c->m.nina.chr[reg - 1] = val & 7;
        return;
    }
    if (addr >= 0x6000 && addr < 0x8000) c->prg_ram[addr - 0x6000] = val;
}

static uint8_t nina_ppu_read(NES *n, uint16_t addr) {
    Cartridge *c = &n->cart;
    if (c->chr_rom) {
        int bank = c->m.nina.chr[(addr & 0x1FFF) / 0x800];
        uint32_t off = bank * 2048 + (addr & 0x7FF);
        return c->chr_rom[off % c->chr_size];
    }
    return c->chr_ram ? c->chr_ram[addr & 0x1FFF] : 0;
}

/* ---- dispatch ---- */

uint8_t cart_cpu_read(NES *n, uint16_t addr) {
    switch (n->cart.mapper_id) {
    case 0:  return nrom_cpu_read(n, addr);
    case 1:  return mmc1_cpu_read(n, addr);
    case 2:  return uxrom_cpu_read(n, addr);
    case 3:  return cnrom_cpu_read(n, addr);
    case 4:  return mmc3_cpu_read(n, addr);
    case 7:  return axrom_cpu_read(n, addr);
    case 11: return color_dreams_cpu_read(n, addr);
    case 34: return bnrom_cpu_read(n, addr);
    case 66: return gxrom_cpu_read(n, addr);
    case 71: return camerica_cpu_read(n, addr);
    case 78: return irem_cpu_read(n, addr);
    case 79: return nina_cpu_read(n, addr);
    default: return nrom_cpu_read(n, addr);
    }
}

void cart_cpu_write(NES *n, uint16_t addr, uint8_t val) {
    switch (n->cart.mapper_id) {
    case 0:  nrom_cpu_write(n, addr, val); break;
    case 1:  mmc1_cpu_write(n, addr, val); break;
    case 2:  uxrom_cpu_write(n, addr, val); break;
    case 3:  cnrom_cpu_write(n, addr, val); break;
    case 4:  mmc3_cpu_write(n, addr, val); break;
    case 7:  axrom_cpu_write(n, addr, val); break;
    case 11: color_dreams_cpu_write(n, addr, val); break;
    case 34: bnrom_cpu_write(n, addr, val); break;
    case 66: gxrom_cpu_write(n, addr, val); break;
    case 71: camerica_cpu_write(n, addr, val); break;
    case 78: irem_cpu_write(n, addr, val); break;
    case 79: nina_cpu_write(n, addr, val); break;
    default: nrom_cpu_write(n, addr, val); break;
    }
}

uint8_t cart_ppu_read(NES *n, uint16_t addr) {
    switch (n->cart.mapper_id) {
    case 1:  return mmc1_ppu_read(n, addr);
    case 3:  return cnrom_ppu_read(n, addr);
    case 4:  return mmc3_ppu_read(n, addr);
    case 11: return color_dreams_ppu_read(n, addr);
    case 66: return gxrom_ppu_read(n, addr);
    case 78: return irem_ppu_read(n, addr);
    case 79: return nina_ppu_read(n, addr);
    default: return nrom_ppu_read(n, addr);
    }
}

void cart_ppu_write(NES *n, uint16_t addr, uint8_t val) {
    switch (n->cart.mapper_id) {
    case 1:  mmc1_ppu_write(n, addr, val); break;
    case 4:  mmc3_ppu_write(n, addr, val); break;
    default: nrom_ppu_write(n, addr, val); break;
    }
}

void cart_scanline(NES *n) {
    if (n->cart.mapper_id != 4) return;
    Cartridge *c = &n->cart;
    if (c->m.mmc3.irq_reload || c->m.mmc3.irq_counter == 0) {
        c->m.mmc3.irq_counter = c->m.mmc3.irq_latch;
        c->m.mmc3.irq_reload = false;
    } else {
        c->m.mmc3.irq_counter--;
    }
    if (c->m.mmc3.irq_counter == 0 && c->m.mmc3.irq_enable) {
        n->cart.irq = true;
    }
}

uint16_t cart_mirror_addr(NES *n, uint16_t addr) {
    uint16_t a = addr & 0x0FFF;
    switch (n->cart.mirror) {
    case MIRROR_VERTICAL:   return a & 0x07FF;
    case MIRROR_HORIZONTAL: return ((a >> 1) & 0x0400) | (a & 0x03FF);
    case MIRROR_SINGLE_LO:  return a & 0x03FF;
    case MIRROR_SINGLE_HI:  return 0x0400 | (a & 0x03FF);
    case MIRROR_FOUR_SCREEN:return a;
    }
    return a & 0x07FF;
}
