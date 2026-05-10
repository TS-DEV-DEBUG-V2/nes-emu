#include "nes.h"
#include <math.h>

static const uint8_t length_table[32] = {
    10,254,20, 2,40, 4,80, 6,160, 8,60,10,14,12,26,14,
    12, 16,24,18,48,20,96,22,192,24,72,26,16,28,32,30
};

static const uint8_t duty_table[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1},
};

static const uint8_t triangle_seq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
};

static const uint16_t noise_period_table[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068
};

static const uint16_t dmc_rate_table[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54
};

typedef struct {
    float prev_in;
    float prev_out;
    float b0, b1, a1;
} Filter;

static void filter_hp_init(Filter *f, float cutoff, float sample_rate) {
    float rc = 1.0f / (2.0f * 3.14159265f * cutoff);
    float dt = 1.0f / sample_rate;
    float alpha = rc / (rc + dt);
    f->b0 = alpha;
    f->b1 = -alpha;
    f->a1 = -alpha;
    f->prev_in = 0;
    f->prev_out = 0;
}

static void filter_lp_init(Filter *f, float cutoff, float sample_rate) {
    float rc = 1.0f / (2.0f * 3.14159265f * cutoff);
    float dt = 1.0f / sample_rate;
    float alpha = dt / (rc + dt);
    f->b0 = alpha;
    f->b1 = 0;
    f->a1 = -(1.0f - alpha);
    f->prev_in = 0;
    f->prev_out = 0;
}

static float filter_apply(Filter *f, float in) {
    float out = f->b0 * in + f->b1 * f->prev_in - f->a1 * f->prev_out;
    f->prev_in = in;
    f->prev_out = out;
    return out;
}

static Filter hp_filter_90;
static Filter hp_filter_440;
static Filter lp_filter_14k;

void apu_init(NES *n, int sample_rate) {
    memset(&n->apu, 0, sizeof(APU));
    n->apu.noise.shift_reg = 1;
    n->apu.dmc.buf_empty = true;
    n->apu.dmc.bits_left = 8;
    n->apu.dmc.silence = true;
    n->apu.sample_rate = sample_rate;
    n->apu.sample_period = 1789773.0 / sample_rate;
    n->apu.audio_buf_size = sample_rate / 5;
    n->apu.audio_buf = (float *)calloc(n->apu.audio_buf_size, sizeof(float));

    filter_hp_init(&hp_filter_90, 90.0f, (float)sample_rate);
    filter_hp_init(&hp_filter_440, 440.0f, (float)sample_rate);
    filter_lp_init(&lp_filter_14k, 14000.0f, (float)sample_rate);
}

void apu_reset(NES *n) {
    APU *a = &n->apu;
    a->frame_counter = 0;
    a->cycles = 0;
    a->audio_buf_pos = 0;
    a->sample_timer = 0;
    memset(&a->pulse[0], 0, sizeof(APU_Pulse));
    memset(&a->pulse[1], 0, sizeof(APU_Pulse));
    memset(&a->triangle, 0, sizeof(APU_Triangle));
    memset(&a->noise, 0, sizeof(APU_Noise));
    memset(&a->dmc, 0, sizeof(APU_DMC));
    a->noise.shift_reg = 1;
    a->dmc.buf_empty = true;
    a->dmc.bits_left = 8;
    a->dmc.silence = true;
}

void apu_destroy(NES *n) {
    free(n->apu.audio_buf);
    n->apu.audio_buf = NULL;
}

static int sweep_target(APU_Pulse *p, int channel) {
    int delta = p->timer_period >> p->sweep_shift;
    if (p->sweep_negate) {
        delta = -delta;
        if (channel == 0) delta--;
    }
    return (int)p->timer_period + delta;
}

static bool sweep_muting(APU_Pulse *p, int channel) {
    return p->timer_period < 8 || sweep_target(p, channel) > 0x7FF;
}

static void clock_envelope(APU_Pulse *p) {
    if (p->env_start) {
        p->env_start = false;
        p->env_vol = 15;
        p->env_div = p->vol;
    } else {
        if (p->env_div > 0) {
            p->env_div--;
        } else {
            p->env_div = p->vol;
            if (p->env_vol > 0)
                p->env_vol--;
            else if (p->halt)
                p->env_vol = 15;
        }
    }
}

