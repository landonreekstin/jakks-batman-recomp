/* SDL2 front end: play The Batman natively.
 *
 *   batman <batman.bin> [--save file.sav] [--scale N] [--filter mode] [--frames N]
 *                       [--record script.txt] [--mods a,b] [--mods-dir dir] [--coop]
 *
 * Keys: arrows = joypad, Z = A (jump), X = B (attack), C = X (search/use),
 *       Enter = Menu, Esc = quit. Game controllers are mapped the same way
 *       (south = A, west = B, north = X, start = Menu).
 *       F5 = save state, F8 = load state, F6/F7 = previous/next slot (0-9),
 *       Backspace (hold) = rewind, Tab (hold) = fast forward,
 *       F9 / Shift+F9 = next/previous CRT filter, F11 = fullscreen, F12 = screenshot,
 *       - / = (or keypad - / +) = volume down/up, M = mute.
 * --coop: two players (coop.c). P2's keys are W A S D, J = A, K = B, L = X, P = Menu. With two
 *       controllers each player gets one; with one, it is P2's. Recordings get a second mask.
 * The 24C04 save EEPROM is kept in --save (default batman.sav next to the ROM). States go
 * to <save file minus .sav>.st<slot>, screenshots to <save file minus .sav>-NNNN.png.
 * The volume (--volume 0-100, default 100) and the filter are remembered in
 * <save file minus .sav>.cfg.
 * It only scales what goes to the speakers; the emulated sound is unchanged.
 *
 * Netplay (net.h; implies --coop): --host [PORT] waits for player 2 (port 7845) while the game
 *       runs; --join ADDR[:PORT] joins a host as player 2, from the host's game. Each plays with
 *       P1's keys or any controller. --net-delay N sets the input delay (frames, 2). Rewind, state
 *       loads, fast forward and recording are off while connected, and the guest never writes its
 *       save file (its game is the host's).
 *
 * --record writes the controller state as an input script (tests/inputs format), so
 * the session can be replayed in MAME and batman-rt by tests/check.sh. A recording
 * starts from a blank EEPROM, like the MAME capture, and saves to <script>.sav.
 *
 * Mods (mod.h) come from --mods, or else from <mods-dir>/enabled.txt (default dir "mods").
 *
 * --versus [--players N] [--stocks N] [--stage N] [--kb2] [--quick]: the fighting mode (versus.h) instead of
 *       the game. It opens on the character select (Left/Right pick, B ready, X back; P1's
 *       Up/Down add or remove CPUs); --quick goes straight to a fight with the default picks.
 *       P1 has the keyboard (arrows, Z = jump, X = attack, C = special, V = block, Enter = pause)
 *       and the first controller; --kb2 makes P2 a human on W A S D, J, K, L, Left Shift, P. Each
 *       further controller takes the next CPU slot, also when plugged in during a match. On a
 *       controller: south = attack, east = special, west/north = jump, shoulders/triggers =
 *       block, Start = pause; Up also jumps. B (or A or Menu) on the result screen plays again.
 *       Paused, Special leaves the match for the select screen.
 * The story game's title menu gets a third item, Versus (vsmenu.h; --no-versus-menu leaves the
 *       menu as it was, and recordings and netplay always do). Special on the select screen goes
 *       back to the title menu, as it was.
 * --versus --host [PORT] hosts an online versus match (vsnet.h; up to 8 machines, each one player
 *       with the keyboard or a controller; guests take CPU slots as they join, any time);
 *       --versus --join ADDR[:PORT] joins one. Pause, rewind, states and fast forward are off. */
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "coop.h"
#include "crt.h"
#include "osd.h"
#include "mod.h"
#include "net.h"
#include "png.h"
#include "rom.h"
#include "spg.h"
#include "state.h"
#include "versus.h"
#include "vsmenu.h"
#include "vsnet.h"
#include "vssound.h"

#define OUT_SCALE 4   /* filter output is 1280x960; SDL scales that to the window */
#define FF_FRAMES 4   /* emulated frames per displayed frame while fast-forwarding */
#define OSD_FRAMES 120

enum {
    BTN_UP = 0x8000, BTN_DOWN = 0x4000, BTN_LEFT = 0x2000, BTN_RIGHT = 0x1000,
    BTN_A = 0x0800, BTN_MENU = 0x0400, BTN_B = 0x0200, BTN_X = 0x0100,
};

static SDL_AudioDeviceID audio_dev;
static int16_t audio_buf[4096 * 2];
static int audio_len;
static int audio_mute; /* fast forward: generate but drop */
static int volume = 100, muted; /* percent; the gain is (volume/100)^2, closer to how loudness is heard */
static int gain = 256;          /* 8.8 fixed point */
static int saved_filter = -1;   /* from the config file */

