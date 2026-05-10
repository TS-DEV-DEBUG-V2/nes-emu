#include "nes.h"
#include <SDL2/SDL.h>

#define WINDOW_SCALE 3
#define AUDIO_SAMPLE_RATE 44100

static NES nes;
static bool running = true;

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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: nesemu <rom.nes>\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom_data = (uint8_t *)malloc(fsize);
    fread(rom_data, 1, fsize, f);
    fclose(f);

    nes_init(&nes, AUDIO_SAMPLE_RATE);
    if (!nes_load_rom(&nes, rom_data, (int)fsize)) {
        fprintf(stderr, "Failed to load ROM\n");
        free(rom_data);
        return 1;
    }
    free(rom_data);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "nesemu by TS copyright 2026",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * WINDOW_SCALE, 240 * WINDOW_SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, 256, 240);
    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);

    /* push-mode audio (no callback, no threading race) */
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = NULL;

    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev > 0) SDL_PauseAudioDevice(audio_dev, 0);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_r && (event.key.keysym.mod & KMOD_CTRL))
                    nes_reset(&nes);
            }
        }

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        nes_set_controller(&nes, 0, get_controller_state(keys));

        nes_step_frame(&nes);

        /* queue this frame's audio samples */
        if (audio_dev > 0 && nes.apu.audio_buf_pos > 0) {
            SDL_QueueAudio(audio_dev, nes.apu.audio_buf,
                           nes.apu.audio_buf_pos * sizeof(float));
            nes.apu.audio_buf_pos = 0;
        }

        /* throttle if audio queue is getting ahead */
        if (audio_dev > 0) {
            while (SDL_GetQueuedAudioSize(audio_dev) > AUDIO_SAMPLE_RATE * sizeof(float) / 15)
                SDL_Delay(1);
        }

        SDL_UpdateTexture(texture, NULL, nes.ppu.framebuffer, 256 * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    nes_destroy(&nes);
    return 0;
}
