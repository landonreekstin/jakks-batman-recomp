/* Versus mode (versus.h, notes/versus.md).
 *
 * Art. Every character is an entry of the game's sprite bank at 0x051C40 (notes/sprites.md,
 * tools/sprites.py): the bank's word 0 is a count, then one offset per character to its header
 * (frame count, palette ids, a default box), whose frame list starts at header + 0x0C. A frame
 * record is: piece count, two words, k, k words of boxes (x0, y0, x1, y1, type; the feet are the
 * origin, y down), three words, then the pieces (dx, dy, tile index), each relative to the one
 * before. Box types 1, 2 and 6 are the body, 3, 4, 5 and 0x14 attacks. Palette id N is the 16
 * words at 0x051840 + 16 N. All of the ROM's characters face right.
 *
 * Moves. Attacks hit with the boxes their ROM frames carry. Frames that have none (Bane's and
 * Firefly's: as bosses they hurt by other means) get a box here, out on the frames given.
 *
 * Stage. Scene 8 (one screen, 12x8 blocks of 32x32 px: a steel floor and a catwalk over it, with
 * the brick room of its page 1 behind) decoded into the tilemaps the way the game's own
 * FUN_00c333 does (notes/level-format.md), with the floor's outer two block columns taken out so
 * the stage has edges. The catwalk is a pass-through platform. The screen shows map x 32-351.
 *
 * Knockback. A hit adds its damage to the target's percent, then launches it at the move's angle
 * with speed bkb + kbg * percent / weight (Smash's shape, much simplified), in hitstun for a time
 * that grows with the speed. A fighter past a blast zone loses a stock. */
#include "versus.h"

#include <stdio.h>
#include <string.h>

#include "osd.h"
#include "spg.h"

#define BANK 0x051C40
#define HEADER_FRAMES 0x0C
#define PALETTES 0x051840
#define TILE_BASE 0x400          /* sprite tile number = piece index + 0x400 */
#define SCENES 0x0D869E          /* the scene table: a count, then 24-word records of far pointers */
#define SCENE_REC 0x18
#define TILE_POOL 0x09EC10
#define ATTR_POOL 0x0A6580

#define SUB 256                  /* positions and speeds are in 1/256 px */
#define KB_GROWTH 5              /* knockback: bkb + KB_GROWTH * kbg * percent / weight */
#define SOLID_DEPTH 48           /* a solid platform is a block this deep: its sides are walls */
#define RESPAWN_HIDDEN 60        /* ticks gone after a KO */
#define RESPAWN_HOVER 120        /* then on the respawn spot, until a button or this long */
#define INVULN 120
#define COUNTDOWN 150
#define GAME_OVER_WAIT 240
#define DROP_TICKS 12

enum { ST_FREE, ST_ATTACK, ST_HITSTUN, ST_RESPAWN, ST_OUT, ST_SHIELD, ST_DODGE, ST_DIZZY, ST_LEDGE, ST_GRAPPLE };

/* block (Smash's shield, much simplified), in 1/256 points and ticks */
#define SHIELD_MAX (50 * SUB)
#define SHIELD_DRAIN 38          /* a tick, while held: a full shield lasts about 5.5 s */
#define SHIELD_REGEN 20          /* a tick, while not held */
#define SHIELD_BREAK_STUN 180    /* dizzy after a break */
#define ROLL_V 896               /* 3.5 px a tick */
#define ROLL_TICKS 22
#define ROLL_MOVE 16             /* ticks of the roll that move */
#define SPOT_TICKS 20
#define AIR_DODGE_TICKS 26
#define INTANGIBLE_FROM 3        /* a dodge's invulnerable ticks start here and last: */
#define INTANGIBLE_FOR 13
#define GRAPNEL_FLY 6            /* ticks the grapnel takes to reach its anchor */
#define GRAPNEL_REACH 176        /* px from the hand */
#define GRAPNEL_PULL (7 * SUB)   /* pulled up the line this fast */
#define JUMPSQUAT 4              /* a ground jump leaves the ground this late; B or X meanwhile is an
                                  * up tilt or up special instead (Up is also jump) */
/* the moves: B alone, B + a direction on the ground (tilts) and in the air, X + a direction */
enum {
    MV_JAB1, MV_JAB2, MV_JAB3, MV_AIR, MV_FTILT, MV_UTILT, MV_DTILT, MV_UAIR, MV_DAIR,
    MV_NSPEC, MV_SSPEC, MV_USPEC, MV_DSPEC, MV_N
};

typedef struct anim {
    uint16_t first, last;   /* ROM frames */
    uint8_t ticks, loop;
} anim_t;

typedef struct move {
    anim_t a;                /* a.ticks 0: the character has no such move */
    int16_t box[4];          /* when the frames carry no attack box: x0, y0, x1, y1 facing right */
    uint16_t act0, act1;     /* the frames that box is out on */
    int16_t dmg, bkb, kbg, angle;   /* angle below 0: downwards (a meteor) */
    int16_t lunge;           /* forward speed when it starts, slowed by the ground */
    int8_t chain;            /* the move a second press continues into, + 1 (0: none) */
    int8_t proj;             /* a projectile it throws, + 1 (PROJS) */
    uint16_t proj_frame;     /* on this ROM frame */
    int16_t vx0, vy0;        /* speed for the whole move (forward), and upwards/downwards at its start */
    int8_t lowgrav;          /* a quarter of the gravity while it lasts (a glide) */
    int8_t helpless;         /* after it, in the air: no jumps or attacks until landing (a recovery) */
    int8_t grapple;          /* the grapnel: to a ledge in reach, if there is one (else the move as given) */
} move_t;

typedef struct character {
    const char *name;
    uint16_t header, pal;
    uint16_t strip_lo, strip_hi; /* tiles not drawn (Bane's frames include his hanging platform) */
    anim_t idle, walk, jump, djump, fall, hurt, tumble;
    move_t mv[MV_N];
    int16_t walk_v, air_v, jump_v, djump_v, gravity, fall_v, weight, air_jumps;
    int16_t body[4];         /* body box for frames without one */
    int16_t tall, reach;     /* for the HUD tag and the bots */
    uint32_t bank, palettes; /* 0: the game's sprite bank and palettes; Robin's are in the upper ROM copy */
} character_t;

/* projectiles: frames of a bank entry, cycled, with a box round the position */
typedef struct proj_def {
    uint16_t header, f0, f1, ticks;
    uint16_t pal;            /* its palette id (drawn in a bank the stage leaves free) */
    int16_t box[4];          /* x0, y0, x1, y1 facing right */
    int16_t vx, vy, grav, life;
    int16_t dmg, bkb, kbg, angle;
    int16_t dx, dy;          /* where it starts, from the thrower's feet, facing right */
    int8_t one;              /* one at a time per thrower */
    int8_t follow;           /* stays with the thrower (Firefly's flame column) */
} proj_def_t;

enum { PJ_BATARANG = 1, PJ_BULLET, PJ_GAS, PJ_BOMB, PJ_FLAME, PJ_COLUMN };
static const proj_def_t PROJS[] = {
    /* Batman's Batarang: the spin frames of bank entry 0x0BD5 (palette id 1) */
    { .header = 0x0BD5, .f0 = 0, .f1 = 13, .ticks = 2, .pal = 1, .box = { -7, -6, 7, 6 },
      .vx = 1280, .life = 44, .dmg = 4, .bkb = 250, .kbg = 40, .angle = 30, .dx = 16, .dy = -30, .one = 1 },
    /* the Joker's gun shot, frame 74 */
    { .header = 0xB682, .f0 = 74, .f1 = 74, .ticks = 8, .pal = 0x25, .box = { -10, -4, 8, 4 },
      .vx = 1792, .life = 40, .dmg = 3, .bkb = 150, .kbg = 20, .angle = 10, .dx = 26, .dy = -30 },
    /* the Joker's gas bomb, frame 73, thrown in an arc */
    { .header = 0xB682, .f0 = 73, .f1 = 73, .ticks = 8, .pal = 0x25, .box = { -8, -30, 8, 0 },
      .vx = 640, .vy = -1024, .grav = 48, .life = 120, .dmg = 9, .bkb = 450, .kbg = 100, .angle = 60,
      .dx = 12, .dy = -20, .one = 1 },
    /* the Penguin's bomb, frames 89-96, in an arc */
    { .header = 0x4D64, .f0 = 89, .f1 = 96, .ticks = 4, .pal = 0x0B, .box = { -8, -22, 10, 0 },
      .vx = 768, .vy = -896, .grav = 48, .life = 120, .dmg = 10, .bkb = 480, .kbg = 105, .angle = 55,
      .dx = 14, .dy = -12, .one = 1 },
    /* Firefly's flame burst, frames 27-32, straight ahead */
    { .header = 0x57EA, .f0 = 27, .f1 = 32, .ticks = 3, .pal = 0x0D, .box = { -14, -14, 14, 12 },
      .vx = 1024, .life = 36, .dmg = 5, .bkb = 300, .kbg = 60, .angle = 35, .dx = 24, .dy = -30 },
    /* Firefly's flame column, frames 34-38, down from under him */
    { .header = 0x57EA, .f0 = 34, .f1 = 38, .ticks = 5, .pal = 0x0D, .box = { -9, -15, 6, 110 },
      .life = 25, .dmg = 8, .bkb = 350, .kbg = 90, .angle = -70, .dx = 0, .dy = 0, .one = 1, .follow = 1 },
};
#define NPROJ ((int)(sizeof(PROJS) / sizeof(PROJS[0])))

