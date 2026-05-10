#ifndef NES_H
#define NES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct NES NES;

/* CPU status flags */
#define FLAG_C 0x01
#define FLAG_Z 0x02
#define FLAG_I 0x04
#define FLAG_D 0x08
#define FLAG_B 0x10
#define FLAG_U 0x20
#define FLAG_V 0x40
#define FLAG_N 0x80

typedef struct {
    uint8_t  a, x, y, sp, status;
    uint16_t pc;
    bool     nmi_pending;
    bool     irq_line;
    uint64_t total_cycles;
    bool     dma_active;
    bool     dma_dummy;
    uint8_t  dma_page;
    uint8_t  dma_addr;
    uint8_t  dma_data;
} CPU;

/* PPU */
typedef struct {
    uint8_t  ctrl;
    uint8_t  mask;
    uint8_t  status;
    uint8_t  oam_addr;

    uint16_t v;    /* current VRAM address (15-bit) */
    uint16_t t;    /* temporary VRAM address (15-bit) */
    uint8_t  fine_x;
    bool     w;    /* write toggle */
    uint8_t  data_buf;

    int      scanline;
    int      dot;
    bool     odd_frame;
    bool     frame_complete;

    /* background shift registers */
    uint16_t bg_shift_lo;
    uint16_t bg_shift_hi;
    uint16_t at_shift_lo;
    uint16_t at_shift_hi;

    /* background latches */
    uint8_t  nt_latch;
    uint8_t  at_latch;
    uint8_t  bg_lo_latch;
    uint8_t  bg_hi_latch;

    /* sprite evaluation */
    uint8_t  oam[256];
    uint8_t  soam[32];
    uint8_t  sprite_count;
    uint8_t  spr_pattern_lo[8];
    uint8_t  spr_pattern_hi[8];
    uint8_t  spr_x[8];
    uint8_t  spr_attr[8];
    uint8_t  spr_index[8];
    bool     spr_zero_on_line;
    bool     spr_zero_possible;

    bool     nmi_output;
    bool     nmi_occurred;

    uint8_t  nametable[2048];
    uint8_t  palette[32];
    uint8_t  open_bus;

    uint32_t framebuffer[256 * 240];
} PPU;

/* APU channels */
typedef struct {
    bool     enabled;
    uint8_t  duty;
    bool     halt;
    bool     constant_vol;
    uint8_t  vol;
    bool     sweep_enable;
    uint8_t  sweep_period;
    bool     sweep_negate;
    uint8_t  sweep_shift;
    bool     sweep_reload;
    uint8_t  sweep_counter;
    uint16_t timer_period;
    uint16_t timer;
    uint8_t  duty_pos;
    uint8_t  length;
    uint8_t  env_vol;
    uint8_t  env_div;
    bool     env_start;
    uint8_t  output;
} APU_Pulse;

typedef struct {
    bool     enabled;
    bool     halt;
    uint8_t  linear_load;
    uint16_t timer_period;
    uint16_t timer;
    uint8_t  length;
    uint8_t  linear;
    bool     linear_reload;
    uint8_t  seq_pos;
    uint8_t  output;
} APU_Triangle;

typedef struct {
    bool     enabled;
    bool     halt;
    bool     constant_vol;
    uint8_t  vol;
    bool     mode;
    uint16_t timer_period;
    uint16_t timer;
    uint8_t  length;
    uint8_t  env_vol;
    uint8_t  env_div;
    bool     env_start;
    uint16_t shift_reg;
    uint8_t  output;
} APU_Noise;

typedef struct {
    bool     enabled;
    bool     irq_enable;
    bool     loop;
    uint8_t  output;
    uint16_t sample_addr;
    uint16_t sample_len;
    uint16_t cur_addr;
    uint16_t bytes_left;
    uint8_t  sample_buf;
    bool     buf_empty;
    uint8_t  shift_reg;
    uint8_t  bits_left;
    uint16_t timer_period;
    uint16_t timer;
    bool     silence;
    bool     irq_flag;
} APU_DMC;

