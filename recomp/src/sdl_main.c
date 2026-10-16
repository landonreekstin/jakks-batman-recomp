/* SDL2 front end: play The Batman natively.
 *
 *   batman <batman.bin> [--save file.sav] [--scale N] [--frames N] [--record script.txt]
 *
 * Keys: arrows = joypad, Z = A (jump), X = B (attack), C = X (search/use),
 *       Enter = Menu, Esc = quit. Game controllers are mapped the same way
 *       (south = A, west = B, north = X, start = Menu).
 *       F5 = save state, F8 = load state, F6/F7 = previous/next slot (0-9),
 *       Backspace (hold) = rewind. States go to <save file minus .sav>.st<slot>.
 * The 24C04 save EEPROM is kept in --save (default batman.sav next to the ROM).
 *
 * --record writes the controller state as an input script (tests/inputs format), so
 * the session can be replayed in MAME and batman-rt by tests/check.sh. A recording
 * starts from a blank EEPROM, like the MAME capture, and saves to <script>.sav. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spg.h"
#include "state.h"

#define ROM_WORDS 0x200000

enum {
    BTN_UP = 0x8000, BTN_DOWN = 0x4000, BTN_LEFT = 0x2000, BTN_RIGHT = 0x1000,
    BTN_A = 0x0800, BTN_MENU = 0x0400, BTN_B = 0x0200, BTN_X = 0x0100,
};

static SDL_AudioDeviceID audio_dev;
static int16_t audio_buf[4096 * 2];
static int audio_len;

static void audio_sample(void *ctx, int16_t l, int16_t r)
{
    (void)ctx;
    audio_buf[audio_len * 2] = l;
    audio_buf[audio_len * 2 + 1] = r;
    if (++audio_len == 4096) {
        if (audio_dev) SDL_QueueAudio(audio_dev, audio_buf, sizeof(audio_buf));
        audio_len = 0;
    }
}

static void audio_flush(void)
{
    if (audio_dev && audio_len) SDL_QueueAudio(audio_dev, audio_buf, (Uint32)(audio_len * 4));
    audio_len = 0;
}

/* rewind: a snapshot every REWIND_EVERY frames, REWIND_SLOTS deep (~32 KB each) */
#define REWIND_EVERY 4
#define REWIND_SLOTS 900 /* 60 s */
static spg_snap_t *rewind_buf;
static int rewind_head, rewind_len;

static void rewind_push(const spg_t *m)
{
    if (!rewind_buf) rewind_buf = malloc(sizeof(spg_snap_t) * REWIND_SLOTS);
    if (!rewind_buf) return;
    spg_snapshot(m, &rewind_buf[rewind_head]);
    rewind_head = (rewind_head + 1) % REWIND_SLOTS;
    if (rewind_len < REWIND_SLOTS) rewind_len++;
}

static int rewind_pop(spg_t *m)
{
    if (!rewind_len) return 0;
    rewind_head = (rewind_head + REWIND_SLOTS - 1) % REWIND_SLOTS;
    rewind_len--;
    spg_restore(m, &rewind_buf[rewind_head]);
    return 1;
}

/* a recording only replays if the run was uninterrupted, so it ends at a state load or rewind */
static void stop_recording(FILE **rec, const char *path, uint32_t frame)
{
    fprintf(*rec, "# end at vblank %u: tests/check.sh <workdir> %u with INPUT=%s\n", frame, frame, path);
    fclose(*rec);
    *rec = NULL;
    fprintf(stderr, "recorded %u vblanks to %s\n", frame, path);
}

static void set_title(SDL_Window *win, int slot, const char *msg)
{
    char t[256];
    snprintf(t, sizeof(t), "The Batman  [slot %d]%s%s", slot, msg ? "  " : "", msg ? msg : "");
    SDL_SetWindowTitle(win, t);
}

static uint16_t *load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    uint8_t *raw = malloc(ROM_WORDS * 2);
    size_t n = fread(raw, 1, ROM_WORDS * 2, f);
    fclose(f);
    if (n != ROM_WORDS * 2) { fprintf(stderr, "%s: expected %d bytes, got %zu\n", path, ROM_WORDS * 2, n); return NULL; }
    uint16_t *rom = malloc(ROM_WORDS * 2);
    for (size_t i = 0; i < ROM_WORDS; i++) rom[i] = (uint16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
    free(raw);
    return rom;
}

static void save_eeprom(spg_t *m, const char *path)
{
    if (!m->eeprom.dirty) return;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(m->eeprom.data, 1, EEPROM_SIZE, f);
    fclose(f);
    m->eeprom.dirty = 0;
}