#define A(f, l, t, lp) { f, l, t, lp }
static const character_t CHARS[] = {
    { .name = "BATMAN", .header = 0x2EFF, .pal = 0x03,
      .idle = A(3, 4, 30, 1), .walk = A(5, 12, 5, 1), .jump = A(18, 19, 8, 0), .djump = A(20, 22, 5, 0),
      .fall = A(23, 24, 8, 1), .hurt = A(105, 106, 6, 0), .tumble = A(109, 111, 6, 0),
      .mv = {
          [MV_JAB1] = { .a = A(42, 46, 4, 0), .dmg = 3, .bkb = 100, .chain = MV_JAB2 + 1 },
          [MV_JAB2] = { .a = A(47, 53, 4, 0), .dmg = 3, .bkb = 100, .chain = MV_JAB3 + 1 },
          [MV_JAB3] = { .a = A(54, 59, 4, 0), .dmg = 6, .bkb = 500, .kbg = 110, .angle = 40, .lunge = 128 },
          [MV_AIR] = { .a = A(30, 34, 5, 0), .dmg = 7, .bkb = 450, .kbg = 100, .angle = 35 },
          /* hero attacks: the cape strike, the cape uppercut, the low sweep, the cape wrap */
          [MV_FTILT] = { .a = A(60, 66, 4, 0), .dmg = 8, .bkb = 450, .kbg = 115, .angle = 40 },
          [MV_UTILT] = { .a = A(84, 89, 4, 0), .dmg = 7, .bkb = 420, .kbg = 110, .angle = 85 },
          [MV_DTILT] = { .a = A(92, 96, 4, 0), .dmg = 6, .bkb = 300, .kbg = 70, .angle = 20 },
          [MV_UAIR] = { .a = A(84, 88, 4, 0), .dmg = 7, .bkb = 400, .kbg = 105, .angle = 80 },
          [MV_DAIR] = { .a = A(99, 102, 5, 0), .dmg = 9, .bkb = 350, .kbg = 110, .angle = -65 },
          [MV_NSPEC] = { .a = A(133, 136, 5, 0), .proj = PJ_BATARANG, .proj_frame = 134 },
          /* the glide (frames 139-143, cape spread, a kick box) */
          [MV_SSPEC] = { .a = A(139, 143, 6, 0), .dmg = 8, .bkb = 400, .kbg = 100, .angle = 30,
                         .vx0 = 1152, .vy0 = -256, .lowgrav = 1 },
          [MV_USPEC] = { .a = A(84, 89, 4, 0), .dmg = 6, .bkb = 450, .kbg = 90, .angle = 80, .vy0 = -1900,
                         .helpless = 1, .grapple = 1 },
      },
      .walk_v = 576, .air_v = 512, .jump_v = 1536, .djump_v = 1280, .gravity = 64, .fall_v = 1408,
      .weight = 100, .air_jumps = 1, .body = { -8, -44, 8, 0 }, .tall = 48, .reach = 34 },
    { .name = "JOKER", .header = 0xB682, .pal = 0x25,
      .idle = A(0, 0, 30, 1), .walk = A(49, 60, 4, 1), .jump = A(4, 4, 8, 0), .djump = A(6, 6, 8, 0),
      .fall = A(4, 4, 8, 1), .hurt = A(2, 3, 6, 0), .tumble = A(6, 6, 6, 0),
      .mv = {
          /* a hand buzzer poke (his frames have no quick jab: frames 62-64, a box given here) */
          [MV_JAB1] = { .a = A(62, 64, 4, 0), .box = { 8, -36, 28, -20 }, .act0 = 63, .act1 = 64,
                        .dmg = 3, .bkb = 200, .kbg = 20, .angle = 20 },
          [MV_AIR] = { .a = A(4, 6, 5, 0), .dmg = 6, .bkb = 400, .kbg = 90, .angle = 35 },
          [MV_FTILT] = { .a = A(23, 29, 4, 0), .dmg = 9, .bkb = 500, .kbg = 120, .angle = 40 },  /* the mallet */
          [MV_UTILT] = { .a = A(25, 28, 4, 0), .dmg = 7, .bkb = 420, .kbg = 105, .angle = 85 },
          [MV_DTILT] = { .a = A(28, 31, 5, 0), .dmg = 7, .bkb = 350, .kbg = 85, .angle = 25 },
          [MV_UAIR] = { .a = A(26, 28, 4, 0), .dmg = 6, .bkb = 380, .kbg = 95, .angle = 80 },
          [MV_DAIR] = { .a = A(28, 30, 5, 0), .dmg = 8, .bkb = 330, .kbg = 100, .angle = -60 },
          [MV_NSPEC] = { .a = A(38, 43, 4, 0), .proj = PJ_BULLET, .proj_frame = 40 },  /* the gun */
          [MV_SSPEC] = { .a = A(31, 34, 5, 0), .proj = PJ_GAS, .proj_frame = 33 },     /* a gas bomb */
          [MV_USPEC] = { .a = A(4, 6, 5, 0), .dmg = 5, .bkb = 400, .kbg = 80, .angle = 80, .vy0 = -1900,
                         .helpless = 1 },
      },
      .walk_v = 512, .air_v = 480, .jump_v = 1500, .djump_v = 1250, .gravity = 64, .fall_v = 1400,
      .weight = 95, .air_jumps = 1, .body = { -8, -42, 10, 0 }, .tall = 50, .reach = 34 },
    { .name = "BANE", .header = 0xBFB5, .pal = 0x26, .strip_lo = 0x10E2, .strip_hi = 0x10EC,
      .idle = A(0, 20, 4, 1), .walk = A(0, 20, 3, 1), .jump = A(0, 0, 8, 0), .djump = A(0, 0, 8, 0),
      .fall = A(0, 0, 8, 1), .hurt = A(29, 30, 4, 0), .tumble = A(26, 26, 4, 0),
      .mv = {
          /* his frames have no attacks: the stagger 31-34 is his lunge, with boxes given here */
          [MV_JAB1] = { .a = A(31, 34, 5, 0), .box = { 16, -64, 60, -16 }, .act0 = 32, .act1 = 33,
                        .dmg = 10, .bkb = 600, .kbg = 120, .angle = 40, .lunge = 512 },
          [MV_AIR] = { .a = A(31, 34, 5, 0), .box = { 16, -64, 60, -16 }, .act0 = 32, .act1 = 33,
                       .dmg = 9, .bkb = 500, .kbg = 110, .angle = 50 },
          [MV_SSPEC] = { .a = A(31, 34, 6, 0), .box = { 10, -70, 64, -4 }, .act0 = 31, .act1 = 34,
                         .dmg = 12, .bkb = 650, .kbg = 120, .angle = 35, .vx0 = 1024 },        /* the charge */
          [MV_USPEC] = { .a = A(0, 0, 30, 0), .vy0 = -2000, .helpless = 1 },                  /* a big jump */
          [MV_DSPEC] = { .a = A(31, 34, 6, 0), .box = { -34, -24, 40, 4 }, .act0 = 31, .act1 = 34,
                         .dmg = 12, .bkb = 550, .kbg = 110, .angle = 60, .vy0 = 1800 },        /* the ground pound */
      },
      .walk_v = 384, .air_v = 384, .jump_v = 1500, .djump_v = 1200, .gravity = 72, .fall_v = 1600,
      .weight = 140, .air_jumps = 1, .body = { -24, -70, 30, 0 }, .tall = 82, .reach = 56 },
    { .name = "FIREFLY", .header = 0x57EA, .pal = 0x0D,
      .idle = A(14, 19, 6, 1), .walk = A(14, 19, 4, 1), .jump = A(40, 43, 6, 0), .djump = A(44, 47, 4, 0),
      .fall = A(44, 51, 5, 1), .hurt = A(56, 58, 5, 0), .tumble = A(60, 60, 6, 0),
      .mv = {
          [MV_JAB1] = { .a = A(10, 13, 4, 0), .box = { 12, -46, 44, -30 }, .act0 = 11, .act1 = 13,
                        .dmg = 5, .bkb = 350, .kbg = 80, .angle = 25 },
          [MV_AIR] = { .a = A(52, 55, 5, 0), .box = { 6, -36, 28, -8 }, .act0 = 53, .act1 = 55,
                       .dmg = 6, .bkb = 400, .kbg = 95, .angle = 45 },
          [MV_NSPEC] = { .a = A(10, 13, 4, 0), .proj = PJ_FLAME, .proj_frame = 11 },
          [MV_SSPEC] = { .a = A(52, 55, 5, 0), .box = { 6, -36, 30, -8 }, .act0 = 52, .act1 = 55,
                         .dmg = 7, .bkb = 420, .kbg = 95, .angle = 35, .vx0 = 1280, .lowgrav = 1 },  /* jet kick */
          [MV_USPEC] = { .a = A(40, 43, 5, 0), .box = { -10, -64, 10, -40 }, .act0 = 40, .act1 = 43,
                         .dmg = 4, .bkb = 380, .kbg = 70, .angle = 85, .vy0 = -2200, .helpless = 1 },
          [MV_DSPEC] = { .a = A(44, 47, 5, 0), .proj = PJ_COLUMN, .proj_frame = 45 },
      },
      .walk_v = 576, .air_v = 576, .jump_v = 1400, .djump_v = 1100, .gravity = 52, .fall_v = 1100,
      .weight = 85, .air_jumps = 3, .body = { -8, -48, 8, 0 }, .tall = 54, .reach = 40 },
    { .name = "PENGUIN", .header = 0x4D64, .pal = 0x0B,
      .idle = A(6, 7, 20, 1), .walk = A(68, 77, 4, 1), .jump = A(79, 79, 8, 0), .djump = A(79, 79, 8, 0),
      .fall = A(79, 79, 8, 1), .hurt = A(80, 81, 6, 0), .tumble = A(79, 79, 6, 0),
      .mv = {
          [MV_JAB1] = { .a = A(21, 25, 4, 0), .dmg = 5, .bkb = 380, .kbg = 85, .angle = 30 },
          [MV_AIR] = { .a = A(16, 20, 4, 0), .dmg = 6, .bkb = 420, .kbg = 95, .angle = 40 },
          [MV_NSPEC] = { .a = A(21, 25, 4, 0), .proj = PJ_BOMB, .proj_frame = 23 },
          /* the umbrella's chain, out to 128 px (frames 37-50) */
          [MV_SSPEC] = { .a = A(37, 50, 3, 0), .dmg = 8, .bkb = 420, .kbg = 95, .angle = 25 },
          [MV_USPEC] = { .a = A(16, 20, 4, 0), .dmg = 5, .bkb = 380, .kbg = 80, .angle = 80, .vy0 = -1850,
                         .helpless = 1 },
      },
      .walk_v = 448, .air_v = 448, .jump_v = 1450, .djump_v = 1200, .gravity = 64, .fall_v = 1400,
      .weight = 110, .air_jumps = 1, .body = { -11, -32, 9, 0 }, .tall = 40, .reach = 30 },
    { .name = "MR.FREEZE", .header = 0x489F, .pal = 0x0A,
      .idle = A(0, 4, 10, 1), .walk = A(13, 18, 6, 1), .jump = A(0, 0, 8, 0), .djump = A(0, 0, 8, 0),
      .fall = A(0, 0, 8, 1), .hurt = A(12, 12, 8, 0), .tumble = A(16, 16, 8, 0),
      .mv = {
          [MV_JAB1] = { .a = A(5, 11, 4, 0), .box = { 12, -46, 30, -30 }, .act0 = 7, .act1 = 10,
                        .dmg = 7, .bkb = 450, .kbg = 100, .angle = 30 },
          /* the freeze breath, round his head */
          [MV_AIR] = { .a = A(21, 26, 4, 0), .box = { -34, -70, 20, -46 }, .act0 = 22, .act1 = 26,
                       .dmg = 8, .bkb = 450, .kbg = 105, .angle = 80 },
          [MV_NSPEC] = { .a = A(19, 29, 4, 0), .box = { -36, -72, 22, -44 }, .act0 = 22, .act1 = 27,
                         .dmg = 10, .bkb = 480, .kbg = 110, .angle = 80 },
          [MV_USPEC] = { .a = A(0, 0, 30, 0), .vy0 = -1900, .helpless = 1 },
      },
      .walk_v = 416, .air_v = 416, .jump_v = 1450, .djump_v = 1150, .gravity = 70, .fall_v = 1500,
      .weight = 125, .air_jumps = 1, .body = { -12, -54, 12, 0 }, .tall = 66, .reach = 32 },
    /* mods/robin-p2's Robin: Batman's frame slots, rebuilt from the staff fighter's, in the
     * upper ROM copy (notes/sprites.md). Only offered when that mod is loaded. Its frames 139 on
     * are still Batman's, so no glide: the flying kick dashes instead. */
    { .name = "ROBIN", .header = 0x2EFF, .pal = 0x03, .bank = 0x251C40, .palettes = 0x251840,
      .idle = A(3, 4, 30, 1), .walk = A(5, 12, 4, 1), .jump = A(18, 19, 8, 0), .djump = A(20, 22, 5, 0),
      .fall = A(23, 24, 8, 1), .hurt = A(105, 106, 6, 0), .tumble = A(109, 111, 6, 0),
      .mv = {
          [MV_JAB1] = { .a = A(42, 46, 4, 0), .dmg = 3, .bkb = 100, .chain = MV_JAB2 + 1 },
          [MV_JAB2] = { .a = A(47, 53, 4, 0), .dmg = 3, .bkb = 100, .chain = MV_JAB3 + 1 },
          [MV_JAB3] = { .a = A(54, 59, 4, 0), .dmg = 5, .bkb = 450, .kbg = 105, .angle = 40, .lunge = 160 },
          [MV_AIR] = { .a = A(30, 34, 5, 0), .dmg = 6, .bkb = 420, .kbg = 95, .angle = 35 },
          [MV_FTILT] = { .a = A(60, 66, 4, 0), .dmg = 7, .bkb = 420, .kbg = 110, .angle = 40 },
          [MV_UTILT] = { .a = A(84, 89, 4, 0), .dmg = 6, .bkb = 400, .kbg = 105, .angle = 85 },
          [MV_DTILT] = { .a = A(92, 96, 4, 0), .dmg = 5, .bkb = 280, .kbg = 65, .angle = 20 },
          [MV_UAIR] = { .a = A(84, 88, 4, 0), .dmg = 6, .bkb = 380, .kbg = 100, .angle = 80 },
          [MV_DAIR] = { .a = A(99, 102, 5, 0), .dmg = 8, .bkb = 330, .kbg = 105, .angle = -65 },
          [MV_NSPEC] = { .a = A(133, 136, 4, 0), .proj = PJ_BATARANG, .proj_frame = 134 },
          [MV_SSPEC] = { .a = A(30, 34, 5, 0), .dmg = 7, .bkb = 400, .kbg = 95, .angle = 30, .vx0 = 1280,
                         .lowgrav = 1 },
          [MV_USPEC] = { .a = A(84, 89, 4, 0), .dmg = 5, .bkb = 430, .kbg = 85, .angle = 80, .vy0 = -2000,
                         .helpless = 1, .grapple = 1 },
      },
      .walk_v = 640, .air_v = 576, .jump_v = 1560, .djump_v = 1300, .gravity = 64, .fall_v = 1408,
      .weight = 90, .air_jumps = 1, .body = { -8, -44, 8, 0 }, .tall = 46, .reach = 40 },
};
enum { CH_ROBIN = 6 };
#define NCHARS ((int)(sizeof(CHARS) / sizeof(CHARS[0])))