static void set_gain(void)
{
    gain = muted ? 0 : (volume * volume * 256 + 5000) / 10000;
}

static int16_t scaled(int16_t v)
{
    return (int16_t)((v * gain) >> 8); /* gain <= 256, so no overflow */
}

static void audio_sample(void *ctx, int16_t l, int16_t r)
{
    (void)ctx;
    audio_buf[audio_len * 2] = scaled(l);
    audio_buf[audio_len * 2 + 1] = scaled(r);
    if (++audio_len == 4096) {
        if (audio_dev && !audio_mute) SDL_QueueAudio(audio_dev, audio_buf, sizeof(audio_buf));
        audio_len = 0;
    }
}

static void audio_flush(void)
{
    if (audio_dev && audio_len && !audio_mute) SDL_QueueAudio(audio_dev, audio_buf, (Uint32)(audio_len * 4));
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

static char osd_msg[96];
static int osd_left;

static void osd(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_msg, sizeof(osd_msg), fmt, ap);
    va_end(ap);
    osd_left = OSD_FRAMES;
}

/* a recording only replays if the run was uninterrupted, so it ends at a state load or rewind */
static void stop_recording(FILE **rec, const char *path, uint32_t frame)
{
    fprintf(*rec, "# end at vblank %u: tests/check.sh <workdir> %u with INPUT=%s\n", frame, frame, path);
    fclose(*rec);
    *rec = NULL;
    fprintf(stderr, "recorded %u vblanks to %s\n", frame, path);
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

static uint16_t pad_buttons(SDL_GameController *pad)
{
    uint16_t b = 0;
    if (!pad) return 0;
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
    return b;
}

/* keyboard: P1 arrows Z X C Enter; P2 (co-op) W A S D, J K L, P */
static uint16_t key_buttons(int player)
{
    static const SDL_Scancode map[2][8] = {
        {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
         SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_RETURN},
        {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D,
         SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_P},
    };
    static const uint16_t bits[8] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_MENU};
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    for (int i = 0; i < 8; i++)
        if (k[map[player][i]]) b |= bits[i];
    return b;
}

/* versus: a controller in the Smash-like layout (versus.h buttons) */
static uint16_t vs_pad_buttons(SDL_GameController *pad)
{
    if (!pad) return 0;
    uint16_t b = pad_buttons(pad) & (VS_UP | VS_DOWN | VS_LEFT | VS_RIGHT | VS_MENU);
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) b |= VS_ATTACK;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B)) b |= VS_SPECIAL;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X) ||
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y)) b |= VS_JUMP;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ||
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
        SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
        SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= VS_BLOCK;
    return b;
}

/* versus: the keyboard, P1 or P2 (the co-op keys plus a block key) */
static uint16_t vs_key_buttons(int player)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = key_buttons(player);
    if (k[player ? SDL_SCANCODE_LSHIFT : SDL_SCANCODE_V]) b |= VS_BLOCK;
    return b;   /* key_buttons' A, B and X are jump, attack and special */
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int v;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "volume %d", &v) == 1 && v >= 0 && v <= 100) volume = v;
        else {
            char name[32];
            if (sscanf(line, "filter %31s", name) == 1) saved_filter = crt_mode_parse(name);
        }
    fclose(f);
}

static void save_config(const char *path, int filter)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "volume %d\nfilter %s\n", volume, crt_mode_name(filter));
    fclose(f);
}

/* netplay runs the frames (net.h): render and sound only when asked */
static void net_run(void *ctx, spg_t *m, int render, int audio)
{
    (void)ctx;
    m->render = render;
    audio_mute = !audio;
    spg_run_frame(m, NULL, NULL);
    mods_frame(m);
    audio_flush();
    audio_mute = 0;
    m->render = 1;
}

/* versus: a frame of the game in the background (its sound driver), with the new frame's sounds */
static void vs_audio(spg_t *m, const versus_t *v, uint32_t *played)
{
    if (v->frame > *played) {
        for (int k = 0; k < v->snd_n; k++) vssound_play((uint16_t)v->snd[k]);
        *played = v->frame;
    }
    m->buttons = 0;
    m->coop.buttons2 = 0;
    spg_run_frame(m, NULL, NULL);
    audio_flush();
}