static void clock_noise_envelope(APU_Noise *ns) {
    if (ns->env_start) {
        ns->env_start = false;
        ns->env_vol = 15;
        ns->env_div = ns->vol;
    } else {
        if (ns->env_div > 0) {
            ns->env_div--;
        } else {
            ns->env_div = ns->vol;
            if (ns->env_vol > 0)
                ns->env_vol--;
            else if (ns->halt)
                ns->env_vol = 15;
        }
    }
}

static void clock_sweep(APU_Pulse *p, int channel) {
    if (p->sweep_counter == 0 && p->sweep_enable && p->sweep_shift > 0 && !sweep_muting(p, channel)) {
        int target = sweep_target(p, channel);
        if (target >= 0 && target <= 0x7FF)
            p->timer_period = (uint16_t)target;
    }
    if (p->sweep_counter == 0 || p->sweep_reload) {
        p->sweep_counter = p->sweep_period;
        p->sweep_reload = false;
    } else {
        p->sweep_counter--;
    }
}

static void clock_length(APU_Pulse *p) {
    if (!p->halt && p->length > 0) p->length--;
}

static void clock_tri_length(APU_Triangle *t) {
    if (!t->halt && t->length > 0) t->length--;
}

static void clock_noise_length(APU_Noise *ns) {
    if (!ns->halt && ns->length > 0) ns->length--;
}

static void clock_tri_linear(APU_Triangle *t) {
    if (t->linear_reload) {
        t->linear = t->linear_load;
    } else if (t->linear > 0) {
        t->linear--;
    }
    if (!t->halt) t->linear_reload = false;
}

static void quarter_frame(NES *n) {
    APU *a = &n->apu;
    clock_envelope(&a->pulse[0]);
    clock_envelope(&a->pulse[1]);
    clock_tri_linear(&a->triangle);
    clock_noise_envelope(&a->noise);
}

static void half_frame(NES *n) {
    APU *a = &n->apu;
    clock_length(&a->pulse[0]);
    clock_length(&a->pulse[1]);
    clock_sweep(&a->pulse[0], 0);
    clock_sweep(&a->pulse[1], 1);
    clock_tri_length(&a->triangle);
    clock_noise_length(&a->noise);
}

static void tick_pulse(APU_Pulse *p, int channel) {
    if (p->timer > 0) {
        p->timer--;
    } else {
        p->timer = p->timer_period;
        p->duty_pos = (p->duty_pos + 1) & 7;
    }
    uint8_t vol = p->constant_vol ? p->vol : p->env_vol;
    if (!p->enabled || p->length == 0 || sweep_muting(p, channel) || duty_table[p->duty][p->duty_pos] == 0)
        p->output = 0;
    else
        p->output = vol;
}

static void tick_triangle(APU_Triangle *t) {
    if (t->timer > 0) {
        t->timer--;
    } else {
        t->timer = t->timer_period;
        if (t->length > 0 && t->linear > 0)
            t->seq_pos = (t->seq_pos + 1) & 31;
    }
    if (!t->enabled || t->length == 0 || t->linear == 0)
        t->output = triangle_seq[t->seq_pos];
    else if (t->timer_period < 2)
        t->output = 7;
    else
        t->output = triangle_seq[t->seq_pos];
}

static void tick_noise(APU_Noise *ns) {
    if (ns->timer > 0) {
        ns->timer--;
    } else {
        ns->timer = ns->timer_period;
        uint16_t fb = ns->mode
            ? ((ns->shift_reg & 1) ^ ((ns->shift_reg >> 6) & 1))
            : ((ns->shift_reg & 1) ^ ((ns->shift_reg >> 1) & 1));
        ns->shift_reg = (ns->shift_reg >> 1) | (fb << 14);
    }
    uint8_t vol = ns->constant_vol ? ns->vol : ns->env_vol;
    if (!ns->enabled || ns->length == 0 || (ns->shift_reg & 1))
        ns->output = 0;
    else
        ns->output = vol;
}