/* ---- stages ----
 * Positions are map pixels of the stage's scene (the "world"). The camera is the top left of the
 * screen in the world; the tilemap ring (64x32 tiles) is filled round it every frame. */
typedef struct platform {
    int16_t x0, x1, top;
    int8_t solid;            /* 1: can't be dropped through, and a block SOLID_DEPTH deep */
    int8_t ledges;           /* bit 0: its left end can be hung from, bit 1: its right end */
} platform_t;

typedef struct stage {
    const char *name;
    uint16_t scene;
    int16_t cam_x0, cam_x1, cam_y0, cam_y1;   /* where the camera may go */
    int16_t blast[4];                          /* x0, y0, x1, y1: past these is a KO */
    int16_t spawn_x0, spawn_x1, spawn_y;       /* the fighters start spread over this */
    int16_t respawn_x, respawn_y;
    uint16_t free_banks;                       /* sprite palette banks 0-7 the scene leaves free */
    platform_t plat[8];
    int8_t nplat;                              /* plat[0] is the main one */
    struct { int8_t bx0, bx1, by0, by1; } cut[4];   /* page 2 blocks taken out (walls, floor ends) */
    int8_t ncut;
} stage_t;

static const stage_t STAGES[] = {
    /* scene 8: a steel floor (its ends cut off) and a catwalk; one screen */
    { .name = "CATWALK", .scene = 8, .cam_x0 = 32, .cam_x1 = 32, .cam_y0 = 0, .cam_y1 = 0,
      .blast = { -32, -96, 416, 304 }, .spawn_x0 = 80, .spawn_x1 = 304, .spawn_y = 208,
      .respawn_x = 192, .respawn_y = 56, .free_banks = 0xFC,
      .plat = { { 64, 320, 208, 1, 3 }, { 96, 288, 112, 0, 0 } }, .nplat = 2,
      .cut = { { 0, 1, 6, 7 }, { 10, 11, 6, 7 } }, .ncut = 2 },
    /* scene 3 (Clock's Ticking), the top room of the clock tower, its side walls taken out: the
     * floor, a long stone platform, stone ledges and wooden beams on both sides, round the clock */
    { .name = "CLOCK TOWER", .scene = 3, .cam_x0 = 320, .cam_x1 = 640, .cam_y0 = 0, .cam_y1 = 240,
      .blast = { 288, -96, 992, 560 }, .spawn_x0 = 432, .spawn_x1 = 848, .spawn_y = 448,
      .respawn_x = 640, .respawn_y = 250, .free_banks = 0xC8,
      .plat = { { 384, 896, 448, 1, 3 }, { 384, 896, 320, 0, 3 }, { 384, 544, 128, 0, 3 },
                { 736, 896, 128, 0, 3 }, { 384, 544, 224, 0, 2 }, { 736, 896, 224, 0, 1 } },
      .nplat = 6, .cut = { { 10, 11, 1, 14 }, { 28, 29, 1, 14 } }, .ncut = 2 },
};
#define NSTAGES ((int)(sizeof(STAGES) / sizeof(STAGES[0])))
#define STAGE(v) (&STAGES[(v)->stage])

/* sin of 0, 5, ... 90 degrees, x256 */
static const int16_t SIN5[19] = { 0, 22, 44, 66, 88, 108, 128, 147, 165, 181, 196, 210, 222, 232, 241, 247, 252, 255, 256 };

static int isin(int deg) { return deg < 0 ? -SIN5[(deg < -90 ? 90 : -deg) / 5] : SIN5[(deg > 90 ? 90 : deg) / 5]; }
static int icos(int deg) { return isin(90 - (deg < 0 ? -deg : deg)); }

static uint32_t rnd(versus_t *v)
{
    uint32_t x = v->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return v->rng = x;
}

const char *versus_char_name(int ch) { return ch >= 0 && ch < NCHARS ? CHARS[ch].name : "?"; }

/* ---- the ROM's frame records ---- */

static uint32_t frame_rec(const uint16_t *rom, const character_t *c, int f)
{
    const uint32_t bank = c->bank ? c->bank : BANK;
    const uint32_t list = bank + c->header + HEADER_FRAMES;
    if (f < 0 || f >= rom[bank + c->header]) f = 0;
    return bank + rom[list + f];
}

/* the frame's boxes of the given kind (attack 1, body 0), in world px for a fighter at (x, y)
 * facing `facing`; returns how many (at most max) */
static int frame_boxes(const uint16_t *rom, const character_t *c, int f, int attack, int x, int y,
                       int facing, int (*out)[4], int max)
{
    const uint32_t a = frame_rec(rom, c, f);
    const int k = rom[a + 3];
    int n = 0;
    for (int b = 0; b + 5 <= k && n < max; b += 5) {
        const int16_t *w = (const int16_t *)&rom[a + 4 + b];
        const int type = w[4];
        const int is_attack = type == 3 || type == 4 || type == 5 || type == 0x14;
        const int is_body = (type == 1 || type == 2 || type == 6) && w[3] <= 2; /* not Bane's platform */
        if (attack ? !is_attack : !is_body) continue;
        const int x0 = facing > 0 ? w[0] : -w[2], x1 = facing > 0 ? w[2] : -w[0];
        out[n][0] = x + x0; out[n][1] = y + w[1]; out[n][2] = x + x1; out[n][3] = y + w[3];
        n++;
    }
    return n;
}

static void box_at(const int16_t *b, int x, int y, int facing, int *out)
{
    out[0] = x + (facing > 0 ? b[0] : -b[2]);
    out[2] = x + (facing > 0 ? b[2] : -b[0]);
    out[1] = y + b[1];
    out[3] = y + b[3];
}

static int overlap(const int *a, const int *b)
{
    return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
}

/* ---- fighters ---- */

static void set_anim(vs_fighter_t *f, const anim_t *a)
{
    if (f->anim_first == a->first && f->anim_last == a->last && f->anim_ticks == a->ticks) return;
    f->anim_first = a->first;
    f->anim_last = a->last;
    f->anim_ticks = a->ticks;
    f->anim_loop = a->loop;
    f->frame = a->first;
    f->frame_t = 0;
}

/* advances the animation; 1 when a one-shot animation has just played its last frame out */
static int step_anim(vs_fighter_t *f)
{
    if (++f->frame_t < f->anim_ticks) return 0;
    f->frame_t = 0;
    if (f->frame < f->anim_last) {
        f->frame++;
        return 0;
    }
    if (f->anim_loop) {
        f->frame = f->anim_first;
        return 0;
    }
    f->frame_t = f->anim_ticks - 1;   /* hold the last frame */
    return 1;
}

static void spawn(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    const character_t *c = &CHARS[f->ch];
    const stage_t *st = STAGE(v);
    const int w = st->spawn_x1 - st->spawn_x0;
    f->x = (st->spawn_x0 + (2 * i + 1) * w / (2 * v->n)) * SUB;
    f->y = st->spawn_y * SUB;
    f->vx = f->vy = 0;
    f->facing = f->x < (st->spawn_x0 + st->spawn_x1) / 2 * SUB ? 1 : -1;
    f->state = ST_FREE;
    f->grounded = 1;
    f->jumps = c->air_jumps;
    f->shield = SHIELD_MAX;
    f->shield_stun = f->dodge = f->intangible = f->air_dodged = f->helpless = f->squat = 0;
    f->anim_first = -1;
    set_anim(f, &c->idle);
}

static void update_camera(versus_t *v, int snap);

int versus_roster(const uint16_t *rom, int *out)
{
    /* Robin is there if mods/robin-p2 changed the upper copy of Batman's frame list */
    const int robin = memcmp(rom + 0x254B4B, rom + 0x054B4B, 183 * sizeof(uint16_t)) != 0;
    int n = 0;
    for (int c = 0; c < NCHARS; c++)
        if (c != CH_ROBIN || robin) out[n++] = c;
    return n;
}

void versus_init(versus_t *v, const uint16_t *rom, int n, uint32_t bot_mask, int stocks, uint32_t seed,
                 int select)
{
    int roster[16];
    const int nr = versus_roster(rom, roster);
    memset(v, 0, sizeof(*v));
    v->magic = 0x56535631;   /* "VSV1" */
    v->size = sizeof(*v);
    v->n = n < 2 ? 2 : n > VS_MAX ? VS_MAX : n;
    v->rng = seed ? seed : 0x2545F491;
    v->stocks = stocks > 0 ? stocks : 3;
    v->phase = select ? VS_SELECT : VS_COUNTDOWN;
    v->select = select;
    v->bot_mask = bot_mask;
    v->winner = -1;
    for (int i = 0; i < VS_MAX; i++) {
        vs_fighter_t *f = &v->f[i];
        f->ch = i < v->n ? roster[i % nr] : -1;
        if (f->ch < 0) { f->state = ST_OUT; continue; }
        f->bot = (bot_mask >> i) & 1;
        f->stocks = v->stocks;
        f->last_hit_by = -1;
        spawn(v, i);
    }
    update_camera(v, 1);
}

