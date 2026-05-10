#include "nes.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <SDL2/SDL.h>

static NES nes;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_dev;
static bool rom_loaded = false;

#define AUDIO_SAMPLE_RATE 44100

/* ---- ring buffer for audio (lock-free SPSC) ---- */
#define RING_BITS 14
#define RING_SIZE (1 << RING_BITS)
#define RING_MASK (RING_SIZE - 1)
static float ring_buf[RING_SIZE];
static volatile int ring_wr = 0;
static volatile int ring_rd = 0;

static void audio_cb(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    float *out = (float *)stream;
    int samples = len / sizeof(float);
    int r = ring_rd;
    int w = ring_wr;
    for (int i = 0; i < samples; i++) {
        if (r != w) {
            out[i] = ring_buf[r & RING_MASK];
            r++;
        } else {
            out[i] = 0.0f;
        }
    }
    ring_rd = r;
}

static void ring_push(const float *data, int count) {
    int w = ring_wr;
    int r = ring_rd;
    for (int i = 0; i < count; i++) {
        if ((w - r) < RING_SIZE) {
            ring_buf[w & RING_MASK] = data[i];
            w++;
        }
    }
    ring_wr = w;
}

/* ---- input ---- */
static uint8_t get_controller_state(const Uint8 *keys) {
    uint8_t state = 0;
    if (keys[SDL_SCANCODE_Z])                              state |= BTN_A;
    if (keys[SDL_SCANCODE_X])                              state |= BTN_B;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_BACKSPACE]) state |= BTN_SELECT;
    if (keys[SDL_SCANCODE_RETURN])                         state |= BTN_START;
    if (keys[SDL_SCANCODE_UP])                             state |= BTN_UP;
    if (keys[SDL_SCANCODE_DOWN])                           state |= BTN_DOWN;
    if (keys[SDL_SCANCODE_LEFT])                           state |= BTN_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])                          state |= BTN_RIGHT;
    return state;
}

/* ---- main loop with frame timing ---- */
static double last_time = 0;

static void main_loop(void) {
    if (!rom_loaded) return;

    double now = emscripten_get_now();
    if (last_time == 0.0) { last_time = now; return; }
    double dt = now - last_time;
    if (dt < 16.0) return;
    last_time = now;
    if (dt > 33.0) last_time = now;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_KEYDOWN) {
            if (event.key.keysym.sym == SDLK_r && (event.key.keysym.mod & KMOD_CTRL))
                nes_reset(&nes);
        }
    }

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    nes_set_controller(&nes, 0, get_controller_state(keys));

    nes_step_frame(&nes);

    if (nes.apu.audio_buf_pos > 0) {
        ring_push(nes.apu.audio_buf, nes.apu.audio_buf_pos);
        nes.apu.audio_buf_pos = 0;
    }

    SDL_UpdateTexture(texture, NULL, nes.ppu.framebuffer, 256 * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* ---- ROM loading ---- */
static void start_audio(void) {
    if (audio_dev > 0) {
        SDL_PauseAudioDevice(audio_dev, 0);
        EM_ASM({
            if (typeof Module !== 'undefined' && Module.SDL2 && Module.SDL2.audioContext) {
                Module.SDL2.audioContext.resume();
            }
        });
    }
}

static void on_rom_loaded(void *arg, void *data, int size) {
    (void)arg;
    if (nes_load_rom(&nes, (const uint8_t *)data, size)) {
        rom_loaded = true;
        last_time = 0;
        start_audio();
    } else {
        fprintf(stderr, "Failed to load ROM\n");
    }
}

static void on_rom_error(void *arg) {
    (void)arg;
    fprintf(stderr, "Failed to download ROM\n");
}

EMSCRIPTEN_KEEPALIVE
void load_rom_from_url(const char *url) {
    emscripten_async_wget_data(url, NULL, on_rom_loaded, on_rom_error);
}

EMSCRIPTEN_KEEPALIVE
void load_rom_from_data(const uint8_t *data, int size) {
    if (nes_load_rom(&nes, data, size)) {
        rom_loaded = true;
        last_time = 0;
        start_audio();
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    nes_init(&nes, AUDIO_SAMPLE_RATE);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    window = SDL_CreateWindow("NES Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * 3, 240 * 3, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(renderer, 256, 240);
    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    want.userdata = NULL;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);

    EM_ASM({
        if (window.nesromurl) {
            var url = window.nesromurl;
            var len = lengthBytesUTF8(url) + 1;
            var buf = _malloc(len);
            stringToUTF8(url, buf, len);
            _load_rom_from_url(buf);
            _free(buf);
        }

        window.loadNESRom = function(url) {
            var len = lengthBytesUTF8(url) + 1;
            var buf = _malloc(len);
            stringToUTF8(url, buf, len);
            _load_rom_from_url(buf);
            _free(buf);
        };

        var canvas = document.getElementById('canvas');
        if (canvas) {
            canvas.addEventListener('dragover', function(e) { e.preventDefault(); });
            canvas.addEventListener('drop', function(e) {
                e.preventDefault();
                var file = e.dataTransfer.files[0];
                if (!file) return;
                var reader = new FileReader();
                reader.onload = function(ev) {
                    var data = new Uint8Array(ev.target.result);
                    var buf = _malloc(data.length);
                    HEAPU8.set(data, buf);
                    _load_rom_from_data(buf, data.length);
                    _free(buf);
                };
                reader.readAsArrayBuffer(file);
            });
        }
    });

    emscripten_set_main_loop(main_loop, 0, 1);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    nes_destroy(&nes);
    return 0;
}

#endif
