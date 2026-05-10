#include "nes.h"

uint8_t bus_read(NES *n, uint16_t addr) {
    if (addr < 0x2000)
        return n->ram[addr & 0x07FF];
    if (addr < 0x4000)
        return ppu_read_reg(n, addr);
    if (addr == 0x4015)
        return apu_read_reg(n, addr);
    if (addr == 0x4016) {
        uint8_t r = n->ctrl_shift[0] & 1;
        n->ctrl_shift[0] >>= 1;
        return r | 0x40;
    }
    if (addr == 0x4017) {
        uint8_t r = n->ctrl_shift[1] & 1;
        n->ctrl_shift[1] >>= 1;
        return r | 0x40;
    }
    if (addr < 0x4020)
        return 0;
    return cart_cpu_read(n, addr);
}

void bus_write(NES *n, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) {
        n->ram[addr & 0x07FF] = val;
        return;
    }
    if (addr < 0x4000) {
        ppu_write_reg(n, addr, val);
        return;
    }
    if (addr == 0x4014) {
        nes_tick(n);
        if (n->cpu.total_cycles & 1) nes_tick(n);
        for (int i = 0; i < 256; i++) {
            nes_tick(n);
            uint8_t data = bus_read(n, ((uint16_t)val << 8) | (uint8_t)i);
            nes_tick(n);
            n->ppu.oam[n->ppu.oam_addr++] = data;
        }
        return;
    }
    if (addr == 0x4016) {
        if (val & 1) {
            n->ctrl_strobe = true;
        } else if (n->ctrl_strobe) {
            n->ctrl_strobe = false;
            n->ctrl_shift[0] = n->ctrl_state[0];
            n->ctrl_shift[1] = n->ctrl_state[1];
        }
        return;
    }
    if (addr < 0x4018) {
        apu_write_reg(n, addr, val);
        return;
    }
    if (addr >= 0x4020) {
        cart_cpu_write(n, addr, val);
    }
}

void nes_tick(NES *n) {
    ppu_step(n);
    ppu_step(n);
    ppu_step(n);
    apu_step(n);
    n->cpu.total_cycles++;
}

void nes_init(NES *n, int sample_rate) {
    memset(n, 0, sizeof(NES));
    cpu_init(n);
    ppu_init(n);
    apu_init(n, sample_rate);
}

void nes_destroy(NES *n) {
    apu_destroy(n);
    cart_destroy(n);
}

bool nes_load_rom(NES *n, const uint8_t *data, int size) {
    if (!cart_load(n, data, size)) return false;
    nes_reset(n);
    return true;
}

void nes_reset(NES *n) {
    cpu_reset(n);
    ppu_reset(n);
    apu_reset(n);
    n->ctrl_strobe = false;
    n->ctrl_state[0] = 0;
    n->ctrl_state[1] = 0;
    n->ctrl_shift[0] = 0;
    n->ctrl_shift[1] = 0;
    n->running = true;
}

void nes_step_frame(NES *n) {
    n->ppu.frame_complete = false;
    while (!n->ppu.frame_complete) {
        cpu_step(n);
    }
}

void nes_set_controller(NES *n, int pad, uint8_t buttons) {
    if (pad >= 0 && pad < 2) {
        n->ctrl_state[pad] = buttons;
        if (n->ctrl_strobe)
            n->ctrl_shift[pad] = buttons;
    }
}