void versus_set_char(versus_t *v, int i, int ch)
{
    if (i < 0 || i >= v->n || ch < 0 || ch >= NCHARS) return;
    vs_fighter_t *f = &v->f[i];
    f->ch = ch;
    f->anim_first = -1;
    spawn(v, i);
}

static int alive(const vs_fighter_t *f)
{
    return f->ch >= 0 && f->state != ST_OUT && !(f->state == ST_RESPAWN && f->t < RESPAWN_HIDDEN);
}

static int has_move(const character_t *c, int mv) { return c->mv[mv].a.ticks != 0; }

static void start_move(vs_fighter_t *f, const character_t *c, int mv)
{
    const move_t *m = &c->mv[mv];
    f->state = ST_ATTACK;
    f->move = mv;
    f->t = 0;
    f->buffered = 0;
    f->hit_mask = 0;
    f->spawned = 0;
    f->air_move = !f->grounded;
    f->anim_first = -1;
    set_anim(f, &m->a);
    if (m->lunge) f->vx = m->lunge * f->facing;
    if (m->vx0) f->vx = m->vx0 * f->facing;
    if (m->vy0) {
        f->vy = m->vy0;
        if (m->vy0 < 0 && f->grounded) { f->grounded = 0; f->air_move = 1; }
    }
}

/* which move B (attack) or X (special) starts, with the direction held */
static int pick_move(const vs_fighter_t *f, const character_t *c, uint16_t b, int special)
{
    const int side = (b & (VS_LEFT | VS_RIGHT)) != 0, up = (b & VS_UP) != 0, down = (b & VS_DOWN) != 0;
    int mv;
    if (special) mv = up ? MV_USPEC : down ? MV_DSPEC : side ? MV_SSPEC : MV_NSPEC;
    else if (f->grounded) mv = up ? MV_UTILT : down ? MV_DTILT : side ? MV_FTILT : MV_JAB1;
    else mv = up ? MV_UAIR : down ? MV_DAIR : MV_AIR;
    if (has_move(c, mv)) return mv;
    if (special) return -1;
    return f->grounded ? MV_JAB1 : MV_AIR;   /* a tilt or aerial it doesn't have: the plain one */
}

static void spawn_proj(versus_t *v, int owner, int kind)
{
    const proj_def_t *d = &PROJS[kind - 1];
    const vs_fighter_t *f = &v->f[owner];
    int slot = -1;
    for (int k = 0; k < VS_PROJ; k++) {
        if (v->proj[k].kind == kind && v->proj[k].owner == owner && d->one) return;   /* one out already */
        if (!v->proj[k].kind && slot < 0) slot = k;
    }
    if (slot < 0) return;
    vs_proj_t *p = &v->proj[slot];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->owner = owner;
    p->facing = f->facing;
    p->x = f->x + d->dx * f->facing * SUB;
    p->y = f->y + d->dy * SUB;
    p->vx = d->vx * f->facing;
    p->vy = d->vy;
}

static void to_free(vs_fighter_t *f, const character_t *c)
{
    if (f->state == ST_ATTACK && c->mv[f->move].helpless && !f->grounded) f->helpless = 1;
    f->state = ST_FREE;
    f->t = 0;
    set_anim(f, f->grounded ? &c->idle : &c->fall);
}

/* a bot's buttons for this tick */
static uint16_t bot_input(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    const character_t *c = &CHARS[f->ch];
    const int x = f->x / SUB, y = f->y / SUB;
    uint16_t b = 0;
    int best = -1, bd = 1 << 30;
    for (int j = 0; j < v->n; j++) {
        if (j == i || !alive(&v->f[j])) continue;
        const int d = (v->f[j].x - f->x) / SUB, e = (v->f[j].y - f->y) / SUB;
        const int dist = (d < 0 ? -d : d) + (e < 0 ? -e : e);
        if (dist < bd) { bd = dist; best = j; }
    }
    const stage_t *st = STAGE(v);
    const platform_t *main_p = &st->plat[0];
    const int x0 = main_p->x0, x1 = main_p->x1, mid = (x0 + x1) / 2;
    const int offstage = x < x0 + 6 || x > x1 - 6 || y > main_p->top + 4;
    if (f->state == ST_LEDGE)   /* climb up after a moment, now and then jump up instead */
        return f->t < 20 + (int)(rnd(v) & 15) ? 0 : (rnd(v) & 3) ? (f->facing > 0 ? VS_RIGHT : VS_LEFT) : VS_JUMP;
    if (!f->grounded && offstage) {   /* recover: back towards the middle, jump when falling */
        b |= x < mid ? VS_RIGHT : VS_LEFT;
        if (f->vy > 0 && f->jumps > 0 && !(f->prev & VS_JUMP) && y > 150) b |= VS_JUMP;
        else if (f->vy > 0 && !f->jumps && !f->helpless && y > main_p->top - 40 && has_move(c, MV_USPEC) &&
                 !(f->prev & VS_SPECIAL) && f->state == ST_FREE)
            b = VS_UP | VS_SPECIAL | (x < mid ? VS_RIGHT : VS_LEFT);   /* the recovery move */
        return b;
    }
    if (best < 0) return 0;
    const vs_fighter_t *t = &v->f[best];
    const int dx = (t->x - f->x) / SUB, dy = (t->y - f->y) / SUB;
    const int adx = dx < 0 ? -dx : dx;
    if (f->bot_t == 0) {   /* rethink the direction every few ticks */
        f->bot_hold = 0;
        if (adx > c->reach - 8) f->bot_hold = dx > 0 ? VS_RIGHT : VS_LEFT;
        else if ((dx > 0) != (f->facing > 0)) f->bot_hold = dx > 0 ? VS_RIGHT : VS_LEFT;
        if ((rnd(v) & 7) == 0) f->bot_hold = 0;   /* hesitate now and then */
        f->bot_t = 4 + rnd(v) % 8;
    } else f->bot_t--;
    /* shield an attack that is coming, now and then roll away from it */
    if (t->state == ST_ATTACK && adx <= CHARS[t->ch].reach + 12 && dy > -48 && dy < 40 && f->grounded &&
        f->shield > SHIELD_MAX / 3 && f->state == ST_FREE && rnd(v) % 4 == 0) {
        f->bot_hold = VS_BLOCK;
        f->bot_t = 10 + rnd(v) % 16;
    }
    if (f->state == ST_SHIELD && f->bot_hold == VS_BLOCK && rnd(v) % 40 == 0)
        return VS_BLOCK | (dx > 0 ? VS_LEFT : VS_RIGHT);
    b |= f->bot_hold;
    /* don't walk off the floor after someone who is off it */
    if (f->grounded == 1 && ((b & VS_LEFT && x < x0 + 14) || (b & VS_RIGHT && x > x1 - 14)))
        b &= ~(VS_LEFT | VS_RIGHT);
    if (adx <= c->reach && dy > -48 && dy < 40 && !(f->prev & (VS_ATTACK | VS_SPECIAL)) && rnd(v) % 3 == 0) {
        /* a mix of the plain attack, the side and up tilts, and aerials by where the target is */
        const uint32_t r = rnd(v) % 4;
        b &= (uint16_t)~(VS_UP | VS_DOWN);
        if (dy < -28) b |= VS_UP;
        else if (!f->grounded && dy > 20) b |= VS_DOWN;
        else if (r == 0) b &= (uint16_t)~(VS_LEFT | VS_RIGHT);
        else if (r == 1) b |= dx > 0 ? VS_RIGHT : VS_LEFT;
        b |= VS_ATTACK;
    } else if (adx > 70 && dy > -30 && dy < 30 && f->grounded && (dx > 0) == (f->facing > 0) &&
               has_move(c, MV_NSPEC) && c->mv[MV_NSPEC].proj && !(f->prev & VS_SPECIAL) && rnd(v) % 30 == 0)
        b = VS_SPECIAL;   /* shoot from afar (no direction: the neutral special) */
    if (!(f->prev & VS_JUMP)) {
        if (dy < -40 && f->grounded && rnd(v) % 16 == 0) b |= VS_JUMP;
        else if (dy < -30 && !f->grounded && f->vy > 0 && f->jumps > 0 && rnd(v) % 6 == 0) b |= VS_JUMP;
        else if (rnd(v) % 200 == 0) b |= VS_JUMP;
    }
    if (dy > 40 && f->grounded > 1 && !(f->prev & VS_DOWN) && rnd(v) % 24 == 0) b |= VS_DOWN;
    return b;
}

static void ko(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    f->falls++;
    if (f->last_hit_by >= 0) v->f[f->last_hit_by].kos++;
    f->last_hit_by = -1;
    f->damage = 0;
    f->vx = f->vy = 0;
    f->hitstun = f->hitlag = 0;
    f->grounded = 0;
    f->t = 0;
    f->helpless = f->squat = 0;
    f->state = --f->stocks > 0 ? ST_RESPAWN : ST_OUT;
}

static void start_dodge(vs_fighter_t *f, const character_t *c, int kind, int dir)
{
    f->state = ST_DODGE;
    f->dodge = kind;
    f->t = 0;
    f->intangible = 0;
    if (kind == 2) { f->vx = dir * ROLL_V; f->facing = -dir; }   /* rolls away, facing back */
    if (kind == 3) { f->air_dodged = 1; f->vx /= 2; if (f->vy < 0) f->vy /= 2; }
    set_anim(f, kind == 2 ? &c->walk : kind == 3 ? &c->fall : &c->idle);
}

static void shield_break(vs_fighter_t *f, const character_t *c)
{
    f->state = ST_DIZZY;
    f->hitstun = SHIELD_BREAK_STUN;
    f->shield = SHIELD_MAX * 3 / 8;
    f->shield_stun = 0;
    f->vy = -5 * SUB;
    f->vx = 0;
    f->grounded = 0;
    f->anim_first = -1;
    set_anim(f, &c->hurt);
}

/* a ledge's hang point: x of the end, and the platform's top */
static int ledge_x(const platform_t *p, int side) { return side ? p->x1 : p->x0; }

static int ledge_taken(const versus_t *v, int i, int id)
{
    for (int j = 0; j < v->n; j++)
        if (j != i && v->f[j].state == ST_LEDGE && v->f[j].ledge == id) return 1;
    return 0;
}

/* fighter i takes hold of ledge id (platform * 2 + side + 1), if nobody has it */
static int hang(versus_t *v, int i, int id)
{
    vs_fighter_t *f = &v->f[i];
    if (ledge_taken(v, i, id)) return 0;
    const platform_t *p = &STAGE(v)->plat[(id - 1) / 2];
    const int side = (id - 1) & 1, out = side ? 1 : -1;
    f->state = ST_LEDGE;
    f->ledge = id;
    f->t = 0;
    f->x = (ledge_x(p, side) + out * 8) * SUB;
    f->y = (p->top + 36) * SUB;
    f->vx = f->vy = 0;
    f->grounded = 0;
    f->facing = -out;
    f->jumps = CHARS[f->ch].air_jumps;
    f->helpless = f->air_dodged = 0;
    if (!f->ledge_used) f->intangible = 40;   /* the first catch since landing is safe for a moment */
    f->ledge_used = 1;
    f->anim_first = -1;
    set_anim(f, &CHARS[f->ch].jump);
    return 1;
}