/* the next free <base>-NNNN.png */
static int screenshot(const char *base, const uint32_t *px, int w, int h, char *path, size_t pathlen)
{
    struct stat st;
    for (int i = 0; i < 10000; i++) {
        snprintf(path, pathlen, "%s-%04d.png", base, i);
        if (stat(path, &st)) return png_write_xrgb(path, px, w, h);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL;
    char save_path[1024] = "";
    int scale = 3, filter = -1, volume_arg = -1, coop = 0, host_port = 0;
    const char *join = NULL;
    net_opts_t net_o = { 2, 0, 0, 0 };
    long max_frames = -1;
    const char *record_path = NULL, *mod_list = NULL, *mods_dir = "mods";
    int versus = 0, vs_players = 4, vs_stocks = 3, vs_kb2 = 0, vs_quick = 0, vs_menu = 1, vs_stage = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(save_path, sizeof(save_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) record_path = argv[++i];
        else if (!strcmp(argv[i], "--mods") && i + 1 < argc) mod_list = argv[++i];
        else if (!strcmp(argv[i], "--mods-dir") && i + 1 < argc) mods_dir = argv[++i];
        else if (!strcmp(argv[i], "--coop")) coop = 1;
        else if (!strcmp(argv[i], "--versus")) versus = 1;
        else if (!strcmp(argv[i], "--players") && i + 1 < argc) vs_players = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stocks") && i + 1 < argc) vs_stocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--kb2")) vs_kb2 = 1;
        else if (!strcmp(argv[i], "--quick")) vs_quick = 1;
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) vs_stage = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-versus-menu")) vs_menu = 0;
        else if (!strcmp(argv[i], "--host")) {
            host_port = NET_PORT;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') host_port = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--net-delay") && i + 1 < argc) net_o.delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) {
            volume_arg = atoi(argv[++i]);
            if (volume_arg < 0 || volume_arg > 100) { fprintf(stderr, "--volume takes 0-100\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
            if ((filter = crt_mode_parse(argv[++i])) < 0) {
                fprintf(stderr, "unknown filter %s (none, scanlines, aperture, composite, crt)\n", argv[i]);
                return 2;
            }
        }
        else if (argv[i][0] != '-') rom_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!rom_path) {
        fprintf(stderr, "usage: %s <batman.bin> [--save file.sav] [--scale N] [--filter mode] [--frames N] [--record script.txt] [--mods a,b] [--mods-dir dir] [--volume 0-100] [--coop] [--host [port] | --join addr[:port]] [--net-delay N] [--versus [--players N] [--stocks N] [--stage N] [--kb2] [--quick]] [--no-versus-menu]\n", argv[0]);
        return 2;
    }
    if (versus && (coop || record_path)) {
        fprintf(stderr, "--versus doesn't go with --coop or --record\n");
        return 2;
    }
    const int vs_online = versus && (host_port || join);   /* vsnet.h, not the co-op netplay */
    if ((host_port || join) && !vs_online) {
        coop = 1;
        if (record_path) { fprintf(stderr, "--record doesn't work with netplay\n"); return 2; }
    }
    FILE *rec = NULL;
    if (record_path) {
        rec = fopen(record_path, "w");
        if (!rec) { perror(record_path); return 1; }
        fprintf(rec, "# vblank hexmask%s (8000 up 4000 down 2000 left 1000 right 0800 A 0400 menu 0200 B 0100 X)\n"
                     "# recorded with recomp/build/batman --record%s, blank EEPROM\n",
                coop ? " [P2 hexmask]" : "", coop ? " --coop" : "");
        snprintf(save_path, sizeof(save_path), "%s.sav", record_path);
    } else if (!save_path[0]) {
        snprintf(save_path, sizeof(save_path), "%s", rom_path);
        char *dot = strrchr(save_path, '.');
        if (dot && !strchr(dot, '/')) *dot = 0;
        strncat(save_path, ".sav", sizeof(save_path) - strlen(save_path) - 1);
    }
    char base[1024]; /* save path without .sav: states and screenshots hang off it */
    snprintf(base, sizeof(base), "%s", save_path);
    {
        const size_t n = strlen(base);
        if (n > 4 && !strcmp(base + n - 4, ".sav")) base[n - 4] = 0;
    }
    char cfg_path[1100];
    snprintf(cfg_path, sizeof(cfg_path), "%s.cfg", base);
    load_config(cfg_path);
    if (volume_arg >= 0) volume = volume_arg;
    set_gain();
    if (filter < 0) filter = saved_filter >= 0 ? saved_filter : CRT_NONE;
    const int volume_at_start = volume, filter_at_start = filter;

    uint8_t file_sha[32];
    uint16_t *rom = rom_load(rom_path, file_sha);
    if (!rom) return 1;
    char enabled[4096];
    if (!mod_list && mods_enabled_list(mods_dir, enabled, sizeof(enabled))) mod_list = enabled;
    if (mod_list && !mods_load(mod_list, mods_dir, rom, file_sha)) return 1;
    if (rec && (mods_count() || coop)) fprintf(stderr, "warning: recording with mods or co-op; MAME will not reproduce it\n");
    static spg_t m;
    spg_init(&m, rom);
    if (coop) coop_attach(&m);
    /* Versus on the title menu: not in recordings (MAME has no such item) or online */
    if (record_path || host_port || join) vs_menu = 0;
    if (vs_menu) vsmenu_attach(&m);
    if (vs_menu || versus) vssound_attach(&m);   /* the versus mode's sound, through the game's driver */
    mods_attach(&m);
    FILE *sf = rec ? NULL : fopen(save_path, "rb");
    if (sf) {
        if (fread(m.eeprom.data, 1, EEPROM_SIZE, sf) != EEPROM_SIZE) fprintf(stderr, "%s: short save file\n", save_path);
        fclose(sf);
    }
    uint8_t rom_hash[32];
    mods_content_id(rom, rom_hash);
    if (vs_menu) vsmenu_patch(rom);   /* after the content id, so save states still load */
    int slot = 0;
    /* netplay. The host plays on its own while nobody is connected, and listens again when
     * player 2 leaves. The guest keeps trying to join until the host is there, and after the
     * host leaves; it stops only if the host refuses it (a different build or mod set). */
    netplay_t *net = NULL;
    int net_on = 0, net_over = 0;   /* playing with the peer; the host refused us */
    int net_retry = 0;              /* display frames until the next try */
    char net_wait[160] = "";        /* why the guest is still waiting */
    char join_addr[256] = "";
    int join_port = NET_PORT;
    if (join) {
        snprintf(join_addr, sizeof(join_addr), "%s", join);
        char *c = strrchr(join_addr, ':');
        if (join_addr[0] == '[') {   /* [v6]:port */
            char *e = strchr(join_addr, ']');
            if (e) {
                *e = 0;
                if (e[1] == ':') join_port = atoi(e + 2);
                memmove(join_addr, join_addr + 1, strlen(join_addr));
            }
        } else if (c && c == strchr(join_addr, ':')) { *c = 0; join_port = atoi(c + 1); }
    }
    if (host_port && !vs_online && !(net = net_host(host_port, &net_o))) return 1;
    vsnet_t *vnet = NULL;
    int vnet_on = 0, vnet_over = 0, vnet_retry = 0;
    char vnet_wait[160] = "";
    if (host_port && vs_online && !(vnet = vsnet_host(host_port, &net_o, rom_hash))) return 1;
    spg_reset(&m);
    mods_reset(&m);
    m.render = 1;
    m.spu.out = audio_sample;

    crt_t *crt = crt_new(OUT_SCALE);
    const int ow = crt_width(crt), oh = crt_height(crt);
    uint32_t *outbuf = malloc(sizeof(uint32_t) * ow * oh);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("The Batman", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       PPU_W * scale, PPU_H * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, ow, oh);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, ow, oh);
    /* the filtered image is already 4x: smooth scaling to the window keeps the scanlines even */
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = SPU_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* SDL resamples to the device */
    if (!audio_dev) fprintf(stderr, "no audio: %s\n", SDL_GetError());
    else SDL_PauseAudioDevice(audio_dev, 0);

    /* controllers in the order they were found. Single player: the first one. Co-op: with two,
     * one each; with one, it is P2's (P1 has the keyboard's arrows). */
    SDL_GameController *pads[2] = {NULL, NULL};
    for (int i = 0; i < SDL_NumJoysticks() && !pads[1]; i++)
        if (SDL_IsGameController(i)) pads[pads[0] ? 1 : 0] = SDL_GameControllerOpen(i);

    /* versus: slot i's controller; slot 0 also has the keyboard. A slot without a human is a bot. */
    static versus_t vs;
    SDL_GameController *vpads[VS_MAX] = {NULL};
    int vs_paused = 0, vs_start = versus, vs_from_title = 0;
    uint32_t vs_snd_frame = 0;   /* the last versus frame whose sounds were played */
    static spg_t vsv;            /* versus draws here: m keeps running the game, for the sound */
    spg_init(&vsv, rom);
    spg_reset(&vsv);
    static spg_snap_t title_snap;   /* the game at its title menu, while a versus match runs */
    /* versus rewind: every frame, VS_REWIND deep (60 s) */
    enum { VS_REWIND = 3600 };
    static versus_t vs_rewind[VS_REWIND];
    int vs_rewind_head = 0, vs_rewind_len = 0;
    versus = 0;

    const Uint64 freq = SDL_GetPerformanceFrequency();
    const double frame_ticks = (double)freq / 60.0;
    double next = (double)SDL_GetPerformanceCounter();
    int running = 1, shot = 0;
    while (running && (max_frames < 0 || (long)m.frame < max_frames)) {
        if (vs_start) {   /* from --versus, or the title menu's Versus */
            vs_start = 0;
            versus = 1;
            vs_rewind_len = 0;
            vs_paused = 0;
            if (vs_players < 2) vs_players = 2;
            if (vs_players > VS_MAX) vs_players = VS_MAX;
            int next_slot = vs_kb2 ? 2 : 1;
            vpads[0] = pads[0];
            if (pads[1] && next_slot < vs_players) vpads[next_slot++] = pads[1];
            for (int i = 0; i < SDL_NumJoysticks() && next_slot < vs_players; i++) {
                if (!SDL_IsGameController(i)) continue;
                SDL_GameController *p = SDL_GameControllerOpen(i);
                int have = 0;
                for (int k = 0; k < VS_MAX; k++) have |= vpads[k] == p;
                if (p && !have) vpads[next_slot++] = p;
            }
            uint32_t bots = 0;
            for (int i = 1; i < vs_players; i++)
                if (!vpads[i] && !(i == 1 && vs_kb2)) bots |= 1u << i;
            if (vs_online) bots = 0xFE;   /* guests take CPU slots as they join */
            if (!vs_from_title) {   /* the game in the background, at its title menu, plays the sound */
                vssound_boot(&m);
                vssound_play(VS_MUSIC_SELECT);
            }
            vs_snd_frame = 0;
            versus_init(&vs, m.rom, vs_players, bots, vs_stocks, vs_online ? 1 : (uint32_t)SDL_GetPerformanceCounter(),
                        vs_from_title || !vs_quick);
            if (vs_stage) versus_set_stage(&vs, vs_stage);
            SDL_SetWindowTitle(win, "The Batman: Versus");
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                char path[1100];
                snprintf(path, sizeof(path), "%s.st%d", base, slot);
                if (vs_online && (e.key.keysym.sym == SDLK_F5 || e.key.keysym.sym == SDLK_F8)) {
                    osd("Not while playing online");
                    continue;
                }
                if (versus && (e.key.keysym.sym == SDLK_F5 || e.key.keysym.sym == SDLK_F8)) {
                    /* a match's state is its versus_t: <base>.vs<slot> */
                    snprintf(path, sizeof(path), "%s.vs%d", base, slot);
                    if (e.key.keysym.sym == SDLK_F5) osd(versus_save(&vs, path) ? "Saved match %d" : "Save FAILED", slot);
                    else if (versus_load(&vs, path)) { osd("Loaded match %d", slot); vs_rewind_len = 0; }
                    else osd("No match in slot %d", slot);
                    continue;
                }
                if ((net || join) && (e.key.keysym.sym == SDLK_F8 || (join && e.key.keysym.sym == SDLK_F5))) {
                    osd("Not while playing online");
                    continue;
                }
                switch (e.key.keysym.sym) {
                case SDLK_F5:
                    if (spg_state_save(&m, rom_hash, path)) osd("Saved state %d", slot);
                    else osd("Save FAILED");
                    break;
                case SDLK_F8: {
                    const uint32_t was = m.frame;
                    if (spg_state_load(&m, rom_hash, path)) {
                        osd("Loaded state %d", slot);
                        if (rec) stop_recording(&rec, record_path, was);
                        if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
                        next = (double)SDL_GetPerformanceCounter();
                    } else osd("No usable state in slot %d", slot);
                    break;
                }
                case SDLK_F6: slot = (slot + 9) % 10; osd("Slot %d", slot); break;
                case SDLK_F7: slot = (slot + 1) % 10; osd("Slot %d", slot); break;
                case SDLK_F9:
                    filter = (filter + ((e.key.keysym.mod & KMOD_SHIFT) ? CRT_MODES - 1 : 1)) % CRT_MODES;
                    osd("Filter: %s", crt_mode_name(filter));
                    break;
                case SDLK_F11:
                    SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)
                                                     ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                case SDLK_F12: shot = 1; break;
                case SDLK_MINUS: case SDLK_KP_MINUS:
                case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: {
                    const int down = e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_KP_MINUS;
                    volume = down ? (volume > 10 ? volume - 10 : 0) : (volume < 90 ? volume + 10 : 100);
                    muted = 0;
                    set_gain();
                    osd("Volume %d%%", volume);
                    break;
                }
                case SDLK_m:
                    muted = !muted;
                    set_gain();
                    if (muted) osd("Muted");
                    else osd("Volume %d%%", volume);
                    break;
                default: break;
                }
            }
            else if (versus && e.type == SDL_CONTROLLERDEVICEADDED) {
                /* takes the first CPU slot (or slot 1's pad if P1 has none) */
                SDL_GameController *p = SDL_GameControllerOpen(e.cdevice.which);
                int have = 0;
                for (int k = 0; k < VS_MAX; k++) have |= vpads[k] == p;
                if (p && !have) {
                    if (!vpads[0]) vpads[0] = p;
                    else for (int k = 1; k < vs.n; k++)
                        if (vs.f[k].bot) {
                            vpads[k] = p;
                            vs.f[k].bot = 0;
                            osd("P%d joined", k + 1);
                            break;
                        }
                }
            }
            else if (versus && e.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (int k = 0; k < VS_MAX; k++)
                    if (vpads[k] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(vpads[k])) == e.cdevice.which) {
                        SDL_GameControllerClose(vpads[k]);
                        vpads[k] = NULL;
                        if (k && !(k == 1 && vs_kb2) && k < vs.n) {
                            vs.f[k].bot = 1;
                            osd("P%d is a CPU now", k + 1);
                        }
                    }
            }
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pads[1]) {
                SDL_GameController *p = SDL_GameControllerOpen(e.cdevice.which);
                if (p && p != pads[0]) pads[pads[0] ? 1 : 0] = p;
            }
        }

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        const int ff = keys[SDL_SCANCODE_TAB] && !net;
        if (!vs_online && !net && !net_over && (host_port || join) && net_retry-- <= 0) {
            net = host_port ? net_host(host_port, &net_o) : net_join(join_addr, join_port, &net_o, rom_hash);
            net_retry = 120;
        }
        if (net && !net_on) {
            const int r = join ? net_wait_start(net, &m) : net_accept(net, &m, rom_hash);
            if (r > 0) {
                net_on = 1;
                net_wait[0] = 0;
            } else if (r < 0) {
                snprintf(net_wait, sizeof(net_wait), "%s", net_reason(net));
                net_over = net_refused(net);
                net_close(net);
    vsnet_close(vnet);
                net = NULL;
            }
        }
        if (net && net_on) {
            const uint16_t b = key_buttons(0) | pad_buttons(pads[0]) | pad_buttons(pads[1]);
            const int r = net_tick(net, &m, b, 0, net_run, NULL);
            for (const char *ev; (ev = net_event(net));) osd("%s", ev);
            if (r < 0) {   /* the host goes on alone and listens again; the guest waits again */
                snprintf(net_wait, sizeof(net_wait), "%s", net_reason(net));
                net_close(net);
                net = NULL;
                net_on = 0;
                net_retry = 60;
            }
            if (host_port && m.frame % 300 == 0) save_eeprom(&m, save_path);
        }
        for (const char *ev; net && (ev = net_event(net));) osd("%s", ev);
        if (versus && vs_online) {
            /* online (vsnet.h): this machine plays one slot, with the keyboard or the first pad */
            if (join && !vnet && !vnet_over && vnet_retry-- <= 0) {
                vnet = vsnet_join(join_addr, join_port, &net_o, rom_hash);
                vnet_retry = 120;
            }
            if (vnet && join && !vnet_on) {
                const int r = vsnet_wait_start(vnet, &vs);
                if (r > 0) { vnet_on = 1; vnet_wait[0] = 0; }
                else if (r < 0) {
                    snprintf(vnet_wait, sizeof(vnet_wait), "%s", vsnet_reason(vnet));
                    vnet_over = vsnet_refused(vnet);
                    vsnet_close(vnet);
                    vnet = NULL;
                }
            }
            if (vnet && (host_port || vnet_on)) {
                const uint16_t local = vs_key_buttons(0) | vs_pad_buttons(vpads[0]) | vs_pad_buttons(pads[1]);
                const int r = vsnet_tick(vnet, &vs, m.rom, local, VSNET_NO_END);
                for (const char *ev; (ev = vsnet_event(vnet));) osd("%s", ev);
                if (r < 0) {   /* the host left: wait for it again */
                    snprintf(vnet_wait, sizeof(vnet_wait), "%s", vsnet_reason(vnet));
                    vsnet_close(vnet);
                    vnet = NULL;
                    vnet_on = 0;
                    vnet_retry = 60;
                }
            }
            vs_audio(&m, &vs, &vs_snd_frame);
            versus_present(&vs, &vsv);
            if (vnet && (host_port || vnet_on)) {
                char line[200];
                snprintf(line, sizeof(line), "%s  (you: P%d)", vsnet_status(vnet), vsnet_slot(vnet) + 1);
                osd_text(vsv.framebuffer, PPU_W, PPU_H, 4, PPU_H - 12, 1, line);
            } else if (join) {
                char line[320];
                if (vnet_over) snprintf(line, sizeof(line), "Can't join %s", join_addr);
                else snprintf(line, sizeof(line), "Waiting for the match at %s...", join_addr);
                osd_text(vsv.framebuffer, PPU_W, PPU_H, 8, 100, 1, line);
                if (vnet_wait[0]) osd_text(vsv.framebuffer, PPU_W, PPU_H, 8, 114, 1, vnet_wait);
            }
        } else if (versus) {
            uint16_t b[VS_MAX] = {0};
            for (int k = 0; k < vs.n; k++) b[k] = vs_pad_buttons(vpads[k]);
            b[0] |= vs_key_buttons(0);
            if (vs_kb2) b[1] |= vs_key_buttons(1);
            /* Menu pauses the fight; on the result screen it plays again (versus.c) */
            static uint16_t menu_prev;
            uint16_t menu = 0;
            for (int k = 0; k < vs.n; k++) if (!vs.f[k].bot && (b[k] & VS_MENU)) menu |= 1u << k;
            if (vs.phase == VS_FIGHT && (menu & ~menu_prev)) vs_paused = !vs_paused;
            menu_prev = menu;
            /* paused: Special leaves the match for the select screen */
            static uint16_t special_prev;
            uint16_t special = 0;
            for (int k = 0; k < vs.n; k++) if (!vs.f[k].bot && (b[k] & VS_SPECIAL)) special |= 1u << k;
            if (vs_paused && (special & ~special_prev)) {
                versus_rematch(&vs, m.rom, 1);
                vs_paused = 0;
            }
            special_prev = special;
            if (keys[SDL_SCANCODE_BACKSPACE]) {   /* rewind, a frame per displayed frame */
                if (vs_rewind_len) {
                    vs_rewind_head = (vs_rewind_head + VS_REWIND - 1) % VS_REWIND;
                    vs_rewind_len--;
                    vs = vs_rewind[vs_rewind_head];
                }
            } else if (!vs_paused)
                for (int i = 0; i < (ff ? FF_FRAMES : 1); i++) {
                    vs_rewind[vs_rewind_head] = vs;
                    vs_rewind_head = (vs_rewind_head + 1) % VS_REWIND;
                    if (vs_rewind_len < VS_REWIND) vs_rewind_len++;
                    versus_step(&vs, m.rom, b);
                }
            if (vs.quit) {   /* backed out of the select screen: to the title menu, if it came from there */
                vs.quit = 0;
                if (vs_from_title) {
                    spg_restore(&m, &title_snap);
                    versus = vs_from_title = 0;
                    SDL_SetWindowTitle(win, "The Batman");
                    if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
                }
            }
            if (versus) {
                if (!vs_paused) vs_audio(&m, &vs, &vs_snd_frame);
                versus_present(&vs, &vsv);
                if (vs_paused) {
                    osd_text(vsv.framebuffer, PPU_W, PPU_H, 112, 96, 3, "PAUSE");
                    osd_text(vsv.framebuffer, PPU_W, PPU_H, 84, 128, 1, "MENU: GO ON   SPECIAL: QUIT");
                }
            }
        } else if (join || (net && net_on)) {
            /* netplay drives the frames (above), or the guest waits or has stopped */
        } else if (keys[SDL_SCANCODE_BACKSPACE]) {
            /* rewind: step back one snapshot per displayed frame (REWIND_EVERY x speed), silently */
            const uint32_t was = m.frame;
            if (rewind_pop(&m)) {
                if (rec) stop_recording(&rec, record_path, was);
                if (audio_dev) SDL_ClearQueuedAudio(audio_dev);
            }
        } else {
            audio_mute = ff;
            for (int i = 0; i < (ff ? FF_FRAMES : 1) && running; i++) {
                /* the buttons set here are what the CPU sees from vblank m.frame on, which is
                 * what an input-script line "<m.frame> <mask>" means to batman-rt and capture.lua.
                 * Nothing is pressed before vblank 1: neither harness applies input at vblank 0. */
                uint16_t b = 0, b2 = 0;
                if (m.frame) {
                    SDL_GameController *p1 = coop && !pads[1] ? NULL : pads[0];
                    SDL_GameController *p2 = coop ? (pads[1] ? pads[1] : pads[0]) : NULL;
                    b = key_buttons(0) | pad_buttons(p1);
                    if (coop) b2 = key_buttons(1) | pad_buttons(p2);
                }
                if (rec && (b != m.buttons || b2 != m.coop.buttons2)) {
                    if (coop) fprintf(rec, "%u %04x %04x\n", m.frame, b, b2);
                    else fprintf(rec, "%u %04x\n", m.frame, b);
                }
                m.buttons = b;
                m.coop.buttons2 = b2;
                if (m.frame % REWIND_EVERY == 0) rewind_push(&m);
                if (!spg_run_frame(&m, NULL, NULL)) {
                    fprintf(stderr, "CPU stopped at %06x\n", unsp_lpc(&m.cpu));
                    running = 0;
                }
                mods_frame(&m);
                audio_flush();
                if (m.frame % 300 == 0) save_eeprom(&m, save_path);
                if (m.versus_request) {   /* Versus on the title menu */
                    m.versus_request = 0;
                    spg_snapshot(&m, &title_snap);
                    vs_start = vs_from_title = 1;
                    if (rec) stop_recording(&rec, record_path, m.frame);
                    break;
                }
            }
            if (!running) break;
        }

        crt_apply(crt, filter, versus ? vsv.framebuffer : m.framebuffer, outbuf);
        if (shot) {
            char path[1100];
            if (screenshot(base, outbuf, ow, oh, path, sizeof(path))) {
                fprintf(stderr, "screenshot %s\n", path);
                osd("Screenshot %.60s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
            } else osd("Screenshot FAILED");
            shot = 0;
        }
        if (ff) osd_text(outbuf, ow, oh, ow - 16 * 6 * 3 - 24, 24, 3, ">> fast forward");
        if (net && net_on) osd_text(outbuf, ow, oh, 16, oh - 16 - 7 * 2, 2, net_status(net));
        else if (join) {
            char line[320];
            if (net_over) snprintf(line, sizeof(line), "Can't join %s", join_addr);
            else snprintf(line, sizeof(line), "Waiting for the game at %s...", join_addr);
            osd_text(outbuf, ow, oh, 24, oh / 2 - 30, 3, line);
            if (net_wait[0]) osd_text(outbuf, ow, oh, 24, oh / 2 + 10, 2, net_wait);
            if (net_over) osd_text(outbuf, ow, oh, 24, oh / 2 + 40, 2, "Esc quits");
        } else if (host_port) {
            char line[96];
            snprintf(line, sizeof(line), "Online: waiting for player 2 (port %d)", host_port);
            osd_text(outbuf, ow, oh, 16, oh - 16 - 7 * 2, 2, line);
        }
        if (keys[SDL_SCANCODE_BACKSPACE]) osd_text(outbuf, ow, oh, ow - 16 * 6 * 3 - 24, 24, 3, "<< rewind");
        if (osd_left > 0) {
            osd_text(outbuf, ow, oh, 24, 24, 3, osd_msg);
            osd_left--;
        }
        SDL_UpdateTexture(tex, NULL, outbuf, ow * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        /* pace to 60 Hz; let the audio queue absorb jitter but never grow past ~4 frames */
        next += frame_ticks;
        const double now = (double)SDL_GetPerformanceCounter();
        if (next > now) SDL_Delay((Uint32)((next - now) * 1000.0 / (double)freq));
        else if (now - next > frame_ticks * 5) next = now; /* fell behind: resync */
        if (audio_dev && SDL_GetQueuedAudioSize(audio_dev) > (Uint32)(SPU_RATE / 60 * 4 * 4)) SDL_Delay(4);
    }

    if (!join) save_eeprom(&m, save_path);   /* the guest's EEPROM was the host's */
    net_close(net);
    if (rec) stop_recording(&rec, record_path, m.frame);
    for (int i = 0; i < 2; i++)
        if (pads[i]) SDL_GameControllerClose(pads[i]);
    for (int i = 0; i < VS_MAX; i++)
        if (vpads[i] && vpads[i] != pads[0] && vpads[i] != pads[1]) SDL_GameControllerClose(vpads[i]);
    if (volume != volume_at_start || filter != filter_at_start) save_config(cfg_path, filter);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    crt_free(crt);
    free(outbuf);
    return 0;
}