static uint16_t read_buttons(SDL_GameController *pad)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= BTN_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= BTN_RIGHT;
    if (k[SDL_SCANCODE_Z]) b |= BTN_A;
    if (k[SDL_SCANCODE_X]) b |= BTN_B;
    if (k[SDL_SCANCODE_C]) b |= BTN_X;
    if (k[SDL_SCANCODE_RETURN]) b |= BTN_MENU;
    if (pad) {
        const Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) b |= BTN_UP;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) b |= BTN_DOWN;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) b |= BTN_LEFT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) b |= BTN_RIGHT;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= BTN_A;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X)) b |= BTN_B;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) b |= BTN_X;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START)) b |= BTN_MENU;
    }
    return b;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL;
    char save_path[1024] = "";
    int scale = 3;
    long max_frames = -1;
    const char *record_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(save_path, sizeof(save_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record_path = argv[++i];
        else if (argv[i][0] != '-') rom_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s <batman.bin> [--save file.sav] [--scale N] [--frames N] [--record script.txt]\n", argv[0]);
        return 2;
    }
    FILE *rec = NULL;
    if (record_path) {
        rec = fopen(record_path, "w");
        if (!rec) { perror(record_path); return 1; }
        fprintf(rec, "# vblank hexmask (P1: 8000 up 4000 down 2000 left 1000 right 0800 A 0400 menu 0200 B 0100 X)\n"
                     "# recorded with recomp/build/batman --record, blank EEPROM\n");
        snprintf(save_path, sizeof(save_path), "%s.sav", record_path);
    } else if (!save_path[0]) {
        snprintf(save_path, sizeof(save_path), "%s", rom_path);
        char *dot = strrchr(save_path, '.');
        if (dot && !strchr(dot, '/')) *dot = 0;
        strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
    }

    uint16_t *rom = load_rom(rom_path);
    if (!rom) return 1;
    static spg_t m;
    spg_init(&m, rom);
    FILE *sf = rec ? NULL : fopen(save_path, "rb");
    if (sf) {
        if (fread(m.eeprom.data, 1, EEPROM_SIZE, sf) != EEPROM_SIZE) fprintf(stderr, "%s: short save file\n", save_path);
        fclose(sf);
    }
    uint8_t rom_hash[32];
    spg_rom_hash(rom, rom_hash);
    char state_base[1024];
    snprintf(state_base, sizeof(state_base), "%s", save_path);
    {
        const size_t n = strlen(state_base);
        if (n > 4 && !strcmp(state_base + n - 4, ".sav")) state_base[n - 4] = 0;
    }
    int slot = 0;
    spg_reset(&m);
    m.render = 1;
    m.spu.out = audio_sample;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("The Batman", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       PPU_W * scale, PPU_H * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, PPU_W, PPU_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, PPU_W, PPU_H);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = SPU_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* SDL resamples to the device */
    if (!audio_dev) fprintf(stderr, "no audio: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(audio_dev, 0);

    set_title(win, slot, NULL);
    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks() && !pad; i++)
        if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

    const Uint64 freq = SDL_GetPerformanceFrequency();
    const double frame_ticks = (double)freq / 60.0;
    double next = (double)SDL_GetPerformanceCounter();
    int running = 1;
    while (running && (max_frames < 0 || (long)m.frame < max_frames)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                char path[1100], msg[64];
                snprintf(path, sizeof(path), "%s.st%d", state_base, slot);
                switch (e.key.keysym.sym) {
                case SDLK_F5:
                    snprintf(msg, sizeof(msg), spg_state_save(&m, rom_hash, path) ? "saved" : "save FAILED");
                    set_title(win, slot, msg);
                    break;
                case SDLK_F8: {
                    const uint32_t was = m.frame;
                    if (spg_state_load(&m, rom_hash, path)) {
                        snprintf(msg, sizeof(msg), "loaded (vblank %u)", m.frame);
                        if (rec) stop_recording(&rec, record_path, was);
                        if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
                        next = (double)SDL_GetPerformanceCounter();
                    } else snprintf(msg, sizeof(msg), "no state in slot");
                    set_title(win, slot, msg);
                    break;
                }
                case SDLK_F6: slot = (slot + 9) % 10; set_title(win, slot, NULL); break;
                case SDLK_F7: slot = (slot + 1) % 10; set_title(win, slot, NULL); break;
                default: break;
                }
            }
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
        }
        if (SDL_GetKeyboardState(NULL)[SDL_SCANCODE_BACKSPACE]) {
            /* rewind: step back one snapshot per displayed frame (REWIND_EVERY x speed), silently */
            const uint32_t was = m.frame;
            if (rewind_pop(&m)) {
                if (rec) stop_recording(&rec, record_path, was);
                if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
            }
        } else {
            /* the buttons set here are what the CPU sees from vblank m.frame on, which is
             * what an input-script line "<m.frame> <mask>" means to batman-rt and capture.lua.
             * Nothing is pressed before vblank 1: neither harness applies input at vblank 0. */
            const uint16_t b = m.frame ? read_buttons(pad) : 0;
            if (rec && b != m.buttons) fprintf(rec, "%u %04x\n", m.frame, b);
            m.buttons = b;
            if (m.frame % REWIND_EVERY == 0) rewind_push(&m);
            if (!spg_run_frame(&m, NULL, NULL)) {
                fprintf(stderr, "CPU stopped at %06x\n", unsp_lpc(&m.cpu));
                break;
            }
            audio_flush();
        }

        SDL_UpdateTexture(tex, NULL, m.framebuffer, PPU_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        if (m.frame % 300 == 0) save_eeprom(&m, save_path);

        /* pace to 60 Hz; let the audio queue absorb jitter but never grow past ~4 frames */
        next += frame_ticks;
        const double now = (double)SDL_GetPerformanceCounter();
        if (next > now) SDL_Delay((Uint32)((next - now) * 1000.0 / (double)freq));
        else if (now - next > frame_ticks * 5) next = now; /* fell behind: resync */
        if (audio_dev && SDL_GetQueuedAudioSize(audio_dev) > (Uint32)(SPU_RATE / 60 * 4 * 4)) SDL_Delay(4);
    }

    save_eeprom(&m, save_path);
    if (rec) stop_recording(&rec, record_path, m.frame);
    if (pad) SDL_GameControllerClose(pad);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