/* the grapnel: the nearest free ledge above and in front of the hand, in reach */
static int try_grapple(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    const stage_t *st = STAGE(v);
    const int hx = f->x / SUB + f->facing * 6, hy = f->y / SUB - 34;
    int best = 0, bd = 1 << 30;
    for (int k = 0; k < st->nplat; k++)
        for (int side = 0; side < 2; side++) {
            const platform_t *p = &st->plat[k];
            if (!(p->ledges >> side & 1)) continue;
            const int dx = ledge_x(p, side) - hx, dy = p->top - hy;
            if (dy > 24 || dx * f->facing < -24) continue;   /* below the hand, or behind */
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            const int d = ax > ay ? ax + ay / 2 : ay + ax / 2;
            if (d > GRAPNEL_REACH || d >= bd || ledge_taken(v, i, k * 2 + side + 1)) continue;
            bd = d;
            best = k * 2 + side + 1;
        }
    if (!best) return 0;
    const platform_t *p = &st->plat[(best - 1) / 2];
    f->state = ST_GRAPPLE;
    f->t = 0;
    f->ledge = best;
    f->grapple_x = ledge_x(p, (best - 1) & 1);
    f->grapple_y = p->top;
    f->vx = f->vy = 0;
    f->grounded = 0;
    f->squat = 0;
    f->anim_first = -1;
    set_anim(f, &CHARS[f->ch].mv[MV_NSPEC].a);   /* the throwing arm */
    return 1;
}

/* on the line: pulled up to the anchor, then hanging from that ledge */
static void on_grapple(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    if (f->frame < f->anim_last) step_anim(f);
    if (f->t < GRAPNEL_FLY) return;
    const platform_t *p = &STAGE(v)->plat[(f->ledge - 1) / 2];
    const int out = (f->ledge - 1) & 1 ? 1 : -1;
    const int dx = (f->grapple_x + out * 8) * SUB - f->x, dy = (p->top + 36) * SUB - f->y;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int d = ax > ay ? ax + ay / 2 : ay + ax / 2;
    if (d <= GRAPNEL_PULL) {
        if (!hang(v, i, f->ledge)) { f->state = ST_FREE; f->helpless = 1; f->ledge = 0; }
        return;
    }
    f->x += (int)((int64_t)dx * GRAPNEL_PULL / d);
    f->y += (int)((int64_t)dy * GRAPNEL_PULL / d);
    if (f->t > 90) { f->state = ST_FREE; f->helpless = 1; f->ledge = 0; }
}

/* a falling fighter near a free ledge end catches it (Smash's ledge grab) */
static void grab_ledge(versus_t *v, int i)
{
    vs_fighter_t *f = &v->f[i];
    const stage_t *st = STAGE(v);
    const int x = f->x / SUB, y = f->y / SUB;
    for (int k = 0; k < st->nplat; k++)
        for (int side = 0; side < 2; side++) {
            const platform_t *p = &st->plat[k];
            if (!(p->ledges >> side & 1)) continue;
            const int lx = ledge_x(p, side), out = side ? 1 : -1;   /* the open side */
            const int dx = (x - lx) * out;   /* > 0: outside the end */
            if (dx < -6 || dx > 18 || y < p->top + 8 || y > p->top + 48) continue;
            if (hang(v, i, k * 2 + side + 1)) return;
        }
}

/* hanging from a ledge: climb, jump, attack or let go */
static void on_ledge(versus_t *v, vs_fighter_t *f, const character_t *c, uint16_t b, uint16_t pressed)
{
    const platform_t *p = &STAGE(v)->plat[(f->ledge - 1) / 2];
    const int side = (f->ledge - 1) & 1, in = side ? -1 : 1;   /* towards the platform */
    const int lx = ledge_x(p, side);
    const int toward = (b & (in > 0 ? VS_RIGHT : VS_LEFT)) != 0, away = (b & (in > 0 ? VS_LEFT : VS_RIGHT)) != 0;
    if (f->t < 6) return;   /* the catch */
    if (pressed & (VS_JUMP | VS_UP)) {
        f->state = ST_FREE;
        f->x = (lx + in * 4) * SUB;
        f->vy = -c->jump_v;
        f->ledge_cd = 20;
        set_anim(f, &c->jump);
    } else if ((pressed & (VS_ATTACK | VS_SPECIAL)) || (toward && f->t > 10) || f->t > 300) {
        f->state = ST_FREE;   /* up onto the platform (B: with the plain attack) */
        f->x = (lx + in * 16) * SUB;
        f->y = p->top * SUB;
        f->vy = 0;
        f->grounded = (f->ledge - 1) / 2 + 1;
        f->facing = in;
        set_anim(f, &c->idle);
        if (pressed & (VS_ATTACK | VS_SPECIAL)) start_move(f, c, MV_JAB1);
    } else if ((away || (b & VS_DOWN)) && f->t > 10) {
        f->state = ST_FREE;
        f->x -= in * 4 * SUB;
        f->ledge_cd = 30;
        set_anim(f, &c->fall);
    }
    if (f->state != ST_LEDGE) f->ledge = 0;
}

/* movement, platforms and blast zones for one fighter */
static void physics(versus_t *v, int i, uint16_t b, uint16_t pressed)
{
    vs_fighter_t *f = &v->f[i];
    const character_t *c = &CHARS[f->ch];
    const int dir = (b & VS_RIGHT ? 1 : 0) - (b & VS_LEFT ? 1 : 0);

    if (f->state == ST_RESPAWN) {
        if (++f->t < RESPAWN_HIDDEN) return;
        if (f->t == RESPAWN_HIDDEN) {
            f->x = STAGE(v)->respawn_x * SUB;
            f->y = STAGE(v)->respawn_y * SUB;
            f->vx = f->vy = 0;
            f->invuln = INVULN + RESPAWN_HOVER;
            f->jumps = c->air_jumps;
            f->anim_first = -1;
            set_anim(f, &c->fall);
        }
        step_anim(f);
        if (f->t >= RESPAWN_HIDDEN + RESPAWN_HOVER || (pressed & ~VS_MENU)) {
            f->state = ST_FREE;
            f->t = 0;
            if (f->invuln > INVULN) f->invuln = INVULN;
        }
        return;
    }

    f->t++;
    if (f->intangible > 0) f->intangible--;
    if (f->ledge_cd > 0) f->ledge_cd--;
    if (f->state == ST_LEDGE) {
        on_ledge(v, f, c, b, pressed);
        if (f->state == ST_LEDGE) { step_anim(f); return; }
    }
    if (f->state == ST_GRAPPLE) {
        on_grapple(v, i);
        if (f->state == ST_GRAPPLE || f->state == ST_LEDGE) return;
    }
    if (f->state != ST_SHIELD && f->shield < SHIELD_MAX) {
        f->shield += SHIELD_REGEN;
        if (f->shield > SHIELD_MAX) f->shield = SHIELD_MAX;
    }
    if (f->state == ST_FREE && (b & VS_BLOCK) && f->grounded && f->shield > SUB) {
        f->state = ST_SHIELD;   /* up comes the shield */
        f->t = 0;
        set_anim(f, &c->idle);
    } else if (f->state == ST_FREE && (pressed & VS_BLOCK) && !f->grounded && !f->air_dodged && !f->helpless) {
        start_dodge(f, c, 3, 0);
    }
    if (f->state == ST_SHIELD) {
        if (f->vx > 0) f->vx = f->vx > 40 ? f->vx - 40 : 0;
        else f->vx = f->vx < -40 ? f->vx + 40 : 0;
        if ((f->shield -= SHIELD_DRAIN) <= 0) shield_break(f, c);
        else if (f->shield_stun > 0) f->shield_stun--;
        else if (!(b & VS_BLOCK)) to_free(f, c);
        else if (pressed & (VS_JUMP | VS_UP)) {   /* jump out of the shield */
            to_free(f, c);
            pressed &= (uint16_t)~VS_UP;
            pressed |= VS_JUMP;
        } else if (pressed & (VS_LEFT | VS_RIGHT)) start_dodge(f, c, 2, dir);
        else if (pressed & VS_DOWN) start_dodge(f, c, 1, 0);
    }
    if (f->state == ST_DODGE) {
        if (f->t == INTANGIBLE_FROM) f->intangible = INTANGIBLE_FOR;
        if (f->dodge == 2 && f->t == ROLL_MOVE) f->vx = 0;
        if (f->dodge == 3) f->vx -= f->vx / 16;
        const int len = f->dodge == 2 ? ROLL_TICKS : f->dodge == 1 ? SPOT_TICKS : AIR_DODGE_TICKS;
        if (f->t >= len) { f->dodge = 0; to_free(f, c); }
    } else if (f->state == ST_DIZZY) {
        if (f->vx > 0) f->vx = f->vx > 40 ? f->vx - 40 : 0;
        else f->vx = f->vx < -40 ? f->vx + 40 : 0;
        if (--f->hitstun <= 0) to_free(f, c);
    }
    if (f->state == ST_FREE) {
        if (f->grounded) {
            f->vx = dir * c->walk_v;
            if (dir) f->facing = dir;
            set_anim(f, dir ? &c->walk : &c->idle);
        } else {
            f->vx += dir * c->air_v / 8;
            if (f->vx > c->air_v) f->vx = c->air_v;
            if (f->vx < -c->air_v) f->vx = -c->air_v;
            if (!dir) f->vx -= f->vx / 16;
            if (f->vy > 0 && f->anim_first != c->djump.first) set_anim(f, &c->fall);
        }
        if (f->helpless) pressed &= (uint16_t)~(VS_JUMP | VS_UP | VS_ATTACK | VS_SPECIAL);
        if (f->squat > 0) {   /* jumpsquat: an attack now is an up tilt or special, else the jump goes */
            if (pressed & (VS_ATTACK | VS_SPECIAL)) f->squat = 0;
            else if (--f->squat == 0 && f->grounded) {
                f->vy = -c->jump_v;
                f->grounded = 0;
                f->jumps = c->air_jumps;
                set_anim(f, &c->jump);
            }
        } else if (pressed & (VS_JUMP | VS_UP) && !(pressed & (VS_SPECIAL | VS_ATTACK))) {
            if (f->grounded) {
                f->squat = JUMPSQUAT;
                f->vx = 0;
            } else if (f->jumps > 0) {
                f->vy = -c->djump_v;
                f->jumps--;
                if (dir) f->facing = dir;
                f->anim_first = -1;
                set_anim(f, &c->djump);
            }
        }
        if (pressed & VS_DOWN) {
            if (f->grounded && !STAGE(v)->plat[f->grounded - 1].solid) {
                f->grounded = 0;
                f->drop = DROP_TICKS;
                f->y += 2 * SUB;
                f->vy = SUB;
                set_anim(f, &c->fall);
            } else if (!f->grounded && f->vy > -SUB) f->vy = c->fall_v * 3 / 2;   /* fast fall */
        }
        if (pressed & (VS_ATTACK | VS_SPECIAL)) {
            const int mv = pick_move(f, c, b, (pressed & VS_SPECIAL) != 0);
            if (mv >= 0) {
                if (dir) f->facing = dir;
                if (!(c->mv[mv].grapple && try_grapple(v, i))) start_move(f, c, mv);
            }
        }
    } else if (f->state == ST_ATTACK) {
        const move_t *m = &c->mv[f->move];
        if (pressed & VS_ATTACK) f->buffered = 1;
        if (f->grounded) {
            if (f->vx > 0) f->vx = f->vx > 40 ? f->vx - 40 : 0;
            else f->vx = f->vx < -40 ? f->vx + 40 : 0;
        } else {
            f->vx += dir * c->air_v / 16;
            if (f->vx > c->air_v) f->vx = c->air_v;
            if (f->vx < -c->air_v) f->vx = -c->air_v;
        }
        if (m->vx0) f->vx = m->vx0 * f->facing;   /* a dash keeps its speed */
        if (m->proj && !f->spawned && f->frame == m->proj_frame) {
            spawn_proj(v, i, m->proj);
            f->spawned = 1;
        }
        if (step_anim(f)) {
            if (f->buffered && m->chain && f->grounded && has_move(c, m->chain - 1)) start_move(f, c, m->chain - 1);
            else to_free(f, c);
        }
    } else if (f->state == ST_HITSTUN) {
        const int fr = f->grounded ? 40 : 6;
        if (f->vx > 0) f->vx = f->vx > fr ? f->vx - fr : 0;
        else f->vx = f->vx < -fr ? f->vx + fr : 0;
        step_anim(f);
        if (--f->hitstun <= 0) to_free(f, c);
    }
    if (f->state != ST_ATTACK && f->state != ST_HITSTUN) step_anim(f);

    /* gravity and motion */
    if (!f->grounded) {
        /* terminal speed: double in hitstun, and a fast fall (already past it) keeps its 1.5x */
        const int maxv = f->state == ST_HITSTUN ? c->fall_v * 2 : f->vy > c->fall_v ? c->fall_v * 3 / 2 : c->fall_v;
        const int glide = f->state == ST_ATTACK && c->mv[f->move].lowgrav;
        f->vy += glide ? c->gravity / 4 : c->gravity;
        if (f->vy > maxv && !(f->state == ST_ATTACK && c->mv[f->move].vy0 > 0)) f->vy = maxv;   /* not a ground pound */
    }
    if (f->drop > 0) f->drop--;
    const int py = f->y;
    f->x += f->vx;
    f->y += f->vy;

    const stage_t *st = STAGE(v);
    /* solid platforms are blocks: walls at their sides, and a ceiling from below */
    for (int k = 0; k < st->nplat; k++) {
        const platform_t *p = &st->plat[k];
        if (!p->solid || f->x <= p->x0 * SUB || f->x >= p->x1 * SUB) continue;
        const int top = p->top * SUB, bottom = (p->top + SOLID_DEPTH) * SUB;
        if (f->y <= top + SUB || f->y - 40 * SUB >= bottom) continue;
        if (py <= top) continue;                       /* came down onto it: a landing below */
        if (py - 40 * SUB >= bottom && f->vy < 0) {    /* jumped into it from below */
            f->y = bottom + 40 * SUB;
            f->vy = 0;
        } else {
            f->x = (f->x - p->x0 * SUB < p->x1 * SUB - f->x) ? p->x0 * SUB : p->x1 * SUB;
            f->vx = 0;
        }
    }

    if (f->grounded) {
        const platform_t *p = &st->plat[f->grounded - 1];
        if (f->x < p->x0 * SUB || f->x > p->x1 * SUB) {   /* walked off */
            f->grounded = 0;
            f->jumps = c->air_jumps;
        } else {
            f->y = p->top * SUB;
            f->vy = 0;
        }
    } else if (f->vy >= 0) {
        for (int k = 0; k < st->nplat; k++) {
            const platform_t *p = &st->plat[k];
            if (f->x < p->x0 * SUB || f->x > p->x1 * SUB) continue;
            if (py > p->top * SUB || f->y < p->top * SUB) continue;
            if (!p->solid && f->drop > 0) continue;
            f->y = p->top * SUB;
            f->vy = 0;
            f->grounded = k + 1;
            f->jumps = c->air_jumps;
            f->air_dodged = 0;
            f->helpless = 0;
            f->ledge_used = 0;
            if (f->state == ST_DODGE && f->dodge == 3) { f->dodge = 0; to_free(f, c); }
            if (f->state == ST_ATTACK && f->air_move) { f->air_move = 0; to_free(f, c); }
            else if (f->state == ST_FREE) set_anim(f, &c->idle);
            break;
        }
    }
    if (!f->grounded && f->vy >= 0 && f->state == ST_FREE && !f->ledge_cd) grab_ledge(v, i);

    const int x = f->x / SUB, y = f->y / SUB;
    if (x < st->blast[0] || x > st->blast[2] || y > st->blast[3] || y < st->blast[1]) ko(v, i);
}

