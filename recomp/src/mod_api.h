/* The interface a native mod plugin (mods/<name>/plugin.so) is built against.
 *
 * A plugin exports
 *     int batman_mod_init(const batman_api_t *api, void **user);
 * which returns 0 on success. It is called once, after the ROM is patched and before the
 * machine is reset. Everything the plugin may do goes through `api`.
 *
 * Addresses are µ'nSP word addresses (segment << 16 | offset). A PC hook runs before the
 * instruction at its address executes; it returns 0 to let that instruction run, or 1
 * when it has changed PC itself (for example by replacing a whole function and calling
 * api->retf). A hook that returns 1 counts as one instruction: pending interrupts are taken
 * right after it. A hook that returns 1 without moving PC is called again forever. */
#ifndef BATMAN_MOD_API_H
#define BATMAN_MOD_API_H

#include <stdint.h>

#define BATMAN_MOD_API_VERSION 1

typedef struct spg spg_t;

enum { BATMAN_SP, BATMAN_R1, BATMAN_R2, BATMAN_R3, BATMAN_R4, BATMAN_BP, BATMAN_SR, BATMAN_PC };

typedef int (*batman_hook_fn)(spg_t *m, void *user);
typedef void (*batman_frame_fn)(spg_t *m, void *user);

typedef struct batman_api {
    int version;           /* BATMAN_MOD_API_VERSION */
    const char *mod_dir;   /* the plugin's mods/<name> directory */

    /* memory: peek has no side effects (RAM, video RAM, ROM); poke writes RAM or video RAM;
     * bus_write goes through the CPU bus, so I/O registers react as to a game write */
    uint16_t (*peek)(spg_t *m, uint32_t addr);
    void (*poke)(spg_t *m, uint32_t addr, uint16_t value);
    void (*bus_write)(spg_t *m, uint32_t addr, uint16_t value);

    /* CPU registers SP, R1-R4, BP, SR, PC (see the enum); the code segment is SR & 0x3f */
    uint16_t *(*regs)(spg_t *m);
    uint32_t (*pc)(spg_t *m);
    /* returns from the current far call as RETF does: pops SR then PC (charges 8 cycles) */
    void (*retf)(spg_t *m);

    void (*hook)(uint32_t addr, batman_hook_fn fn, void *user);
    /* called at every vblank, after the frame has been rendered */
    void (*on_frame)(batman_frame_fn fn, void *user);

    uint32_t (*frame)(spg_t *m);       /* vblanks since reset */
    uint32_t *(*framebuffer)(spg_t *m); /* 320x240 XRGB8888, drawable from on_frame */
    /* draws text (5x7 font on a dark box) into the framebuffer, from on_frame */
    void (*text)(spg_t *m, int x, int y, const char *s);
    void (*log)(const char *fmt, ...);
} batman_api_t;

int batman_mod_init(const batman_api_t *api, void **user);

#endif