typedef struct {
    APU_Pulse    pulse[2];
    APU_Triangle triangle;
    APU_Noise    noise;
    APU_DMC      dmc;
    uint8_t      frame_mode;
    bool         frame_irq_inhibit;
    bool         frame_irq;
    uint32_t     frame_counter;
    uint64_t     cycles;

    float       *audio_buf;
    int          audio_buf_size;
    int          audio_buf_pos;
    int          sample_rate;
    double       sample_timer;
    double       sample_period;
} APU;

/* Mirroring */
typedef enum {
    MIRROR_HORIZONTAL,
    MIRROR_VERTICAL,
    MIRROR_SINGLE_LO,
    MIRROR_SINGLE_HI,
    MIRROR_FOUR_SCREEN
} MirrorMode;

/* Cartridge + mapper state */
typedef struct {
    uint8_t    *prg_rom;
    uint8_t    *chr_rom;
    uint8_t    *prg_ram;
    uint8_t    *chr_ram;
    int         prg_size;
    int         chr_size;
    int         prg_ram_size;
    int         chr_ram_size;
    int         mapper_id;
    MirrorMode  mirror;
    bool        battery;
    bool        irq;
    int         prg_banks;
    int         chr_banks;

    union {
        struct { uint8_t sr; uint8_t cnt; uint8_t ctrl; uint8_t chr0; uint8_t chr1; uint8_t prg; } mmc1;
        struct { uint8_t bank; } uxrom;
        struct { uint8_t bank; } cnrom;
        struct {
            uint8_t bank_sel; uint8_t banks[8]; bool prg_mode; bool chr_mode;
            uint8_t irq_latch; uint8_t irq_counter; bool irq_enable; bool irq_reload;
        } mmc3;
        struct { uint8_t bank; } axrom;
    } m;
} Cartridge;

/* Main NES system */
struct NES {
    CPU        cpu;
    PPU        ppu;
    APU        apu;
    Cartridge  cart;
    uint8_t    ram[2048];
    uint8_t    ctrl_state[2];
    uint8_t    ctrl_shift[2];
    bool       ctrl_strobe;
    bool       running;
};

/* Standard NTSC palette */
extern const uint32_t nes_palette[64];

/* Controller buttons */
#define BTN_A      0x01
#define BTN_B      0x02
#define BTN_SELECT 0x04
#define BTN_START  0x08
#define BTN_UP     0x10
#define BTN_DOWN   0x20
#define BTN_LEFT   0x40
#define BTN_RIGHT  0x80

/* CPU */
void    cpu_init(NES *nes);
void    cpu_reset(NES *nes);
void    cpu_step(NES *nes);

/* PPU */
void    ppu_init(NES *nes);
void    ppu_reset(NES *nes);
void    ppu_step(NES *nes);
uint8_t ppu_read_reg(NES *nes, uint16_t addr);
void    ppu_write_reg(NES *nes, uint16_t addr, uint8_t val);

/* APU */
void    apu_init(NES *nes, int sample_rate);
void    apu_reset(NES *nes);
void    apu_step(NES *nes);
uint8_t apu_read_reg(NES *nes, uint16_t addr);
void    apu_write_reg(NES *nes, uint16_t addr, uint8_t val);
void    apu_destroy(NES *nes);

/* Cartridge */
bool    cart_load(NES *nes, const uint8_t *data, int size);
void    cart_destroy(NES *nes);
uint8_t cart_cpu_read(NES *nes, uint16_t addr);
void    cart_cpu_write(NES *nes, uint16_t addr, uint8_t val);
uint8_t cart_ppu_read(NES *nes, uint16_t addr);
void    cart_ppu_write(NES *nes, uint16_t addr, uint8_t val);
void    cart_scanline(NES *nes);
uint16_t cart_mirror_addr(NES *nes, uint16_t addr);

/* Bus */
uint8_t bus_read(NES *nes, uint16_t addr);
void    bus_write(NES *nes, uint16_t addr, uint8_t val);

/* NES top-level */
void    nes_init(NES *nes, int sample_rate);
void    nes_destroy(NES *nes);
bool    nes_load_rom(NES *nes, const uint8_t *data, int size);
void    nes_reset(NES *nes);
void    nes_step_frame(NES *nes);
void    nes_tick(NES *nes);
void    nes_set_controller(NES *nes, int pad, uint8_t buttons);

#endif