/* fighter j's body boxes, in world px */
static int body_boxes(const uint16_t *rom, const vs_fighter_t *t, int (*body)[4])
{
    const character_t *ct = &CHARS[t->ch];
    const int tx = t->x / SUB, ty = t->y / SUB;
    int nb = frame_boxes(rom, ct, t->frame, 0, tx, ty, t->facing, body, 4);
    if (!nb) {
        box_at(ct->body, tx, ty, t->facing, body[0]);
        nb = 1;
    }
    return nb;
}

static int can_be_hit(const vs_fighter_t *t)
{
    return alive(t) && t->state != ST_RESPAWN && !t->invuln && !t->intangible;
}

/* i's hit lands on j, coming from the side `facing` points away from; returns the hitlag */
static int land_hit(versus_t *v, int i, int j, int dmg, int bkb, int kbg, int angle, int facing)
{
    vs_fighter_t *t = &v->f[j];
    const character_t *ct = &CHARS[t->ch];
    if (t->state == ST_SHIELD) {   /* blocked: shield damage, a short stun, pushed back */
        t->shield -= dmg * SUB * 3 / 2;
        t->shield_stun = 4 + dmg / 2;
        t->vx = facing * (128 + dmg * 48);
        t->hitlag = 2 + dmg / 4;
        if (t->shield <= 0) shield_break(t, ct);
        return t->hitlag;
    }
    t->damage += dmg;
    if (t->damage > 999) t->damage = 999;
    const int s = bkb + KB_GROWTH * kbg * t->damage / ct->weight;
    t->vx = s * icos(angle) / 256 * facing;
    t->vy = -s * isin(angle) / 256;
    if (t->vy < 0) t->grounded = 0;
    t->hitstun = 6 + s / 48;
    t->hitlag = 3 + dmg / 3;
    t->state = ST_HITSTUN;
    t->t = 0;
    t->squat = 0;
    t->helpless = 0;
    t->ledge = 0;
    t->facing = -facing;
    t->last_hit_by = i;
    t->anim_first = -1;
    set_anim(t, s < 700 && t->grounded ? &ct->hurt : &ct->tumble);
    return t->hitlag;
}

/* attacks against bodies, after everyone has moved */
static void combat(versus_t *v, const uint16_t *rom)
{
    for (int i = 0; i < v->n; i++) {
        vs_fighter_t *a = &v->f[i];
        if (a->state != ST_ATTACK || a->hitlag) continue;
        const character_t *ca = &CHARS[a->ch];
        const move_t *m = &ca->mv[a->move];
        if (!m->dmg) continue;
        int hit[4][4];
        const int ax = a->x / SUB, ay = a->y / SUB;
        int nh = frame_boxes(rom, ca, a->frame, 1, ax, ay, a->facing, hit, 4);
        if (!nh && (m->box[0] || m->box[2]) && a->frame >= m->act0 && a->frame <= m->act1) {
            box_at(m->box, ax, ay, a->facing, hit[0]);
            nh = 1;
        }
        if (!nh) continue;
        for (int j = 0; j < v->n; j++) {
            vs_fighter_t *t = &v->f[j];
            if (j == i || !can_be_hit(t) || (a->hit_mask >> j & 1)) continue;
            int body[4][4];
            const int nb = body_boxes(rom, t, body);
            int touch = 0;
            for (int p = 0; p < nh && !touch; p++)
                for (int q = 0; q < nb && !touch; q++) touch = overlap(hit[p], body[q]);
            if (!touch) continue;
            a->hit_mask |= 1 << j;
            a->hitlag = land_hit(v, i, j, m->dmg, m->bkb, m->kbg, m->angle, a->facing);
        }
    }
}

/* projectiles: fly, fall, run out, and hit */
static void projectiles(versus_t *v, const uint16_t *rom)
{
    for (int k = 0; k < VS_PROJ; k++) {
        vs_proj_t *p = &v->proj[k];
        if (!p->kind) continue;
        const proj_def_t *d = &PROJS[p->kind - 1];
        const vs_fighter_t *o = &v->f[p->owner];
        if (++p->t > d->life || (d->follow && (o->state == ST_HITSTUN || !alive(o)))) { p->kind = 0; continue; }
        if (d->follow) {
            p->x = o->x + d->dx * p->facing * SUB;
            p->y = o->y + d->dy * SUB;
        } else {
            p->x += p->vx;
            p->y += p->vy;
            p->vy += d->grav;
        }
        const int x = p->x / SUB, y = p->y / SUB;
        const stage_t *st = STAGE(v);
        if (x < st->blast[0] || x > st->blast[2] || y > st->blast[3]) { p->kind = 0; continue; }
        if (d->grav && p->vy > 0)   /* a thrown bomb goes off on whatever it lands on */
            for (int q = 0; q < st->nplat; q++)
                if (x >= st->plat[q].x0 && x <= st->plat[q].x1 && y >= st->plat[q].top && y - p->vy / SUB < st->plat[q].top)
                    p->t = d->life - 6;   /* lingers a moment where it landed */
        int box[4];
        box_at(d->box, x, y, p->facing, box);
        for (int j = 0; j < v->n; j++) {
            if (j == p->owner || !can_be_hit(&v->f[j]) || (p->hit_mask >> j & 1)) continue;
            int body[4][4];
            const int nb = body_boxes(rom, &v->f[j], body);
            int touch = 0;
            for (int q = 0; q < nb && !touch; q++) touch = overlap(box, body[q]);
            if (!touch) continue;
            p->hit_mask |= 1 << j;
            land_hit(v, p->owner, j, d->dmg, d->bkb, d->kbg, d->angle, p->vx < 0 || (!p->vx && p->facing < 0) ? -1 : 1);
            if (!d->follow) { p->kind = 0; break; }
        }
    }
}

void versus_rematch(versus_t *v, const uint16_t *rom, int select)
{
    uint32_t bots = 0;
    for (int i = 0; i < v->n; i++) bots |= (uint32_t)v->f[i].bot << i;
    const uint32_t frame = v->frame, seed = rnd(v);
    const int32_t matches = v->matches, keep_select = v->select, stage = v->stage;
    int32_t chars[VS_MAX];
    for (int i = 0; i < VS_MAX; i++) chars[i] = v->f[i].ch;
    versus_init(v, rom, v->n, bots, v->stocks, seed, select);
    v->stage = stage;
    for (int i = 0; i < v->n; i++) versus_set_char(v, i, chars[i]);
    update_camera(v, 1);
    v->frame = frame;
    v->matches = matches;
    v->select = keep_select;
    for (int i = 0; i < v->n; i++) v->f[i].prev = 0xFFFF;   /* no press carries over */
}