static void tick_dmc(NES *n) {
    APU_DMC *d = &n->apu.dmc;

    if (d->buf_empty && d->bytes_left > 0) {
        d->sample_buf = bus_read(n, d->cur_addr);
        d->buf_empty = false;
        d->cur_addr = (d->cur_addr + 1) | 0x8000;
        d->bytes_left--;
        if (d->bytes_left == 0) {
            if (d->loop) {
                d->cur_addr = d->sample_addr;
                d->bytes_left = d->sample_len;
            } else if (d->irq_enable) {
                d->irq_flag = true;
            }
        }
    }

    if (d->timer > 0) { d->timer--; return; }
    d->timer = d->timer_period;

    if (!d->silence) {
        if (d->shift_reg & 1) {
            if (d->output <= 125) d->output += 2;
        } else {
            if (d->output >= 2) d->output -= 2;
        }
    }
    d->shift_reg >>= 1;
    d->bits_left--;
    if (d->bits_left == 0) {
        d->bits_left = 8;
        if (d->buf_empty) {
            d->silence = true;
        } else {
            d->silence = false;
            d->shift_reg = d->sample_buf;
            d->buf_empty = true;
        }
    }
}

static float apu_mix(APU *a) {
    float pulse_out = 0;
    float p1 = (float)a->pulse[0].output;
    float p2 = (float)a->pulse[1].output;
    if (p1 + p2 > 0)
        pulse_out = 95.88f / (8128.0f / (p1 + p2) + 100.0f);

    float tnd_out = 0;
    float t = (float)a->triangle.output;
    float ns = (float)a->noise.output;
    float d = (float)a->dmc.output;
    float denom = t / 8227.0f + ns / 12241.0f + d / 22638.0f;
    if (denom > 0)
        tnd_out = 159.79f / (1.0f / denom + 100.0f);

    return pulse_out + tnd_out;
}

void apu_step(NES *n) {
    APU *a = &n->apu;
    uint64_t fc = a->frame_counter;

    if (a->frame_mode == 0) {
        if      (fc == 7457)  quarter_frame(n);
        else if (fc == 14913) { quarter_frame(n); half_frame(n); }
        else if (fc == 22371) quarter_frame(n);
        else if (fc == 29828) {
            if (!a->frame_irq_inhibit) a->frame_irq = true;
        }
        else if (fc == 29829) {
            quarter_frame(n); half_frame(n);
            if (!a->frame_irq_inhibit) a->frame_irq = true;
        }
        else if (fc == 29830) {
            if (!a->frame_irq_inhibit) a->frame_irq = true;
            a->frame_counter = (uint64_t)-1;
        }
    } else {
        if      (fc == 7457)  quarter_frame(n);
        else if (fc == 14913) { quarter_frame(n); half_frame(n); }
        else if (fc == 22371) quarter_frame(n);
        else if (fc == 37281) { quarter_frame(n); half_frame(n); }
        else if (fc == 37282) {
            a->frame_counter = (uint64_t)-1;
        }
    }

    if (a->cycles & 1) {
        tick_pulse(&a->pulse[0], 0);
        tick_pulse(&a->pulse[1], 1);
        tick_noise(&a->noise);
        tick_dmc(n);
    }
    tick_triangle(&a->triangle);

    n->cpu.irq_line = a->frame_irq || a->dmc.irq_flag || n->cart.irq;

    a->sample_timer += 1.0;
    if (a->sample_timer >= a->sample_period) {
        a->sample_timer -= a->sample_period;
        if (a->audio_buf_pos < a->audio_buf_size) {
            float raw = apu_mix(a);
            raw = filter_apply(&hp_filter_90, raw);
            raw = filter_apply(&hp_filter_440, raw);
            raw = filter_apply(&lp_filter_14k, raw);
            a->audio_buf[a->audio_buf_pos++] = raw;
        }
    }

    a->cycles++;
    a->frame_counter++;
}

uint8_t apu_read_reg(NES *n, uint16_t addr) {
    APU *a = &n->apu;
    if (addr == 0x4015) {
        uint8_t r = 0;
        if (a->pulse[0].length > 0) r |= 0x01;
        if (a->pulse[1].length > 0) r |= 0x02;
        if (a->triangle.length > 0) r |= 0x04;
        if (a->noise.length > 0)    r |= 0x08;
        if (a->dmc.bytes_left > 0)  r |= 0x10;
        if (a->frame_irq)           r |= 0x40;
        if (a->dmc.irq_flag)        r |= 0x80;
        a->frame_irq = false;
        return r;
    }
    return 0;
}

