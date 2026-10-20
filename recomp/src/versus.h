/* Versus: a Smash-style fighting mode on the game's own art (notes/versus.md).
 *
 * The match is native code: the game's CPU does not run. The ROM supplies what is drawn (the
 * characters' sprite frames, their hit and hurt boxes and palettes, a scene's tilemaps) and the
 * frames go through the PPU port like the game's own. All of the state is plain data in
 * versus_t, stepped once per frame from the players' buttons, so a match with the same inputs
 * is always the same match (and can be snapshotted for rollback). */
#ifndef VERSUS_H
#define VERSUS_H

#include <stdint.h>

#define VS_MAX 8

/* buttons: the game's port A layout (the front ends' BTN_*), plus block */
enum {
    VS_UP = 0x8000, VS_DOWN = 0x4000, VS_LEFT = 0x2000, VS_RIGHT = 0x1000,
    VS_JUMP = 0x0800, VS_MENU = 0x0400, VS_ATTACK = 0x0200, VS_SPECIAL = 0x0100,
    VS_BLOCK = 0x0080,   /* shield; with a direction a roll or a spot dodge; in the air an air dodge */
};

enum { VS_COUNTDOWN, VS_FIGHT, VS_GAME_OVER, VS_SELECT };

typedef struct vs_fighter {
    int32_t ch;              /* character (versus.c), -1: empty slot */
    int32_t bot;
    int32_t x, y;            /* the feet, 1/256 px, screen coordinates (y down) */
    int32_t vx, vy;
    int32_t facing;          /* 1 right, -1 left */
    int32_t state;           /* VS_ST_* in versus.c */
    int32_t t;               /* ticks in the state */
    int32_t move;            /* the attack being done */
    int32_t buffered;        /* attack pressed during an attack: the next hit of the combo */
    int32_t anim_first, anim_last, anim_ticks, anim_loop; /* the animation playing */
    int32_t frame, frame_t;  /* its current ROM frame and ticks into it */
    int32_t damage;          /* percent */
    int32_t stocks;
    int32_t hitlag, hitstun, invuln;
    int32_t jumps;           /* air jumps left */
    int32_t grounded;        /* index + 1 of the platform stood on, 0 in the air */
    int32_t drop;            /* ticks of falling through pass-through platforms */
    int32_t hit_mask;        /* fighters the current attack already hit */
    int32_t last_hit_by;
    int32_t kos, falls;
    uint32_t prev;           /* buttons last tick */
    uint32_t bot_t;          /* bot: ticks until it rethinks, and what it holds meanwhile */
    uint32_t bot_hold;
    int32_t ready;           /* character select: this human has picked */
    int32_t shield;          /* shield health, 1/256 points (versus.c SHIELD_MAX when full) */
    int32_t shield_stun;     /* ticks the shield is held by the last hit on it */
    int32_t dodge;           /* the dodge under way: 0 none, 1 spot, 2 roll, 3 in the air */
    int32_t intangible;      /* ticks attacks go through (dodges) */
    int32_t air_dodged;      /* used its air dodge since it last landed */
    int32_t helpless;        /* after a recovery move: nothing but drifting until it lands */
    int32_t spawned;         /* the current move has thrown its projectile */
    int32_t air_move;        /* the current move started in the air (landing ends it) */
    int32_t squat;           /* ticks of jumpsquat left before a ground jump leaves the ground */
    int32_t ledge;           /* hanging from this ledge: platform * 2 + side + 1 */
    int32_t ledge_cd;        /* ticks before it can catch a ledge again */
    int32_t ledge_used;      /* caught one since it last landed (no second safe catch) */
    int32_t grapple_x, grapple_y;   /* the grapnel's anchor, px, while ST_GRAPPLE */
    int32_t ice_used;        /* Mr. Freeze: made his ice since he last stood on the stage */
} vs_fighter_t;

#define VS_ICE 4
typedef struct vs_ice {      /* Mr. Freeze's ice platform */
    int32_t t;               /* 0: none; ticks since it was made */
    int32_t owner, x, top;   /* its middle and its top, px */
    int32_t hit_mask;
} vs_ice_t;

#define VS_PROJ 16
typedef struct vs_proj {
    int32_t kind;            /* 0: free, else versus.c's PROJS index + 1 */
    int32_t owner, facing;
    int32_t x, y, vx, vy;    /* 1/256 px */
    int32_t t, hit_mask;
} vs_proj_t;

typedef struct versus {
    uint32_t magic, size;    /* identify a saved state */
    uint32_t frame;
    uint32_t rng;
    int32_t n;               /* fighters */
    int32_t phase, phase_t;
    int32_t winner;
    int32_t stocks;          /* each fighter's at the start */
    int32_t matches;         /* matches finished since versus_init (a rematch keeps counting) */
    int32_t select;          /* start with (and come back to) the character select */
    uint32_t bot_mask;       /* the CPU slots, also for slots added on the select screen */
    int32_t quit;            /* a human backed out of the select screen (the front end decides) */
    vs_fighter_t f[VS_MAX];
    vs_proj_t proj[VS_PROJ];
    vs_ice_t ice[VS_ICE];
    int32_t stage;
    int32_t cam_x, cam_y;    /* the camera's top left, 1/256 px of the stage's map */
} versus_t;

/* a new match: n fighters (characters assigned in turn from the roster), bot_mask bit i = slot i
 * is a CPU. With `select` it opens on the character select screen (and rematches go back to it):
 * each human picks with Left/Right and confirms with Attack or Jump (Special takes it back), P1's
 * Up/Down add or remove CPU fighters, and the countdown starts when every human is ready.
 * Special from a human who hasn't picked sets v->quit. */
void versus_init(versus_t *v, const uint16_t *rom, int n, uint32_t bot_mask, int stocks, uint32_t seed,
                 int select);
/* the characters this ROM offers (Robin only with mods/robin-p2), as versus_char_name indices */
int versus_roster(const uint16_t *rom, int *out);
/* a new match with the same fighters and characters, on the select screen or straight to the
 * countdown */
void versus_rematch(versus_t *v, const uint16_t *rom, int select);
/* netplay: makes a CPU slot (the first after slot 0) a human's, or on the select screen adds a
 * fighter for it; returns the slot, -1 when there is none. versus_release_slot gives it back to
 * a CPU. */
int versus_take_slot(versus_t *v, const uint16_t *rom);
void versus_release_slot(versus_t *v, int slot);
/* the stages: versus_set_stage puts everyone back on their spots there */
void versus_set_stage(versus_t *v, int stage);
int versus_stage_count(void);
const char *versus_stage_name(int stage);
/* gives slot i another character (before the fight starts) */
void versus_set_char(versus_t *v, int i, int ch);
/* one frame; buttons[i] for slot i (ignored for bots). The ROM supplies the frame boxes. */
void versus_step(versus_t *v, const uint16_t *rom, const uint16_t *buttons);

struct spg;
/* draws the match into m->framebuffer: video registers, stage, fighters, then the HUD */
void versus_present(const versus_t *v, struct spg *m);

uint64_t versus_hash(const versus_t *v);
int versus_save(const versus_t *v, const char *path);
int versus_load(versus_t *v, const char *path);
const char *versus_char_name(int ch);

#endif