static void add_fighter(versus_t *v, const uint16_t *rom)
{
    int roster[16];
    const int nr = versus_roster(rom, roster);
    vs_fighter_t *g = &v->f[v->n];
    memset(g, 0, sizeof(*g));
    g->ch = roster[v->n % nr];
    g->bot = 1;
    g->stocks = v->stocks;
    g->last_hit_by = -1;
    v->bot_mask |= 1u << v->n;
    v->n++;
    for (int k = 0; k < v->n; k++) spawn(v, k);
}

/* the camera follows the middle of the fighters in play, eased (snap: at once) */
static void update_camera(versus_t *v, int snap)
{
    const stage_t *st = STAGE(v);
    int x0 = 1 << 30, x1 = -(1 << 30), y0 = 1 << 30, y1 = -(1 << 30);
    for (int i = 0; i < v->n; i++) {
        const vs_fighter_t *f = &v->f[i];
        if (!alive(f)) continue;
        const int x = f->x / SUB, y = f->y / SUB;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x0 > x1) { x0 = x1 = st->respawn_x; y0 = y1 = st->spawn_y; }
    int tx = (x0 + x1) / 2 - PPU_W / 2, ty = (y0 + y1) / 2 - 150;
    if (tx < st->cam_x0) tx = st->cam_x0;
    if (tx > st->cam_x1) tx = st->cam_x1;
    if (ty < st->cam_y0) ty = st->cam_y0;
    if (ty > st->cam_y1) ty = st->cam_y1;
    if (snap) { v->cam_x = tx * SUB; v->cam_y = ty * SUB; return; }
    v->cam_x += (tx * SUB - v->cam_x) / 8;
    v->cam_y += (ty * SUB - v->cam_y) / 8;
}

void versus_set_stage(versus_t *v, int stage)
{
    v->stage = ((stage % NSTAGES) + NSTAGES) % NSTAGES;
    for (int i = 0; i < v->n; i++) spawn(v, i);
    update_camera(v, 1);
}

int versus_stage_count(void) { return NSTAGES; }
const char *versus_stage_name(int stage) { return stage >= 0 && stage < NSTAGES ? STAGES[stage].name : "?"; }

int versus_take_slot(versus_t *v, const uint16_t *rom)
{
    for (int i = 1; i < v->n; i++)
        if (v->f[i].bot) {
            v->f[i].bot = 0;
            v->f[i].ready = 0;
            v->bot_mask &= ~(1u << i);
            return i;
        }
    if (v->phase != VS_SELECT || v->n >= VS_MAX) return -1;
    add_fighter(v, rom);
    v->f[v->n - 1].bot = 0;
    v->bot_mask &= ~(1u << (v->n - 1));
    return v->n - 1;
}

void versus_release_slot(versus_t *v, int slot)
{
    if (slot <= 0 || slot >= v->n) return;
    v->f[slot].bot = 1;
    v->f[slot].ready = 0;
    v->bot_mask |= 1u << slot;
}

/* the character select screen: the fighters stand in their places, the humans pick */
static void select_step(versus_t *v, const uint16_t *rom, const uint16_t *b)
{
    int roster[16];
    const int nr = versus_roster(rom, roster);
    int humans = 0, ready = 0;
    for (int i = 0; i < v->n; i++) {
        vs_fighter_t *f = &v->f[i];
        const uint16_t pressed = b[i] & ~f->prev;
        f->prev = b[i];
        step_anim(f);
        if (f->bot) continue;
        humans++;
        if (!f->ready) {
            const int d = (pressed & VS_RIGHT ? 1 : 0) - (pressed & VS_LEFT ? 1 : 0);
            if (d) {
                int k = 0;
                while (k < nr && roster[k] != f->ch) k++;
                versus_set_char(v, i, roster[((k + d) % nr + nr) % nr]);
            }
            if (pressed & (VS_ATTACK | VS_JUMP)) f->ready = 1;
            else if (pressed & VS_SPECIAL) v->quit = 1;   /* back out of the select screen */
            if (i == 0 && (pressed & VS_UP) && v->n < VS_MAX) add_fighter(v, rom);   /* one more CPU */
            if (i == 0 && (pressed & VS_BLOCK)) versus_set_stage(v, v->stage + 1);    /* the next stage */
            if (i == 0 && (pressed & VS_DOWN) && v->n > 2 && v->f[v->n - 1].bot) {   /* one fewer */
                v->n--;
                v->f[v->n].ch = -1;
                v->f[v->n].state = ST_OUT;
                for (int k = 0; k < v->n; k++) spawn(v, k);
            }
        } else if (pressed & VS_SPECIAL) f->ready = 0;
        ready += f->ready;
    }
    if (v->phase_t > 20 && ready == humans) {
        for (int i = 0; i < v->n; i++) {
            v->f[i].stocks = v->stocks;
            v->f[i].ready = 0;
            v->f[i].prev = 0xFFFF;   /* the confirming press doesn't carry into the fight */
            spawn(v, i);
        }
        v->phase = VS_COUNTDOWN;
        v->phase_t = 0;
    }
}

void versus_step(versus_t *v, const uint16_t *rom, const uint16_t *buttons)
{
    uint16_t b[VS_MAX] = { 0 };
    for (int i = 0; i < v->n; i++) {
        vs_fighter_t *f = &v->f[i];
        if (f->ch < 0) continue;
        b[i] = f->bot ? (v->phase == VS_FIGHT && alive(f) ? bot_input(v, i) : 0) : buttons[i];
    }
    v->frame++;
    v->phase_t++;
    if (v->phase == VS_SELECT) {
        select_step(v, rom, b);
        return;
    }

    if (v->phase == VS_GAME_OVER) {
        int humans = 0, again = 0;
        for (int i = 0; i < v->n; i++) {
            if (v->f[i].bot) continue;
            humans++;
            if (v->phase_t > GAME_OVER_WAIT && (b[i] & ~v->f[i].prev & (VS_ATTACK | VS_MENU | VS_JUMP))) again = 1;
            v->f[i].prev = b[i];
        }
        if (!humans && v->phase_t > GAME_OVER_WAIT * 2) again = 1;   /* bots only: start again by themselves */
        if (again) {
            versus_rematch(v, rom, v->select && humans);
            return;
        }
    }
    const int frozen = v->phase == VS_COUNTDOWN;
    if (frozen && v->phase_t >= COUNTDOWN) {
        v->phase = VS_FIGHT;
        v->phase_t = 0;
    }

    for (int i = 0; i < v->n; i++) {
        vs_fighter_t *f = &v->f[i];
        if (f->ch < 0 || f->state == ST_OUT) continue;
        const uint16_t in = frozen ? 0 : b[i];
        const uint16_t pressed = in & ~f->prev;
        f->prev = in;
        if (f->invuln > 0 && f->state != ST_RESPAWN) f->invuln--;
        if (f->hitlag > 0) {
            f->hitlag--;
            continue;
        }
        if (frozen) {
            step_anim(f);
            continue;
        }
        physics(v, i, in, pressed);
    }
    if (!frozen) {
        combat(v, rom);
        projectiles(v, rom);
    }

    update_camera(v, 0);
    if (v->phase == VS_FIGHT) {
        int left = 0, who = -1;
        for (int i = 0; i < v->n; i++)
            if (v->f[i].ch >= 0 && v->f[i].stocks > 0) { left++; who = i; }
        if (left <= 1) {
            v->phase = VS_GAME_OVER;
            v->phase_t = 0;
            v->matches++;
            v->winner = left ? who : -1;
        }
    }
}

/* ---- drawing ---- */

/* a page of the stage's scene (page 2: slot 2, page 1: slot 3) into the tilemap and attribute
 * RAM at map/attr: the 64x32-tile ring round the camera (cx, cy). Page 1 repeats its map. */
static void draw_map(spg_t *m, const stage_t *st, int slot, uint16_t map, uint16_t attr, int cx, int cy)
{
    const uint16_t *rom = m->rom;
    const uint32_t rec = SCENES + 1 + st->scene * SCENE_REC;
    const uint32_t a = rom[rec + 2 * slot] | (uint32_t)rom[rec + 2 * slot + 1] << 16;
    const uint32_t tiles = rom[rec] | (uint32_t)rom[rec + 1] << 16;
    const uint16_t tile_base = (uint16_t)(tiles / 16);
    if (!a) {   /* no such page */
        memset(&m->ram[map], 0, 64 * 32 * sizeof(uint16_t));
        return;
    }
    const int w = rom[a], h = rom[a + 1], n = rom[a + 2], repeat = slot == 3;
    const uint32_t grid = a + 3 + n;
    const int tx0 = cx >> 3, ty0 = cy >> 3;
    for (int ty = ty0; ty < ty0 + 32; ty++)
        for (int tx = tx0; tx < tx0 + 64; tx++) {
            uint16_t t = 0, at = 0;
            int bx = tx >> 2, by = ty >> 2;
            if (repeat) { bx = (bx % w + w) % w; by = (by % h + h) % h; }
            int ok = tx >= 0 && ty >= 0 && bx < w && by < h;
            for (int k = 0; ok && slot == 2 && k < st->ncut; k++)
                if (bx >= st->cut[k].bx0 && bx <= st->cut[k].bx1 && by >= st->cut[k].by0 && by <= st->cut[k].by1) ok = 0;
            if (ok) {
                const uint16_t g = rom[grid + by * (w / 2) + bx / 2];
                const uint16_t e = rom[a + 3 + ((bx & 1 ? g >> 8 : g) & 0xFF)];
                t = (uint16_t)(rom[TILE_POOL + e + (ty & 3) * 4 + (tx & 3)] + tile_base);
                const uint16_t aw = rom[ATTR_POOL + e / 2 + (ty & 3) * 2 + (tx & 3) / 2];
                at = (tx & 1 ? aw >> 8 : aw) & 0xFF;
            }
            const int r = (ty & 31) * 64 + (tx & 63);
            m->ram[map + r] = t;
            uint16_t *aw = &m->ram[attr + r / 2];
            *aw = r & 1 ? (uint16_t)((*aw & 0x00FF) | at << 8) : (uint16_t)((*aw & 0xFF00) | at);
        }
}

static void draw_stage(spg_t *m, const stage_t *st, int cx, int cy)
{
    uint16_t *vr = m->vregs;
    memset(vr, 0, 0x100 * sizeof(uint16_t));
    /* what Street Party runs with: two 8x8 4bpp tile pages, sprites on, centred sprite coordinates.
     * Page 1 (the room behind) scrolls at half speed. */
    vr[0x10] = (uint16_t)(cx / 2); vr[0x11] = (uint16_t)(cy / 2); vr[0x12] = 0x0001; vr[0x13] = 0x0108;
    vr[0x14] = 0x088E; vr[0x15] = 0x108E;
    vr[0x16] = (uint16_t)cx; vr[0x17] = (uint16_t)cy; vr[0x18] = 0x1001; vr[0x19] = 0x0108;
    vr[0x1A] = 0x148E; vr[0x1B] = 0x1C8E;
    vr[0x29] = 0x0004; vr[0x2A] = 0x0002;
    vr[0x36] = vr[0x37] = 0xFFFF;
    vr[0x3C] = 0x0020;
    vr[0x42] = 0x0001;
    draw_map(m, st, 3, 0x088E, 0x108E, cx / 2, cy / 2);
    draw_map(m, st, 2, 0x148E, 0x1C8E, cx, cy);
    const uint16_t *rom = m->rom;
    const uint32_t rec = SCENES + 1 + st->scene * SCENE_REC;
    const uint32_t pal = rom[rec + 2] | (uint32_t)rom[rec + 3] << 16;
    for (int i = 0; i < 128; i++) m->vram[0x200 + i] = rom[pal + i] | (i % 16 ? 0 : 0x8000);
    for (int i = 128; i < 256; i++) m->vram[0x200 + i] = 0x8000;
}