void apu_write_reg(NES *n, uint16_t addr, uint8_t val) {
    APU *a = &n->apu;
    int ch;

    switch (addr) {
    case 0x4000: case 0x4004:
        ch = (addr >= 0x4004) ? 1 : 0;
        a->pulse[ch].duty = (val >> 6) & 3;
        a->pulse[ch].halt = (val & 0x20) != 0;
        a->pulse[ch].constant_vol = (val & 0x10) != 0;
        a->pulse[ch].vol = val & 0x0F;
        break;
    case 0x4001: case 0x4005:
        ch = (addr >= 0x4004) ? 1 : 0;
        a->pulse[ch].sweep_enable = (val & 0x80) != 0;
        a->pulse[ch].sweep_period = (val >> 4) & 7;
        a->pulse[ch].sweep_negate = (val & 0x08) != 0;
        a->pulse[ch].sweep_shift = val & 7;
        a->pulse[ch].sweep_reload = true;
        break;
    case 0x4002: case 0x4006:
        ch = (addr >= 0x4004) ? 1 : 0;
        a->pulse[ch].timer_period = (a->pulse[ch].timer_period & 0x700) | val;
        break;
    case 0x4003: case 0x4007:
        ch = (addr >= 0x4004) ? 1 : 0;
        a->pulse[ch].timer_period = (a->pulse[ch].timer_period & 0xFF) | ((val & 7) << 8);
        if (a->pulse[ch].enabled)
            a->pulse[ch].length = length_table[val >> 3];
        a->pulse[ch].duty_pos = 0;
        a->pulse[ch].env_start = true;
        break;

    case 0x4008:
        a->triangle.halt = (val & 0x80) != 0;
        a->triangle.linear_load = val & 0x7F;
        break;
    case 0x400A:
        a->triangle.timer_period = (a->triangle.timer_period & 0x700) | val;
        break;
    case 0x400B:
        a->triangle.timer_period = (a->triangle.timer_period & 0xFF) | ((val & 7) << 8);
        if (a->triangle.enabled)
            a->triangle.length = length_table[val >> 3];
        a->triangle.linear_reload = true;
        break;

    case 0x400C:
        a->noise.halt = (val & 0x20) != 0;
        a->noise.constant_vol = (val & 0x10) != 0;
        a->noise.vol = val & 0x0F;
        break;
    case 0x400E:
        a->noise.mode = (val & 0x80) != 0;
        a->noise.timer_period = noise_period_table[val & 0x0F];
        break;
    case 0x400F:
        if (a->noise.enabled)
            a->noise.length = length_table[val >> 3];
        a->noise.env_start = true;
        break;

    case 0x4010:
        a->dmc.irq_enable = (val & 0x80) != 0;
        a->dmc.loop = (val & 0x40) != 0;
        a->dmc.timer_period = dmc_rate_table[val & 0x0F];
        if (!a->dmc.irq_enable) a->dmc.irq_flag = false;
        break;
    case 0x4011:
        a->dmc.output = val & 0x7F;
        break;
    case 0x4012:
        a->dmc.sample_addr = 0xC000 | ((uint16_t)val << 6);
        break;
    case 0x4013:
        a->dmc.sample_len = ((uint16_t)val << 4) | 1;
        break;

    case 0x4015:
        a->pulse[0].enabled = (val & 0x01) != 0;
        a->pulse[1].enabled = (val & 0x02) != 0;
        a->triangle.enabled = (val & 0x04) != 0;
        a->noise.enabled    = (val & 0x08) != 0;
        a->dmc.enabled      = (val & 0x10) != 0;
        if (!a->pulse[0].enabled) a->pulse[0].length = 0;
        if (!a->pulse[1].enabled) a->pulse[1].length = 0;
        if (!a->triangle.enabled) a->triangle.length = 0;
        if (!a->noise.enabled)    a->noise.length = 0;
        if (!a->dmc.enabled) {
            a->dmc.bytes_left = 0;
        } else if (a->dmc.bytes_left == 0) {
            a->dmc.cur_addr = a->dmc.sample_addr;
            a->dmc.bytes_left = a->dmc.sample_len;
        }
        a->dmc.irq_flag = false;
        break;

    case 0x4017:
        a->frame_mode = (val & 0x80) ? 1 : 0;
        a->frame_irq_inhibit = (val & 0x40) != 0;
        if (a->frame_irq_inhibit) a->frame_irq = false;
        a->frame_counter = 0;
        if (a->frame_mode == 1) {
            quarter_frame(n);
            half_frame(n);
        }
        break;
    }
}