/* RGB555 c moved k/16 of the way to (r, g, b) */
static uint16_t tint(uint16_t c, int r, int g, int b, int k)
{
    const int cr = c >> 10 & 31, cg = c >> 5 & 31, cb = c & 31;
    return (uint16_t)((c & 0x8000) | (cr + (r - cr) * k / 16) << 10 | (cg + (g - cg) * k / 16) << 5 | (cb + (b - cb) * k / 16));
}

/* a frame record's pieces into sprite RAM from sprite n on, at (fx, fy) */
static int draw_frame(spg_t *m, uint32_t rec, int fx, int fy, int facing, uint16_t attr, uint16_t strip_lo,
                      uint16_t strip_hi, int n)
{
    const uint16_t *rom = m->rom;
    const int pieces = rom[rec], k = rom[rec + 3];
    int x = 0, y = 0;
    for (int p = 0; p < pieces && n < 256; p++) {
        const int16_t *w = (const int16_t *)&rom[rec + 7 + k + 3 * p];
        x += w[0];
        y += w[1];
        const uint16_t tile = (uint16_t)(w[2] + TILE_BASE);
        if (strip_lo && tile >= strip_lo && tile <= strip_hi) continue;
        const int cx = fx + (facing > 0 ? x : -x), cy = fy + y;
        uint16_t *s = &m->vram[0x300 + 4 * n++];
        s[0] = tile;
        s[1] = (uint16_t)(cx - 160);
        s[2] = (uint16_t)(128 - cy);
        s[3] = attr | (facing < 0 ? 0x0004 : 0);
    }
    return n;
}

static int draw_fighter(spg_t *m, const vs_fighter_t *f, int slot, int n, uint32_t frame, int cx, int cy)
{
    const uint16_t *rom = m->rom;
    const character_t *c = &CHARS[f->ch];
    /* see-through while a dodge makes it intangible (the PPU's blend) */
    const uint16_t attr = 0x2051 | (uint16_t)((8 + slot) << 8) | (f->intangible ? 0x4000 : 0);
    n = draw_frame(m, frame_rec(rom, c, f->frame), f->x / SUB - cx, f->y / SUB - cy, f->facing, attr, c->strip_lo,
                   c->strip_hi, n);
    for (int i = 0; i < 16; i++) {
        uint16_t col = rom[(c->palettes ? c->palettes : PALETTES) + 16 * c->pal + i];
        if (i && f->state == ST_SHIELD)   /* the shield: blue-white, red when it is about to break */
            col = f->shield < SHIELD_MAX / 3 ? tint(col, 31, 10, 8, 8) : tint(col, 20, 26, 31, 9);
        else if (i && f->state == ST_DIZZY && (frame >> 3 & 1)) col = tint(col, 31, 31, 8, 8);
        else if (i && f->helpless) col = tint(col, 4, 4, 8, 6);   /* darker until it lands */
        m->vram[0x200 + (8 + slot) * 16 + i] = col;
    }
    return n;
}

static int draw_proj(spg_t *m, const vs_proj_t *p, int bank, int n, int cx, int cy)
{
    const proj_def_t *d = &PROJS[p->kind - 1];
    const uint32_t rec = BANK + m->rom[BANK + d->header + HEADER_FRAMES + d->f0 + (p->t / d->ticks) % (d->f1 - d->f0 + 1)];
    n = draw_frame(m, rec, p->x / SUB - cx, p->y / SUB - cy, p->facing, 0x3051 | (uint16_t)(bank << 8), 0, 0, n);
    for (int i = 0; i < 16; i++) m->vram[0x200 + bank * 16 + i] = m->rom[PALETTES + 16 * d->pal + i];
    return n;
}

/* a line into the finished frame (the grapnel's) */
static void draw_line(uint32_t *fb, int x0, int y0, int x1, int y1, uint32_t colour)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (int k = 0; k < 2048; k++) {
        if (x0 >= 0 && x0 < PPU_W && y0 >= 0 && y0 < PPU_H) fb[y0 * PPU_W + x0] = colour;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

void versus_present(const versus_t *v, spg_t *m)
{
    const stage_t *st = STAGE(v);
    const int cx = v->cam_x / SUB, cy = v->cam_y / SUB;
    draw_stage(m, st, cx, cy);
    memset(&m->vram[0x300], 0, 0x400 * sizeof(uint16_t));
    int n = 0;
    for (int i = 0; i < v->n; i++) {
        const vs_fighter_t *f = &v->f[i];
        if (!alive(f)) continue;
        if (f->invuln && (v->frame >> 2 & 1)) continue;   /* blinks while invulnerable */
        n = draw_fighter(m, f, i, n, v->frame, cx, cy);
    }
    /* projectiles: each kind on screen gets a palette bank the stage leaves free */
    int bank_of[NPROJ + 1] = { 0 }, next_bank = 0, last = 0;
    for (int k = 0; k < VS_PROJ; k++) {
        const vs_proj_t *p = &v->proj[k];
        if (!p->kind) continue;
        if (!bank_of[p->kind]) {
            while (next_bank < 8 && !(st->free_banks >> next_bank & 1)) next_bank++;
            bank_of[p->kind] = next_bank < 8 ? next_bank++ : last;
            last = bank_of[p->kind];
        }
        n = draw_proj(m, p, bank_of[p->kind], n, cx, cy);
    }
    spg_render(m);
    for (int i = 0; i < v->n; i++) {   /* grapnel lines */
        const vs_fighter_t *f = &v->f[i];
        if (f->state != ST_GRAPPLE) continue;
        const int hx = f->x / SUB - cx + f->facing * 6, hy = f->y / SUB - cy - 34;
        int ax = f->grapple_x - cx, ay = f->grapple_y - cy;
        if (f->t < GRAPNEL_FLY) { ax = hx + (ax - hx) * f->t / GRAPNEL_FLY; ay = hy + (ay - hy) * f->t / GRAPNEL_FLY; }
        draw_line(m->framebuffer, hx, hy, ax, ay, 0x101018);
        draw_line(m->framebuffer, hx, hy - 1, ax, ay - 1, 0x8a8a96);
    }

    /* HUD: name, percent and stocks per player along the top (the floor is at the bottom), a tag
     * over each fighter */
    char s[64];
    const int cols = v->n > 4 ? 4 : v->n;
    for (int i = 0; i < v->n; i++) {
        const vs_fighter_t *f = &v->f[i];
        const int cw = 320 / cols, x = (i % cols) * cw + 4, y = 2 + (i / cols) * 22;
        snprintf(s, sizeof(s), "P%d %s", i + 1, CHARS[f->ch].name);
        osd_text(m->framebuffer, PPU_W, PPU_H, x, y, 1, s);
        if (v->phase == VS_SELECT) {
            snprintf(s, sizeof(s), "%s", f->bot ? "CPU" : f->ready ? "READY" : "< PICK >");
        } else if (f->stocks > 0) {
            char st[16] = "";
            for (int k = 0; k < f->stocks && k < 9; k++) st[k] = 'o';
            snprintf(s, sizeof(s), "%3d%% %s%s", f->damage, st, f->bot ? " CPU" : "");
        } else snprintf(s, sizeof(s), "OUT%s", f->bot ? " CPU" : "");
        osd_text(m->framebuffer, PPU_W, PPU_H, x, y + 10, 1, s);
        if (alive(f)) {   /* a tag over the fighter, or at the screen's edge when it is off the screen */
            int tx = f->x / SUB - cx - 7, ty = f->y / SUB - cy - CHARS[f->ch].tall - 14;
            const char *edge = "";
            if (tx < 2) { tx = 2; edge = "<"; } else if (tx > PPU_W - 30) { tx = PPU_W - 30; edge = ">"; }
            if (ty < 46) { ty = 46; edge = "^"; } else if (ty > PPU_H - 14) { ty = PPU_H - 14; edge = "v"; }
            snprintf(s, sizeof(s), "%sP%d", edge, i + 1);
            osd_text(m->framebuffer, PPU_W, PPU_H, tx, ty, 1, s);
        }
    }
    if (v->phase == VS_SELECT) {
        const int rows = (v->n + 3) / 4;
        osd_text(m->framebuffer, PPU_W, PPU_H, 160 - 19 * 6, 4 + rows * 22, 2, "CHOOSE YOUR FIGHTER");
        osd_text(m->framebuffer, PPU_W, PPU_H, 160 - 26 * 3, 26 + rows * 22, 1, "LEFT/RIGHT PICK   B READY");
        snprintf(s, sizeof(s), "P1 UP/DOWN: %d FIGHTERS   BLOCK: %s", v->n, st->name);
        osd_text(m->framebuffer, PPU_W, PPU_H, 160 - (int)strlen(s) * 3, 38 + rows * 22, 1, s);
    } else if (v->phase == VS_COUNTDOWN) {
        const int left = (COUNTDOWN - v->phase_t + 49) / 50;
        snprintf(s, sizeof(s), "%d", left > 0 ? left : 1);
        osd_text(m->framebuffer, PPU_W, PPU_H, 148, 60, 4, s);
    } else if (v->phase == VS_FIGHT && v->phase_t < 40) {
        osd_text(m->framebuffer, PPU_W, PPU_H, 128, 60, 4, "GO!");
    } else if (v->phase == VS_GAME_OVER) {
        if (v->winner >= 0) snprintf(s, sizeof(s), "P%d %s WINS!", v->winner + 1, CHARS[v->f[v->winner].ch].name);
        else snprintf(s, sizeof(s), "NO CONTEST");
        osd_text(m->framebuffer, PPU_W, PPU_H, 160 - (int)strlen(s) * 6, 50, 2, s);
        if (v->phase_t > GAME_OVER_WAIT)
            osd_text(m->framebuffer, PPU_W, PPU_H, 160 - 20 * 3, 80, 1, "B: play again");
    }
}

/* ---- state ---- */

uint64_t versus_hash(const versus_t *v)
{
    const uint8_t *p = (const uint8_t *)v;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof(*v); i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int versus_save(const versus_t *v, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    const int ok = fwrite(v, sizeof(*v), 1, f) == 1;
    return fclose(f) == 0 && ok;
}

int versus_load(versus_t *v, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    versus_t t;
    const int ok = fread(&t, sizeof(t), 1, f) == 1;
    fclose(f);
    if (!ok || t.magic != 0x56535631 || t.size != sizeof(t)) {
        fprintf(stderr, "%s: not a versus state of this build\n", path);
        return 0;
    }
    *v = t;
    return 1;
}
